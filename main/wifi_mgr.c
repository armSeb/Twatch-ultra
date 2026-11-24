/* wifi_mgr.c — voir wifi_mgr.h */
#include "wifi_mgr.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "i18n.h"
#include "net.h"
#include "ble.h"
#include "power.h"
#include "settings.h"
#include "webrx_cfg.h"
#include "app_events.h"
#include "task_psram.h"
#include "display.h"

static const char *TAG = "wifi";

#define BIT_IP   BIT0
#define BIT_DISC BIT1

#define HOSTNAME        "twatch-ultra"
#define LOW_BATT_PCT    30
#define CONNECT_TMO_MS  15000
#define RSSI_PERIOD_MS  10000
/* sans réseau trouvé, la radio est coupée entre deux essais : une station
 * non connectée ne peut pas dormir et garde le récepteur allumé */
static const uint16_t BACKOFF_S[] = { 5, 10, 30, 60, 120, 300 };

static SemaphoreHandle_t s_drv;          /* opérations sur le pilote (tâche wifi ↔ tâche app) */
static SemaphoreHandle_t s_cfg_mx;       /* s_cfg / s_pick */
static EventGroupHandle_t s_evg;
static TaskHandle_t s_task;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static wifi_status_t s_st;               /* sous s_mux */

static volatile uint32_t s_holders;      /* écrit par la tâche app seule */
static uint32_t s_perf_hold;             /* tenants qui veulent WIFI_ACQ_PERF */
static volatile uint32_t s_perf;         /* WIFI_PERF_* (toute tâche, sous s_mux) */
static volatile bool s_ps_dirty;
static bool s_inited, s_started;         /* pilote : sous s_drv */
static volatile bool s_blocked;          /* arrêt avant la mise à jour par point d'accès */
static volatile bool s_abort;            /* l'essai en cours doit s'arrêter */
static volatile bool s_kick, s_reload;
static uint32_t s_ble_pauser;            /* tenant qui a suspendu le Bluetooth */
static volatile int s_reason;
static bool s_scan_radio;                /* analyseur : radio voulue allumée */

static wrx_cfg_t *s_cfg, *s_tcfg;        /* PSRAM : réseaux (app) et copie de la tâche */
static uint8_t s_pick;                   /* 0 = automatique, k = réseau k-1 seul */
static char s_pick_ssid[33];             /* son nom : suit le réseau si la liste change d'ordre */

/* ------------------------------------------------------------------ */
/* État                                                                 */
/* ------------------------------------------------------------------ */
static void post_state(void) { app_post_simple(EV_WIFI, WIFI_EV_STATE); }

static void set_state(wifi_state_t st, const char *msg, const char *detail)
{
    bool changed;
    portENTER_CRITICAL(&s_mux);
    changed = s_st.state != st || s_st.msg != msg || strncmp(s_st.detail, detail ? detail : "", sizeof s_st.detail);
    s_st.state = st;
    s_st.msg = msg;
    strlcpy(s_st.detail, detail ? detail : "", sizeof s_st.detail);
    if (st != WIFI_CONNECTED) { s_st.ip[0] = 0; s_st.rssi = 0; }
    if (st != WIFI_FAILED) s_st.retry_at_us = 0;
    if (st == WIFI_OFF || st == WIFI_IDLE) s_st.ssid[0] = 0;
    s_st.holders = s_holders;
    s_st.ble_paused = s_ble_pauser != 0;
    portEXIT_CRITICAL(&s_mux);
    if (changed) post_state();
}

static wifi_state_t cur_state(void)
{
    portENTER_CRITICAL(&s_mux);
    wifi_state_t st = s_st.state;
    portEXIT_CRITICAL(&s_mux);
    return st;
}

static void sync_holders(void)
{
    portENTER_CRITICAL(&s_mux);
    s_st.holders = s_holders;
    s_st.ble_paused = s_ble_pauser != 0;
    portEXIT_CRITICAL(&s_mux);
    post_state();
}

void wifi_get_status(wifi_status_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_st;
    portEXIT_CRITICAL(&s_mux);
}

bool wifi_is_up(void) { return cur_state() == WIFI_CONNECTED; }

uint32_t wifi_gen(void)
{
    portENTER_CRITICAL(&s_mux);
    uint32_t g = s_st.gen;
    portEXIT_CRITICAL(&s_mux);
    return g;
}

bool wifi_lost(uint32_t gen) { return !wifi_is_up() || wifi_gen() != gen; }

int wifi_rssi(void)
{
    portENTER_CRITICAL(&s_mux);
    int r = s_st.rssi;
    portEXIT_CRITICAL(&s_mux);
    return r;
}

bool wifi_ip(char out[16])
{
    portENTER_CRITICAL(&s_mux);
    bool up = s_st.state == WIFI_CONNECTED && s_st.ip[0];
    memcpy(out, s_st.ip, 16);
    portEXIT_CRITICAL(&s_mux);
    if (!up) out[0] = 0;
    return up;
}

bool wifi_held(uint32_t holder) { return (s_holders & holder) != 0; }
bool wifi_ble_paused(void) { return s_ble_pauser != 0; }
bool wifi_user_on(void) { return wifi_held(WIFI_HOLD_USER); }

static void kick_task(void) { if (s_task) xTaskNotifyGive(s_task); }

void wifi_retry(void)
{
    s_kick = true;
    kick_task();
}

void wifi_refresh(void) { kick_task(); }

void wifi_request_reload(void) { app_post_simple(EV_WIFI, WIFI_EV_RELOAD); }

void wifi_perf(uint32_t who, bool on)
{
    portENTER_CRITICAL(&s_mux);
    uint32_t old = s_perf;
    s_perf = on ? (old | who) : (old & ~who);
    bool ch = s_perf != old;
    portEXIT_CRITICAL(&s_mux);
    if (ch) { s_ps_dirty = true; kick_task(); }
}

/* ------------------------------------------------------------------ */
/* Événements du pilote (boucle d'événements par défaut)                */
/* ------------------------------------------------------------------ */
static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_reason = ((wifi_event_sta_disconnected_t *)data)->reason;
        xEventGroupClearBits(s_evg, BIT_IP);
        xEventGroupSetBits(s_evg, BIT_DISC);
        kick_task();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        portENTER_CRITICAL(&s_mux);
        snprintf(s_st.ip, sizeof s_st.ip, IPSTR, IP2STR(&e->ip_info.ip));
        portEXIT_CRITICAL(&s_mux);
        xEventGroupSetBits(s_evg, BIT_IP);
        kick_task();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        xEventGroupClearBits(s_evg, BIT_IP);
        xEventGroupSetBits(s_evg, BIT_DISC);
        kick_task();
    }
}

/* ------------------------------------------------------------------ */
/* Pilote (sous s_drv)                                                  */
/* ------------------------------------------------------------------ */
/* puissance d'émission (quarts de dBm) : 8, 13, 17, 20 dBm */
static const int8_t PWR_QDBM[4] = { 32, 52, 68, 80 };

int8_t wifi_power_qdbm(void)
{
    uint8_t i = settings_get()->wifi_pwr;
    return PWR_QDBM[i <= 3 ? i : 3];
}

static void apply_power_locked(void)
{
    if (s_started) esp_wifi_set_max_tx_power(wifi_power_qdbm());
}

static void apply_ps_locked(void)
{
    s_ps_dirty = false;
    if (!s_started) return;
    bool perf = s_perf_hold || s_perf;
    /* Bluetooth actif : la coexistence impose l'économie du modem */
    if (perf && esp_wifi_set_ps(WIFI_PS_NONE) == ESP_OK) return;
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
}

static bool radio_start_locked(void)
{
    if (s_started) return true;
    esp_wifi_set_mode(WIFI_MODE_STA);
    if (esp_wifi_start() != ESP_OK) return false;
    s_started = true;
    apply_ps_locked();
    apply_power_locked();
    return true;
}

static void radio_stop_locked(void)
{
    if (!s_started) return;
    esp_wifi_disconnect();
    esp_wifi_stop();
    s_started = false;
    xEventGroupClearBits(s_evg, BIT_IP | BIT_DISC);
}

/* Light sleep + Bluetooth : avec les deux radios, la puce en light sleep
 * manque les balises du point d'accès (la coexistence donne les créneaux au
 * Bluetooth au réveil) ; la station rate ses trames pendant des secondes et
 * l'AP les lui rend en rafale (ping de 1 à 17 s, « host unreachable »).
 * Seb 0.2.21. Tant que la radio Wi-Fi tourne avec le Bluetooth actif, pas de
 * light sleep : le modem Wi-Fi garde son économie (réveil aux balises) et le
 * CPU descend toujours en fréquence. Sans Bluetooth, le light sleep reste. */
static bool s_ns_held;
static portMUX_TYPE s_ns_mux = portMUX_INITIALIZER_UNLOCKED;

static void sleep_policy(void)
{
    bool want = s_started && ble_running();
    portENTER_CRITICAL(&s_ns_mux);
    bool ch = want != s_ns_held;
    s_ns_held = want;
    portEXIT_CRITICAL(&s_ns_mux);
    ble_wifi_coex(s_started);
    if (ch) {
        power_hold_nosleep(want);
        ESP_LOGI(TAG, "light sleep %s (Wi-Fi + Bluetooth)", want ? "suspendu" : "permis");
    }
}

void wifi_ble_changed(void) { sleep_policy(); kick_task(); }

/* Bluetooth suspendu pour le débit : option « Couper le Bluetooth » tant que
 * le Wi-Fi est tenu, ou mise à jour reçue par le serveur web. Tâche app. */
static bool s_ble_pol;          /* Bluetooth arrêté par cette règle */
static bool s_web_ota;

static void ble_resume(void)
{
    if (!s_ble_pauser) return;
    s_ble_pauser = 0;
    if (!s_ble_pol && settings_get()->ble_enabled && !ble_running()) ble_start();
}

void wifi_ble_policy(void)
{
    bool want = s_web_ota || (settings_get()->wifi_noble && s_holders);
    if (want && !s_ble_pol && ble_running()) {
        ble_diag(s_web_ota ? "Wi-Fi : Bluetooth suspendu (mise à jour web)" : "Wi-Fi : Bluetooth suspendu (réglage)");
        ble_stop();
        s_ble_pol = !ble_running();
    } else if (!want && s_ble_pol) {
        s_ble_pol = false;
        if (!s_ble_pauser && settings_get()->ble_enabled && !ble_running()) ble_start();
    }
}

void wifi_web_ota(bool on)
{
    s_web_ota = on;
    wifi_ble_policy();
}

static esp_err_t driver_init_locked(uint32_t holder, bool may_pause_ble)
{
    if (s_inited) return ESP_OK;
    wifi_init_config_t wc = WIFI_INIT_CONFIG_DEFAULT();
    wc.nvs_enable = 0;                                  /* rien à mémoriser en flash */
    esp_err_t e = esp_wifi_init(&wc);
    if (e != ESP_OK && may_pause_ble && ble_running()) {
        /* pas assez de RAM interne à côté du Bluetooth : suspendu le temps
         * que l'app tienne le Wi-Fi */
        ESP_LOGW(TAG, "esp_wifi_init: %s, Bluetooth suspendu", esp_err_to_name(e));
        ble_diag("Wi-Fi : Bluetooth suspendu (RAM)");
        ble_stop();
        if (!ble_running()) {
            s_ble_pauser = holder;
            esp_wifi_deinit();                          /* au cas où l'échec ait laissé un état partiel */
            e = esp_wifi_init(&wc);
        }
    }
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_init: %s (interne libre %u, bloc %u)", esp_err_to_name(e),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        esp_wifi_deinit();
        ble_resume();
        portENTER_CRITICAL(&s_mux);
        s_st.no_mem = true;
        portEXIT_CRITICAL(&s_mux);
        return e;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_country_code("FR", false);            /* canaux 1 à 13 */
    s_inited = true;
    portENTER_CRITICAL(&s_mux);
    s_st.no_mem = false;
    portEXIT_CRITICAL(&s_mux);
    return ESP_OK;
}

static void driver_deinit_locked(void)
{
    if (!s_inited) return;
    radio_stop_locked();
    esp_wifi_deinit();
    s_inited = false;
}

/* ------------------------------------------------------------------ */
/* Connexion (tâche wifi, sous s_drv)                                   */
/* ------------------------------------------------------------------ */
static bool want_connect(void) { return (s_holders & WIFI_HOLD_CONNECT) && !s_blocked; }
static bool quit_attempt(void) { return s_abort || s_reload || !want_connect(); }

static EventBits_t wait_bits(EventBits_t bits, int ms)
{
    for (int t = 0; t < ms; t += 100) {
        EventBits_t b = xEventGroupWaitBits(s_evg, bits, pdFALSE, pdFALSE, pdMS_TO_TICKS(100));
        if ((b & bits) || quit_attempt()) return b;
    }
    return xEventGroupGetBits(s_evg);
}

static void ssid_set(const char *ssid)
{
    portENTER_CRITICAL(&s_mux);
    strlcpy(s_st.ssid, ssid, sizeof s_st.ssid);
    portEXIT_CRITICAL(&s_mux);
}

/* un essai complet : réseaux visibles dans l'ordre de préférence (ou le
 * seul réseau choisi) ; true = connecté. Échec : état FAILED renseigné. */
static bool attempt_locked(void)
{
    xSemaphoreTake(s_cfg_mx, portMAX_DELAY);
    memcpy(s_tcfg, s_cfg, sizeof *s_tcfg);
    int pick = s_pick;
    xSemaphoreGive(s_cfg_mx);
    const wrx_cfg_t *cfg = s_tcfg;
    if (pick && (pick > WRX_NETS || !cfg->ssid[pick - 1][0])) pick = 0;
    int nnets = 0;
    for (int k = 0; k < WRX_NETS; k++) nnets += cfg->ssid[k][0] != 0;
    if (!nnets) {
        set_state(WIFI_FAILED, N_("Aucun réseau enregistré"), "");
        return false;
    }
    if (!radio_start_locked()) {
        set_state(WIFI_FAILED, N_("Erreur Wi-Fi"), "");
        return false;
    }
    set_state(WIFI_CONNECTING, N_("Recherche du Wi-Fi…"), "");
    bool seen[WRX_NETS] = { 0 };
    int nseen = 0;
    wifi_scan_config_t sc = { .show_hidden = true };
    if (!ble_running()) {               /* avec le Bluetooth : durées par défaut (coexistence) */
        sc.scan_time.active.min = 40;
        sc.scan_time.active.max = 100;
    }
    if (esp_wifi_scan_start(&sc, true) == ESP_OK) {
        uint16_t n = 24;
        wifi_ap_record_t *rec = heap_caps_malloc(n * sizeof *rec, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (rec && esp_wifi_scan_get_ap_records(&n, rec) == ESP_OK) {
            for (int i = 0; i < n; i++)
                for (int k = 0; k < WRX_NETS; k++)
                    if (cfg->ssid[k][0] && !seen[k] && !strcmp((const char *)rec[i].ssid, cfg->ssid[k])) {
                        seen[k] = true;
                        nseen++;
                    }
        } else {
            esp_wifi_clear_ap_list();
        }
        free(rec);
    }
    if (quit_attempt()) return false;
    /* réseaux vus, dans l'ordre de préférence ; aucun vu : tous (réseau masqué) */
    int order[WRX_NETS], no = 0;
    for (int k = 0; k < WRX_NETS; k++) {
        if (!cfg->ssid[k][0] || (pick && k != pick - 1)) continue;
        if (seen[k] || !nseen) order[no++] = k;
    }
    bool bad_pass = false;
    int last_reason = 0;
    for (int j = 0; j < no && !quit_attempt(); j++) {
        int k = order[j];
        set_state(WIFI_CONNECTING, N_("Connexion au Wi-Fi"), cfg->ssid[k]);
        ssid_set(cfg->ssid[k]);
        wifi_config_t wc = { 0 };
        strlcpy((char *)wc.sta.ssid, cfg->ssid[k], sizeof wc.sta.ssid);
        strlcpy((char *)wc.sta.password, cfg->pass[k], sizeof wc.sta.password);
        wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
        wc.sta.threshold.authmode = cfg->pass[k][0] ? WIFI_AUTH_WEP : WIFI_AUTH_OPEN;
        wc.sta.pmf_cfg.capable = true;
        wc.sta.listen_interval = 3;                     /* modem sleep : réveil toutes les 3 balises */
        esp_wifi_set_config(WIFI_IF_STA, &wc);
        xEventGroupClearBits(s_evg, BIT_IP | BIT_DISC);
        s_reason = 0;
        esp_wifi_connect();
        EventBits_t b = wait_bits(BIT_IP | BIT_DISC, CONNECT_TMO_MS);
        if (b & BIT_IP) {
            wifi_ap_record_t ap;
            int rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
            portENTER_CRITICAL(&s_mux);
            s_st.gen++;
            s_st.rssi = rssi;
            portEXIT_CRITICAL(&s_mux);
            set_state(WIFI_CONNECTED, NULL, cfg->ssid[k]);
            ESP_LOGI(TAG, "connecté à « %s », %s", cfg->ssid[k], s_st.ip);
            return true;
        }
        int r = s_reason;
        last_reason = r;
        if (r == WIFI_REASON_AUTH_FAIL || r == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
            r == WIFI_REASON_HANDSHAKE_TIMEOUT || r == WIFI_REASON_MIC_FAILURE) bad_pass = true;
        ESP_LOGW(TAG, "« %s » : échec (raison %d)", cfg->ssid[k], r);
        ble_diag("wifi %.16s r%d int %u Ko", cfg->ssid[k], r,
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
        esp_wifi_disconnect();
        wait_bits(BIT_DISC, 500);
    }
    if (quit_attempt()) return false;
    if (bad_pass) set_state(WIFI_FAILED, N_("Mot de passe Wi-Fi refusé"), "");
    else if (!nseen || !no) set_state(WIFI_FAILED, N_("Aucun réseau connu à portée"), "");
    else {
        char d[32];
        if (last_reason) snprintf(d, sizeof d, "%d", last_reason);
        else d[0] = 0;
        set_state(WIFI_FAILED, N_("Connexion Wi-Fi impossible"), d);
    }
    return false;
}

static void drv_lock(void) { xSemaphoreTake(s_drv, portMAX_DELAY); }
static void drv_unlock(void) { xSemaphoreGive(s_drv); }

static void wifi_task(void *arg)
{
    int fails = 0;
    int64_t next_try = 0, next_rssi = 0;
    for (;;) {
        sleep_policy();
        TickType_t wait = portMAX_DELAY;
        wifi_state_t st = cur_state();
        int64_t now = esp_timer_get_time();
        if (!want_connect()) {
            fails = 0;
            next_try = 0;
            s_reload = s_kick = false;
            if (st != WIFI_OFF && st != WIFI_IDLE) {
                drv_lock();
                if (s_started) esp_wifi_disconnect();
                bool inited = s_inited;
                drv_unlock();
                set_state(inited ? WIFI_IDLE : WIFI_OFF, NULL, "");
            }
        } else if (st == WIFI_CONNECTED) {
            EventBits_t b = xEventGroupGetBits(s_evg);
            if (b & BIT_DISC) {
                ESP_LOGW(TAG, "connexion perdue (raison %d)", s_reason);
                wifi_status_t cs;
                wifi_get_status(&cs);
                set_state(WIFI_CONNECTING, N_("Wi-Fi perdu, reconnexion…"), cs.ssid);
                fails = 0;
                next_try = 0;
                continue;
            }
            if (s_reload) {
                s_reload = false;
                /* réseau choisi changé : on quitte l'actuel s'il ne convient plus */
                xSemaphoreTake(s_cfg_mx, portMAX_DELAY);
                int pick = s_pick;
                char want[33] = "";
                if (pick && pick <= WRX_NETS) strlcpy(want, s_cfg->ssid[pick - 1], sizeof want);
                bool known = false;
                wifi_status_t cs;
                wifi_get_status(&cs);
                for (int k = 0; k < WRX_NETS; k++) known |= s_cfg->ssid[k][0] && !strcmp(s_cfg->ssid[k], cs.ssid);
                xSemaphoreGive(s_cfg_mx);
                if (!known || (want[0] && strcmp(want, cs.ssid))) {
                    drv_lock();
                    esp_wifi_disconnect();
                    drv_unlock();
                    set_state(WIFI_CONNECTING, N_("Recherche du Wi-Fi…"), "");
                    next_try = 0;
                    continue;
                }
            }
            if (s_ps_dirty || now >= next_rssi) {
                drv_lock();
                if (s_ps_dirty) apply_ps_locked();
                if (now >= next_rssi && s_started) {
                    wifi_ap_record_t ap;
                    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                        portENTER_CRITICAL(&s_mux);
                        s_st.rssi = ap.rssi;
                        portEXIT_CRITICAL(&s_mux);
                    }
                    next_rssi = now + RSSI_PERIOD_MS * 1000LL;
                }
                drv_unlock();
            }
            /* signal relu pour l'affichage seulement : pas de réveil écran éteint */
            wait = display_is_on() ? pdMS_TO_TICKS(RSSI_PERIOD_MS) : portMAX_DELAY;
        } else {
            if (s_kick || s_reload) { s_kick = s_reload = false; next_try = 0; fails = 0; }
            if (now >= next_try) {
                drv_lock();
                bool ok = false;
                if (s_inited && want_connect() && !s_abort) {
                    ok = attempt_locked();
                    if (!ok && !quit_attempt()) {
                        int i = fails < (int)(sizeof BACKOFF_S / sizeof BACKOFF_S[0]) ? fails : (int)(sizeof BACKOFF_S / sizeof BACKOFF_S[0]) - 1;
                        fails++;
                        wifi_status_t cs;
                        wifi_get_status(&cs);
                        bool none = cs.msg && !strcmp(cs.msg, "Aucun réseau enregistré");
                        next_try = none ? INT64_MAX : esp_timer_get_time() + BACKOFF_S[i] * 1000000LL;
                        portENTER_CRITICAL(&s_mux);
                        s_st.retry_at_us = none ? 0 : next_try;
                        portEXIT_CRITICAL(&s_mux);
                        post_state();
                        /* longue attente : radio coupée, sauf si l'analyseur s'en sert */
                        if ((none || BACKOFF_S[i] >= 30) && !(s_holders & WIFI_HOLD_SCAN)) radio_stop_locked();
                    }
                }
                drv_unlock();
                if (ok) { fails = 0; next_rssi = esp_timer_get_time() + RSSI_PERIOD_MS * 1000LL; }
                continue;
            }
            int64_t ms = (next_try - now) / 1000;
            wait = ms > 600000 ? pdMS_TO_TICKS(600000) : pdMS_TO_TICKS(ms + 1);
        }
        ulTaskNotifyTake(pdTRUE, wait);
    }
}

/* ------------------------------------------------------------------ */
/* API (tâche app)                                                      */
/* ------------------------------------------------------------------ */
static bool ensure_base(void)
{
    if (s_task) return true;
    if (!s_drv) s_drv = xSemaphoreCreateMutex();
    if (!s_cfg_mx) s_cfg_mx = xSemaphoreCreateMutex();
    if (!s_evg) s_evg = xEventGroupCreate();
    if (!s_cfg) s_cfg = heap_caps_calloc(1, sizeof *s_cfg, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_tcfg) s_tcfg = heap_caps_calloc(1, sizeof *s_tcfg, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_drv || !s_cfg_mx || !s_evg || !s_cfg || !s_tcfg) return false;
    if (net_base_init() != ESP_OK) return false;
    esp_netif_t *sta = net_sta_netif();
    if (!sta) return false;
    esp_netif_set_hostname(sta, HOSTNAME);
    esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_event, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_LOST_IP, on_event, NULL, NULL);
    wifi_load_cfg();
    if (task_psram(wifi_task, "wifi", 4096, NULL, 4, &s_task, tskNO_AFFINITY) != pdPASS) {
        s_task = NULL;
        return false;
    }
    return true;
}

void wifi_load_cfg(void)
{
    if (!s_cfg_mx || !s_cfg) return;
    wrx_cfg_t *c = heap_caps_malloc(sizeof *c, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!c) return;
    wrx_cfg_load(c);
    memset(c->srv, 0, sizeof c->srv);                   /* seuls les réseaux servent ici */
    settings_t *st = settings_get();
    uint8_t pick = st->wifi_net <= WRX_NETS ? st->wifi_net : 0;
    if (pick && s_pick_ssid[0] && strcmp(c->ssid[pick - 1], s_pick_ssid)) {
        /* liste modifiée (page web) : le réseau choisi a changé de place ou disparu */
        pick = 0;
        for (int k = 0; k < WRX_NETS; k++)
            if (c->ssid[k][0] && !strcmp(c->ssid[k], s_pick_ssid)) pick = (uint8_t)(k + 1);
    }
    if (pick && !c->ssid[pick - 1][0]) pick = 0;
    if (pick != st->wifi_net) { st->wifi_net = pick; settings_save(); }
    strlcpy(s_pick_ssid, pick ? c->ssid[pick - 1] : "", sizeof s_pick_ssid);
    xSemaphoreTake(s_cfg_mx, portMAX_DELAY);
    memcpy(s_cfg, c, sizeof *c);
    s_pick = pick;
    xSemaphoreGive(s_cfg_mx);
    memset(c, 0, sizeof *c);                            /* mots de passe */
    free(c);
    s_reload = true;
    kick_task();
}

void wifi_set_pick(uint8_t pick)
{
    settings_t *st = settings_get();
    if (pick > WRX_NETS) pick = 0;
    if (st->wifi_net != pick) { st->wifi_net = pick; settings_save(); }
    s_pick_ssid[0] = 0;                                 /* le choix vient d'être fait : pas de suivi */
    wifi_load_cfg();
}

esp_err_t wifi_acquire(uint32_t holder, uint32_t flags)
{
    if (s_blocked) return ESP_ERR_INVALID_STATE;
    if (!ensure_base()) return ESP_ERR_NO_MEM;
    if (flags & WIFI_ACQ_PERF) { s_perf_hold |= holder; s_ps_dirty = true; }
    if (s_holders & holder) { kick_task(); return ESP_OK; }
    if (s_inited && (!(holder & WIFI_HOLD_CONNECT) || (s_holders & WIFI_HOLD_CONNECT))) {
        /* pilote déjà prêt (et connexion déjà voulue) : rien à attendre de la
         * tâche, qui peut être au milieu d'un essai (jusqu'à 15 s par réseau) */
        s_holders |= holder;
        sync_holders();
        kick_task();
        wifi_ble_policy();
        return ESP_OK;
    }
    drv_lock();
    esp_err_t e = driver_init_locked(holder, flags & WIFI_ACQ_PAUSE_BLE);
    if (e != ESP_OK) {
        s_perf_hold &= ~holder;
        drv_unlock();
        sync_holders();
        return e;
    }
    s_holders |= holder;
    if (holder & WIFI_HOLD_CONNECT) {
        if (!radio_start_locked()) ESP_LOGW(TAG, "esp_wifi_start a échoué");
    }
    if (s_started) apply_ps_locked();
    drv_unlock();
    power_set_cpu_max(160);                             /* radio en marche : 80 MHz suffisent écran éteint */
    sleep_policy();
    if (cur_state() == WIFI_OFF) set_state(WIFI_IDLE, NULL, "");
    sync_holders();
    if (holder & WIFI_HOLD_CONNECT) { s_kick = true; kick_task(); }
    wifi_ble_policy();
    return ESP_OK;
}

void wifi_release(uint32_t holder)
{
    if (!(s_holders & holder)) return;
    s_holders &= ~holder;
    s_perf_hold &= ~holder;
    if (holder == WIFI_HOLD_SCAN) s_scan_radio = false;
    bool stop_connect = !(s_holders & WIFI_HOLD_CONNECT);
    bool reinit = s_holders && s_ble_pauser == holder;
    if (s_holders && !stop_connect && !reinit) {
        /* les autres gardent la connexion : seule l'économie du modem change,
         * appliquée par la tâche (pas d'attente d'un essai en cours) */
        s_ps_dirty = true;
        sync_holders();
        kick_task();
        return;
    }
    s_abort = true;                                     /* l'essai en cours s'arrête */
    if (s_started) esp_wifi_scan_stop();                /* débloque un balayage de la tâche */
    kick_task();
    drv_lock();
    if (!s_holders) {
        driver_deinit_locked();
        ble_resume();
        power_set_cpu_max(0);
        portENTER_CRITICAL(&s_mux);
        s_st.ssid[0] = 0;
        portEXIT_CRITICAL(&s_mux);
        set_state(WIFI_OFF, NULL, "");
    } else if (s_ble_pauser == holder) {
        /* l'app qui avait suspendu le Bluetooth s'en va mais d'autres tiennent
         * le Wi-Fi : on le redémarre à côté du Bluetooth rendu, pour ne
         * jamais laisser le Bluetooth coupé par le Wi-Fi global */
        driver_deinit_locked();
        ble_resume();
        uint32_t keep = s_holders;
        s_holders = 0;
        set_state(WIFI_OFF, NULL, "");
        if (driver_init_locked(0, false) == ESP_OK) {
            s_holders = keep;
            if ((keep & WIFI_HOLD_CONNECT) || s_scan_radio) radio_start_locked();
            set_state(WIFI_IDLE, NULL, "");
        } else {
            /* plus assez de RAM : le Wi-Fi global s'arrête */
            ESP_LOGW(TAG, "Wi-Fi arrêté pour rendre le Bluetooth (RAM)");
            if (keep & WIFI_HOLD_USER) { settings_get()->wifi_on = false; settings_save(); }
            power_set_cpu_max(0);
            set_state(WIFI_OFF, N_("Mémoire insuffisante"), "");
        }
    } else if (stop_connect) {
        if (s_started) esp_wifi_disconnect();
        if (!s_scan_radio) radio_stop_locked();
        set_state(WIFI_IDLE, NULL, "");
    }
    s_abort = false;
    drv_unlock();
    sleep_policy();
    sync_holders();
    wifi_ble_policy();
    if (!stop_connect) kick_task();
}

bool wifi_user_set(bool on)
{
    settings_t *st = settings_get();
    if (on) {
        if (wifi_acquire(WIFI_HOLD_USER, 0) != ESP_OK) {
            if (st->wifi_on) { st->wifi_on = false; settings_save(); }
            return false;
        }
    } else {
        wifi_release(WIFI_HOLD_USER);
    }
    if (st->wifi_on != on) { st->wifi_on = on; settings_save(); }
    return true;
}

void wifi_scan_radio(bool on)
{
    if (!(s_holders & WIFI_HOLD_SCAN)) return;
    s_scan_radio = on;
    if (s_holders & WIFI_HOLD_CONNECT) return;          /* la connexion garde la radio */
    drv_lock();
    if (on) radio_start_locked();
    else radio_stop_locked();
    drv_unlock();
    sleep_policy();
}

void wifi_apply_power(void)
{
    /* sans le verrou du pilote : la tâche peut être au milieu d'un essai ;
     * pilote arrêté, la fonction échoue sans effet (appliqué au démarrage) */
    if (s_started) esp_wifi_set_max_tx_power(wifi_power_qdbm());
}

static int s_last_pct = -1;

bool wifi_battery_check(int pct, bool vbus)
{
    if (pct < 0) return false;
    int prev = s_last_pct;
    s_last_pct = vbus ? 100 : pct;
    if (!settings_get()->wifi_lowbatt || vbus || pct >= LOW_BATT_PCT) return false;
    /* seulement au passage du seuil : rallumé ensuite, c'est un choix */
    if (prev < LOW_BATT_PCT) return false;
    if (!wifi_user_on()) return false;
    ESP_LOGW(TAG, "batterie %d %% : Wi-Fi coupé", pct);
    wifi_user_set(false);
    return true;
}

void wifi_mgr_init(void)
{
    int pct = power_percent();
    bool vbus = power_vbus();
    s_last_pct = pct < 0 ? -1 : vbus ? 100 : pct;
    if (!settings_get()->wifi_on) return;
    /* batterie déjà faible au démarrage : pas de Wi-Fi automatique */
    if (settings_get()->wifi_lowbatt && !vbus && pct >= 0 && pct < LOW_BATT_PCT) {
        ESP_LOGW(TAG, "batterie %d %% : Wi-Fi global laissé coupé", pct);
        settings_get()->wifi_on = false;
        settings_save();
        return;
    }
    if (!wifi_user_set(true)) ESP_LOGW(TAG, "Wi-Fi global : démarrage impossible");
}

void wifi_mgr_shutdown(void)
{
    s_blocked = true;
    for (uint32_t b = 1; b && s_holders; b <<= 1)
        if (s_holders & b) wifi_release(b);
}

/* ------------------------------------------------------------------ */
/* Attente de la connexion (tâches réseau des apps)                     */
/* ------------------------------------------------------------------ */
bool wifi_wait_up(volatile bool *quit, wifi_status_cb cb)
{
    wifi_status_t st;
    wifi_get_status(&st);
    if (st.state == WIFI_CONNECTED) return true;
    if (st.state == WIFI_FAILED || st.state == WIFI_OFF || st.state == WIFI_IDLE) wifi_retry();
    /* l'essai relancé n'a peut-être pas encore commencé : un FAILED vu
     * juste après la relance est l'ancien */
    int64_t t0 = esp_timer_get_time();
    const char *last_msg = (const char *)-1;
    char last_det[sizeof st.detail] = "";
    while (!*quit) {
        wifi_get_status(&st);
        if (st.state == WIFI_CONNECTED) return true;
        bool fresh = esp_timer_get_time() - t0 > 1500000;
        if (st.state == WIFI_FAILED && fresh) {
            char d[48];
            if (st.msg && !strcmp(st.msg, "Connexion Wi-Fi impossible") && st.detail[0])
                snprintf(d, sizeof d, _("raison %d"), atoi(st.detail));
            else
                strlcpy(d, st.detail, sizeof d);
            cb(true, st.msg ? st.msg : N_("Connexion Wi-Fi impossible"), d);
            return false;
        }
        if (st.state == WIFI_OFF && fresh) {
            cb(true, st.msg ? st.msg : N_("Erreur Wi-Fi"), "");
            return false;
        }
        if (st.state == WIFI_CONNECTING && (st.msg != last_msg || strcmp(st.detail, last_det))) {
            last_msg = st.msg;
            strlcpy(last_det, st.detail, sizeof last_det);
            cb(false, st.msg ? st.msg : N_("Recherche du Wi-Fi…"), st.detail);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return false;
}
