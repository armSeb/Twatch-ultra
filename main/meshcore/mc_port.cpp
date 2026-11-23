/* mc_port.cpp — voir mc_port.h et target.h */
#include "mc_port.h"
#include "target.h"
#include <SPIFFS.h>
#include <MyMesh.h>
#include <new>
#include <time.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

extern "C" {
#include "sx1262.h"
#include "axp2101.h"
#include "wclock.h"
#include "ble.h"
}

static const char *TAG = "mesh";

#define OFFQ_PATH      "/offq"
#define OFFQ_SAVE_MS   5000     /* file modifiée : sauvegarde après 5 s de calme */
#define BUSY_TICK_MS   20       /* tâche active (émission, échanges avec l'app) */
#define IDLE_TICK_MS   1000
#define BUSY_HOLD_MS   3000     /* reste active un moment après un événement */
#define RXQ_LEN        4

static TaskHandle_t s_task;
static volatile bool s_run;
static int64_t s_busy_until;

static void wake_task(void)
{
    if (s_task) xTaskNotifyGive(s_task);
}

/* ================================================================== radio */
typedef struct { uint8_t len; int8_t snr_q4; int16_t rssi; uint8_t buf[SX_MAX_PKT]; } rxpkt_t;
static QueueHandle_t s_rxq;
static uint32_t s_rx_drop;

static void radio_rx_cb(const uint8_t *buf, size_t len, int16_t rssi, int8_t snr_q4)
{
    radio_driver.onRx(buf, len, rssi, snr_q4);
}

void TWatchRadio::onRx(const uint8_t *buf, size_t len, int16_t rssi, int8_t snr_q4)
{
    if (!s_rxq || !len || len > SX_MAX_PKT) return;
    static rxpkt_t p;                       /* tâche « lora » uniquement */
    p.len = (uint8_t)len;
    p.rssi = rssi;
    p.snr_q4 = snr_q4;
    memcpy(p.buf, buf, len);
    if (xQueueSend(s_rxq, &p, 0) != pdTRUE) s_rx_drop++;
    wake_task();
}

static sx_cfg_t radio_cfg(float freq, float bw, uint8_t sf, uint8_t cr, int8_t dbm)
{
    sx_cfg_t c = {};
    c.freq_hz = (uint32_t)(freq * 1e6f + 0.5f);
    c.bw_hz = (uint32_t)(bw * 1000.0f + 0.5f);
    c.sf = sf;
    c.cr = cr;
    c.power_dbm = dbm;
    c.sync_word = 0x12;                                 /* MeshCore : réseau privé */
    c.preamble = sf <= 8 ? 32 : 16;                     /* comme RadioLibWrapper */
    return c;
}

/* begin() est appelé par Dispatcher::begin(), avant le chargement des
 * préférences : la puce ne s'allume qu'avec start(), depuis la tâche */
void TWatchRadio::begin() { }

void TWatchRadio::start()
{
    dirty = false;
    sending = false;
    sx_cfg_t c = radio_cfg(freq, bw, sf, cr, power);
    if (sx_start(&c, radio_rx_cb) != ESP_OK) ESP_LOGE(TAG, "radio absente");
}

void TWatchRadio::stop() { sx_stop(); }

/* plusieurs réglages arrivent d'affilée (fréquence puis puissance) : la
 * puce n'est reconfigurée qu'une fois, au tour de boucle suivant */
void TWatchRadio::apply()
{
    if (!dirty || sending || !sx_tx_idle()) return;
    dirty = false;
    sx_cfg_t c = radio_cfg(freq, bw, sf, cr, power);
    if (sx_set_config(&c) != ESP_OK) ESP_LOGE(TAG, "reconfiguration radio impossible");
}

void TWatchRadio::setParams(float f, float b, uint8_t s, uint8_t c)
{
    if (f == freq && b == bw && s == sf && c == cr) return;
    freq = f; bw = b; sf = s; cr = c;
    dirty = true;
}

void TWatchRadio::setTxPower(int8_t dbm)
{
    if (dbm > 22) dbm = 22;
    if (dbm < -9) dbm = -9;
    if (dbm == power) return;
    power = dbm;
    dirty = true;
}

void TWatchRadio::loop() { apply(); }

int TWatchRadio::recvRaw(uint8_t *bytes, int sz)
{
    static rxpkt_t p;                       /* tâche « mesh » uniquement */
    if (!s_rxq || xQueueReceive(s_rxq, &p, 0) != pdTRUE) return 0;
    int n = p.len < sz ? p.len : sz;
    memcpy(bytes, p.buf, n);
    last_rssi = p.rssi;
    last_snr = p.snr_q4 / 4.0f;
    if (uxQueueMessagesWaiting(s_rxq)) wake_task();
    return n;
}

uint32_t TWatchRadio::getEstAirtimeFor(int len) { return sx_airtime_ms(len > 0 ? (size_t)len : 0); }

/* même estimation que RadioLibWrapper::packetScoreInt (seuils Semtech) */
float TWatchRadio::packetScore(float snr, int len)
{
    static const float thr[] = { -7.5f, -10.0f, -12.5f, -15.0f, -17.5f, -20.0f };
    if (sf < 7) return 0.0f;
    float t = thr[(sf > 12 ? 12 : sf) - 7];
    if (snr < t) return 0.0f;
    float v = (snr - t) / 10.0f * (1.0f - len / 256.0f);
    return v < 0 ? 0 : v > 1 ? 1 : v;
}

bool TWatchRadio::startSendRaw(const uint8_t *bytes, int len)
{
    sx_stats_t st;
    sx_get_stats(&st);
    tx_mark = st.tx_ok;
    if (!sx_send(bytes, (size_t)len)) return false;
    sending = true;
    return true;
}

/* succès seulement sur TxDone : un échec laisse le Dispatcher expirer et
 * compter l'envoi comme raté */
bool TWatchRadio::isSendComplete()
{
    if (!sending) return false;
    sx_stats_t st;
    sx_get_stats(&st);
    return st.tx_ok != tx_mark;
}

void TWatchRadio::onSendFinished() { sending = false; }
int TWatchRadio::getNoiseFloor() const
{
    int16_t n = sx_noise_floor();
    return n == INT16_MIN ? 0 : n;
}
bool TWatchRadio::isInRecvMode() const { return sx_running() && !sending; }
bool TWatchRadio::isReceiving() { return sx_rx_active(); }
uint32_t TWatchRadio::getPacketsRecv() const { sx_stats_t s; sx_get_stats(&s); return s.rx_ok; }
uint32_t TWatchRadio::getPacketsSent() const { sx_stats_t s; sx_get_stats(&s); return s.tx_ok; }
uint32_t TWatchRadio::getPacketsRecvErrors() const { sx_stats_t s; sx_get_stats(&s); return s.rx_crc; }

/* ================================================================== carte */
uint16_t TWatchBoard::getBattMilliVolts() { return (uint16_t)axp_batt_mv(); }
float TWatchBoard::getMCUTemperature() { return NAN; }
static void offq_save(void);
static bool offq_dirty(void);

/* demandé par l'app (tâche mesh) : MyMesh a déjà sauvé les contacts */
void TWatchBoard::reboot()
{
    if (offq_dirty()) offq_save();
    radio_driver.stop();
    esp_restart();
}

uint32_t TWatchClock::getCurrentTime() { return (uint32_t)time(NULL); }
void TWatchClock::setCurrentTime(uint32_t t) { wclock_set_utc((time_t)t); }

class HwRNG : public mesh::RNG {
public:
    void random(uint8_t *dest, size_t sz) override { esp_fill_random(dest, sz); }
};
static HwRNG s_rng;

mesh::LocalIdentity radio_new_identity() { return mesh::LocalIdentity(&s_rng); }

TWatchRadio radio_driver;
TWatchBoard board;
TWatchClock rtc_clock;
TWatchSensors sensors;

/* ================================================================== Bluetooth */
/* Interface « série » du protocole compagnon : une trame = une écriture
 * (RX) ou une notification (TX), comme le firmware MeshCore pour ESP32. */
typedef struct { uint8_t len; uint8_t buf[MC_FRAME_MAX]; } frame_t;
static QueueHandle_t s_ble_rxq;
static volatile bool s_link;            /* l'app est abonnée aux notifications */

class BleCompanion : public BaseSerialInterface {
public:
    void enable() override { enabled = true; }
    void disable() override { enabled = false; }
    bool isEnabled() const override { return enabled; }
    bool isConnected() const override { return s_link; }
    bool isWriteBusy() const override { return n >= TXQ; }
    size_t writeFrame(const uint8_t src[], size_t len) override
    {
        if (!s_link || !len || len > MC_FRAME_MAX || n >= TXQ) return 0;
        q[(head + n) % TXQ].len = (uint8_t)len;
        memcpy(q[(head + n) % TXQ].buf, src, len);
        n++;
        return len;
    }
    size_t checkRecvFrame(uint8_t dest[]) override
    {
        bool link = s_link;
        if (link != was_link) {                         /* l'app arrive ou repart */
            was_link = link;
            n = 0;
            if (!link && s_ble_rxq) xQueueReset(s_ble_rxq);
        }
        /* une notification tous les 60 ms au plus, comme l'original : iOS
         * perd des notifications envoyées en rafale */
        int64_t now = esp_timer_get_time();
        if (n && now - last_tx >= 60000) {
            if (ble_mc_notify(q[head].buf, q[head].len)) {
                head = (head + 1) % TXQ;
                n--;
            }
            last_tx = now;                              /* échec (mbufs) : on réessaie */
        }
        if (n) s_busy_until = now + 100000;
        static frame_t f;
        if (s_ble_rxq && xQueueReceive(s_ble_rxq, &f, 0) == pdTRUE) {
            memcpy(dest, f.buf, f.len);
            return f.len;
        }
        return 0;
    }
    bool pending() const { return n > 0; }
private:
    static const int TXQ = 16;
    frame_t q[TXQ];
    int head = 0, n = 0;
    int64_t last_tx = 0;
    bool enabled = false, was_link = false;
};

extern "C" void mc_ble_link(bool up)
{
    s_link = up;
    s_busy_until = esp_timer_get_time() + BUSY_HOLD_MS * 1000LL;
    wake_task();
}

extern "C" bool mc_ble_rx(const uint8_t *buf, size_t len)
{
    if (!s_ble_rxq || !len || len > MC_FRAME_MAX) return false;
    frame_t f;
    f.len = (uint8_t)len;
    memcpy(f.buf, buf, len);
    if (xQueueSend(s_ble_rxq, &f, 0) != pdTRUE) return false;
    s_busy_until = esp_timer_get_time() + BUSY_HOLD_MS * 1000LL;
    wake_task();
    return true;
}

/* ================================================================== historique montre */
#define HIST_N 64
EXT_RAM_BSS_ATTR static mc_msg_t s_hist[HIST_N];
static int s_hist_n, s_hist_head;       /* head : prochain emplacement */
static uint32_t s_seq;
static SemaphoreHandle_t s_hist_mx;
static mc_msg_cb_t s_msg_cb;

/* tronque sans couper un caractère UTF-8 */
static void copy_utf8(char *dst, size_t cap, const char *src)
{
    size_t n = strnlen(src, cap - 1);
    if (n == cap - 1) while (n > 0 && ((uint8_t)src[n] & 0xC0) == 0x80) n--;
    memcpy(dst, src, n);
    dst[n] = 0;
}

static void hist_add(mc_msg_t *m)
{
    xSemaphoreTake(s_hist_mx, portMAX_DELAY);
    m->seq = ++s_seq;
    s_hist[s_hist_head] = *m;
    s_hist_head = (s_hist_head + 1) % HIST_N;
    if (s_hist_n < HIST_N) s_hist_n++;
    xSemaphoreGive(s_hist_mx);
    if (s_msg_cb) s_msg_cb(m);
}

extern "C" int mc_history_count(void) { return s_hist_n; }
extern "C" uint32_t mc_history_last_seq(void) { return s_seq; }
extern "C" void mc_set_msg_cb(mc_msg_cb_t cb) { s_msg_cb = cb; }
extern "C" bool mc_history_get(int i, mc_msg_t *out)
{
    if (!s_hist_mx) return false;
    xSemaphoreTake(s_hist_mx, portMAX_DELAY);
    bool ok = i >= 0 && i < s_hist_n;
    if (ok) *out = s_hist[(s_hist_head - 1 - i + HIST_N) % HIST_N];
    xSemaphoreGive(s_hist_mx);
    return ok;
}

class WatchListener : public MyMesh::Listener {
public:
    void onMessageRecv(mesh::Packet *pkt, const ContactInfo &from, uint8_t txt_type,
                       uint32_t ts, const char *text) override
    {
        if (txt_type != TXT_TYPE_PLAIN && txt_type != TXT_TYPE_SIGNED_PLAIN) return;
        static mc_msg_t m;
        memset(&m, 0, sizeof m);
        m.ts = ts;
        m.snr_q4 = pkt->_snr;
        m.hops = pkt->isRouteFlood() ? pkt->path_len : 0xFF;
        copy_utf8(m.from, sizeof m.from, from.name);
        copy_utf8(m.text, sizeof m.text, text);
        hist_add(&m);
    }
    void onChannelMessageRecv(mesh::Packet *pkt, ChannelDetails &ch, const char *text) override
    {
        static mc_msg_t m;
        memset(&m, 0, sizeof m);
        m.ts = (uint32_t)time(NULL);
        m.channel = true;
        m.snr_q4 = pkt->_snr;
        m.hops = pkt->isRouteFlood() ? pkt->path_len : 0xFF;
        copy_utf8(m.from, sizeof m.from, ch.name);
        copy_utf8(m.text, sizeof m.text, text);
        hist_add(&m);
    }
    void onQueueSizeChanged(int n) override { }
};

/* ================================================================== mesh */
/* MeshCore n'est construit qu'une fois et vit en PSRAM : un arrêt coupe la
 * radio et la tâche, l'état (contacts, file de l'app) reste en mémoire. */
alignas(MyMesh) EXT_RAM_BSS_ATTR static uint8_t s_mesh_mem[sizeof(MyMesh)];
alignas(DataStore) EXT_RAM_BSS_ATTR static uint8_t s_store_mem[sizeof(DataStore)];
alignas(SimpleMeshTables) EXT_RAM_BSS_ATTR static uint8_t s_tables_mem[sizeof(SimpleMeshTables)];
static MyMesh *s_mesh;
static DataStore *s_store;
static BleCompanion *s_ble;
static WatchListener s_listener;
static StdRNG s_fast_rng;
static int64_t s_offq_dirty_at;

static portMUX_TYPE s_st_mux = portMUX_INITIALIZER_UNLOCKED;
static mc_status_t s_st;

static void update_status(void)
{
    mc_status_t t = {};
    NodePrefs *p = s_mesh->getNodePrefs();
    copy_utf8(t.name, sizeof t.name, p->node_name);
    t.freq_mhz = p->freq;
    t.bw_khz = p->bw;
    t.sf = p->sf;
    t.cr = p->cr;
    t.tx_dbm = p->tx_power_dbm;
    t.contacts = s_mesh->getNumContacts();
    t.app_queue = s_mesh->getOfflineQueueLen();
    t.app_connected = s_link;
    portENTER_CRITICAL(&s_st_mux);
    s_st = t;
    portEXIT_CRITICAL(&s_st_mux);
}

extern "C" void mc_get_status(mc_status_t *out)
{
    portENTER_CRITICAL(&s_st_mux);
    *out = s_st;
    portEXIT_CRITICAL(&s_st_mux);
}

static bool offq_dirty(void) { return s_mesh && s_mesh->isOfflineQueueDirty(); }

static void offq_save(void)
{
    s_mesh->clearOfflineQueueDirty();
    s_offq_dirty_at = 0;
    if (s_mesh->getOfflineQueueLen() == 0) {
        SPIFFS.remove(OFFQ_PATH);
        return;
    }
    File f = SPIFFS.open(OFFQ_PATH, "w", true);
    if (!f || !s_mesh->saveOfflineQueue(f)) ESP_LOGW(TAG, "file de l'app non sauvegardée");
    f.close();                          /* remplace l'ancienne version à la fermeture */
}

static void offq_load(void)
{
    File f = SPIFFS.open(OFFQ_PATH, "r");
    if (!f) return;
    s_mesh->loadOfflineQueue(f);
    f.close();
    s_mesh->clearOfflineQueueDirty();
    ESP_LOGI(TAG, "%d message(s) en attente pour l'app", s_mesh->getOfflineQueueLen());
}

static void mesh_task(void *arg)
{
    radio_driver.start();
    while (s_run) {
        s_mesh->loop();
        rtc_clock.tick();

        int64_t now = esp_timer_get_time();
        if (s_mesh->isOfflineQueueDirty()) {
            if (!s_offq_dirty_at) s_offq_dirty_at = now;
            /* l'app vide la file : on attend la fin de la synchro */
            if (now - s_offq_dirty_at >= OFFQ_SAVE_MS * 1000LL && !s_ble->pending()) offq_save();
        }
        update_status();

        bool busy = now < s_busy_until || s_mesh->hasPendingWork() || s_ble->pending() ||
                    !sx_tx_idle() || (s_rxq && uxQueueMessagesWaiting(s_rxq));
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(busy ? BUSY_TICK_MS : IDLE_TICK_MS));
    }
    if (s_mesh->isOfflineQueueDirty()) offq_save();
    radio_driver.stop();
    s_task = NULL;
    vTaskDelete(NULL);
}

extern "C" esp_err_t mc_start(void)
{
    if (s_run) return ESP_OK;
    if (!s_mesh) {
        if (!SPIFFS.begin(true)) return ESP_FAIL;
        s_hist_mx = xSemaphoreCreateMutex();
        s_rxq = xQueueCreate(RXQ_LEN, sizeof(rxpkt_t));
        s_ble_rxq = xQueueCreate(6, sizeof(frame_t));
        s_ble = new (std::nothrow) BleCompanion();
        if (!s_hist_mx || !s_rxq || !s_ble_rxq || !s_ble) return ESP_ERR_NO_MEM;
        auto *tables = new (s_tables_mem) SimpleMeshTables();
        s_store = new (s_store_mem) DataStore(SPIFFS, rtc_clock);
        s_mesh = new (s_mesh_mem) MyMesh(radio_driver, s_fast_rng, rtc_clock, *tables, *s_store);
        s_store->begin();
        s_mesh->begin();                    /* identité, préférences, contacts, canaux */
        offq_load();
        s_mesh->startInterface(*s_ble);
        s_mesh->setListener(&s_listener);
        update_status();
        ESP_LOGI(TAG, "MeshCore %s, nœud « %s »", FIRMWARE_VERSION, s_mesh->getNodeName());
    }
    s_run = true;
    if (xTaskCreatePinnedToCore(mesh_task, "mesh", 10240, NULL, 5, &s_task, 0) != pdPASS) {
        s_run = false;
        return ESP_ERR_NO_MEM;
    }
    ble_mc_set_enabled(true);
    return ESP_OK;
}

extern "C" void mc_stop(void)
{
    if (!s_run) return;
    ble_mc_set_enabled(false);
    s_run = false;
    wake_task();
    for (int i = 0; i < 200 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(10));
}

extern "C" bool mc_running(void) { return s_run; }
