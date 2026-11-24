/* websrv.c — voir websrv.h */
#include "websrv.h"
#include <string.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "nvs.h"
#include "cJSON.h"
#include "mbedtls/sha256.h"
#include "lwip/sockets.h"
#include "app_events.h"
#include "settings.h"
#include "wifi_mgr.h"
#include "power.h"
#include "sdcard.h"
#include "ota.h"
#include "ble.h"
#include "ui.h"
#include "webrx_cfg.h"
#include "meshcore/mc_port.h"

static const char *TAG = "web";

extern const char WEB_INDEX[] asm("_binary_index_html_start");
extern const char WEB_INDEX_END[] asm("_binary_index_html_end");

#define PW_LEN        10
#define SESS_MAX      4
#define SESS_IDLE_US  (30LL * 60 * 1000000)
#define SESS_LIFE_US  (12LL * 3600 * 1000000)
#define NONCE_MAX     4
#define NONCE_LIFE_US (60LL * 1000000)
#define BODY_MAX      4096                   /* réglages, connexion */
#define CHUNK         16384                  /* transferts de fichiers (réception) */
#define SD_IO         4096                   /* lecture / écriture de la carte, par appel */
#define UPLOAD_MAX    (64LL * 1024 * 1024)   /* au-delà, passer par le PC (partage USB) */
#define LIST_MAX      400                    /* entrées d'un dossier */
#define PATH_MAX_LEN  200

static httpd_handle_t s_srv;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_pw[PW_LEN + 1];               /* sous s_mux */
static volatile bool s_stopping;
static volatile int s_busy_fd = -1;         /* transfert long en cours (arrêt : coupé) */

typedef struct { char tok[33]; int64_t last, born; } sess_t;
typedef struct { char n[33]; int64_t t; } nonce_t;
EXT_RAM_BSS_ATTR static sess_t s_sess[SESS_MAX];             /* sous s_mux */
EXT_RAM_BSS_ATTR static nonce_t s_nonce[NONCE_MAX];          /* tâche httpd */
static int s_fails;
static int64_t s_lock_until;

/* tampons de la tâche httpd (une requête à la fois), en PSRAM */
static char *s_body, *s_buf;

/* ------------------------------------------------------------------ */
/* Mot de passe (tâche app)                                             */
/* ------------------------------------------------------------------ */
static const char PW_ALPHA[] = "abcdefghjkmnpqrstuvwxyz23456789";   /* sans 0/o, 1/l/i */

static void sessions_clear(void)
{
    portENTER_CRITICAL(&s_mux);
    memset(s_sess, 0, sizeof s_sess);
    portEXIT_CRITICAL(&s_mux);
}

static void pw_generate(char *out)
{
    const unsigned na = sizeof PW_ALPHA - 1;              /* 31 */
    const unsigned lim = 256 - 256 % na;                  /* tirage sans biais */
    int i = 0;
    while (i < PW_LEN) {
        uint8_t b[16];
        esp_fill_random(b, sizeof b);
        for (size_t k = 0; k < sizeof b && i < PW_LEN; k++)
            if (b[k] < lim) out[i++] = PW_ALPHA[b[k] % na];
    }
    out[PW_LEN] = 0;
}

static bool pw_valid(const char *p)
{
    if (strlen(p) != PW_LEN) return false;
    for (const char *c = p; *c; c++) if (!strchr(PW_ALPHA, *c)) return false;
    return true;
}

static void pw_save(const char *p)
{
    nvs_handle_t h;
    if (nvs_open("tw", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "webpw", p);
    nvs_commit(h);
    nvs_close(h);
}

const char *websrv_password(void)
{
    static char shown[PW_LEN + 1];
    portENTER_CRITICAL(&s_mux);
    bool have = s_pw[0] != 0;
    portEXIT_CRITICAL(&s_mux);
    if (!have) {
        char p[PW_LEN + 1] = "";
        nvs_handle_t h;
        size_t len = sizeof p;
        if (nvs_open("tw", NVS_READONLY, &h) == ESP_OK) {
            if (nvs_get_str(h, "webpw", p, &len) != ESP_OK) p[0] = 0;
            nvs_close(h);
        }
        if (!pw_valid(p)) {
            pw_generate(p);
            pw_save(p);
            ESP_LOGI(TAG, "mot de passe du serveur web créé");
        }
        portENTER_CRITICAL(&s_mux);
        memcpy(s_pw, p, sizeof s_pw);
        portEXIT_CRITICAL(&s_mux);
    }
    portENTER_CRITICAL(&s_mux);
    memcpy(shown, s_pw, sizeof shown);
    portEXIT_CRITICAL(&s_mux);
    return shown;
}

void websrv_new_password(void)
{
    char p[PW_LEN + 1];
    pw_generate(p);
    pw_save(p);
    portENTER_CRITICAL(&s_mux);
    memcpy(s_pw, p, sizeof s_pw);
    portEXIT_CRITICAL(&s_mux);
    sessions_clear();
    ESP_LOGI(TAG, "nouveau mot de passe : sessions fermées");
}

/* ------------------------------------------------------------------ */
/* Outils                                                               */
/* ------------------------------------------------------------------ */
static void hex(const uint8_t *b, size_t n, char *out)
{
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[2 * i] = H[b[i] >> 4]; out[2 * i + 1] = H[b[i] & 15]; }
    out[2 * n] = 0;
}

static bool ct_equal(const char *a, const char *b, size_t n)
{
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

static void sec_headers(httpd_req_t *r)
{
    httpd_resp_set_hdr(r, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(r, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(r, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_set_hdr(r, "Content-Security-Policy",
                       "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; "
                       "img-src data:; connect-src 'self'; form-action 'self'; frame-ancestors 'none'; base-uri 'none'");
}

static esp_err_t send_status(httpd_req_t *r, const char *status, const char *type, const char *body)
{
    sec_headers(r);
    httpd_resp_set_status(r, status);
    httpd_resp_set_type(r, type);
    return httpd_resp_sendstr(r, body);
}

static esp_err_t send_json(httpd_req_t *r, const char *json)
{
    return send_status(r, "200 OK", "application/json; charset=utf-8", json);
}

static esp_err_t err_json(httpd_req_t *r, const char *status, const char *code)
{
    char b[48];
    snprintf(b, sizeof b, "{\"err\":\"%s\"}", code);
    return send_status(r, status, "application/json; charset=utf-8", b);
}

/* Host : adresse IP de la montre ou nom local (contre le DNS rebinding) */
static bool host_ok(httpd_req_t *r)
{
    char h[72];
    if (httpd_req_get_hdr_value_str(r, "Host", h, sizeof h) != ESP_OK) return false;
    char *c = strrchr(h, ':');
    if (c && !strchr(h, ']')) *c = 0;
    for (char *p = h; *p; p++) *p = (char)tolower((unsigned char)*p);
    char ip[16];
    if (wifi_ip(ip) && !strcmp(h, ip)) return true;
    if (!h[0]) return false;
    if (!strchr(h, '.')) return true;                        /* nom d'hôte seul */
    static const char *const LOCAL[] = { ".local", ".lan", ".home", ".internal", ".home.arpa", ".localdomain" };
    size_t l = strlen(h);
    for (size_t i = 0; i < sizeof LOCAL / sizeof LOCAL[0]; i++) {
        size_t k = strlen(LOCAL[i]);
        if (l > k && !strcmp(h + l - k, LOCAL[i])) return true;
    }
    return false;
}

/* modification : en-tête X-TW (jamais envoyé par un formulaire d'un autre
 * site) et, s'il est là, Origin identique à l'hôte */
static bool csrf_ok(httpd_req_t *r)
{
    char v[4];
    if (httpd_req_get_hdr_value_str(r, "X-TW", v, sizeof v) != ESP_OK || strcmp(v, "1")) return false;
    size_t ol = httpd_req_get_hdr_value_len(r, "Origin");
    if (!ol) return true;
    char o[96], h[72];
    if (ol >= sizeof o || httpd_req_get_hdr_value_str(r, "Origin", o, sizeof o) != ESP_OK) return false;
    if (httpd_req_get_hdr_value_str(r, "Host", h, sizeof h) != ESP_OK) return false;
    return !strncmp(o, "http://", 7) && !strcasecmp(o + 7, h);
}

static bool cookie_token(httpd_req_t *r, char tok[33])
{
    char c[512];
    size_t l = httpd_req_get_hdr_value_len(r, "Cookie");
    if (!l || l >= sizeof c || httpd_req_get_hdr_value_str(r, "Cookie", c, sizeof c) != ESP_OK) return false;
    for (char *p = c; p && *p;) {
        while (*p == ' ') p++;
        if (!strncmp(p, "tw_s=", 5)) {
            p += 5;
            size_t n = strcspn(p, ";");
            if (n != 32) return false;
            memcpy(tok, p, 32);
            tok[32] = 0;
            return true;
        }
        p = strchr(p, ';');
        if (p) p++;
    }
    return false;
}

static bool authed(httpd_req_t *r)
{
    if (s_stopping || !host_ok(r)) return false;
    char tok[33];
    if (!cookie_token(r, tok)) return false;
    int64_t now = esp_timer_get_time();
    bool ok = false;
    portENTER_CRITICAL(&s_mux);
    for (int i = 0; i < SESS_MAX; i++) {
        sess_t *s = &s_sess[i];
        if (!s->tok[0]) continue;
        if (now - s->last > SESS_IDLE_US || now - s->born > SESS_LIFE_US) { memset(s, 0, sizeof *s); continue; }
        if (ct_equal(s->tok, tok, 32)) { s->last = now; ok = true; }
    }
    portEXIT_CRITICAL(&s_mux);
    return ok;
}

/* corps entier (petites requêtes) ; false = trop gros ou coupé */
static bool read_body(httpd_req_t *r, size_t max)
{
    if (r->content_len <= 0 || r->content_len >= max || r->content_len >= BODY_MAX) return false;
    size_t got = 0;
    while (got < r->content_len) {
        int n = httpd_req_recv(r, s_body + got, r->content_len - got);
        if (n <= 0) return false;
        got += (size_t)n;
    }
    s_body[got] = 0;
    return true;
}

/* ---- JSON ---- */
typedef struct { char *b; size_t n, cap; bool full; } jb_t;

static void jb_raw(jb_t *j, const char *s, size_t l)
{
    if (j->full || j->n + l + 1 > j->cap) { j->full = true; return; }
    memcpy(j->b + j->n, s, l);
    j->n += l;
    j->b[j->n] = 0;
}

static void jb_fmt(jb_t *j, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void jb_fmt(jb_t *j, const char *fmt, ...)
{
    if (j->full) return;
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(j->b + j->n, j->cap - j->n, fmt, ap);
    va_end(ap);
    if (k < 0 || j->n + (size_t)k + 1 > j->cap) { j->full = true; j->b[j->n] = 0; return; }
    j->n += (size_t)k;
}

/* chaîne JSON : guillemets, contrôles et < > & échappés ; UTF-8 invalide → ? */
static void jb_str(jb_t *j, const char *s)
{
    jb_raw(j, "\"", 1);
    const uint8_t *p = (const uint8_t *)s;
    while (*p && !j->full) {
        uint8_t c = *p;
        if (c < 0x80) {
            char e[8];
            if (c == '"' || c == '\\') { e[0] = '\\'; e[1] = (char)c; jb_raw(j, e, 2); }
            else if (c < 0x20 || c == '<' || c == '>' || c == '&') { snprintf(e, sizeof e, "\\u%04x", c); jb_raw(j, e, 6); }
            else jb_raw(j, (const char *)p, 1);
            p++;
            continue;
        }
        int n = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        bool ok = n > 0 && c >= 0xC2 && c <= 0xF4;
        for (int k = 1; ok && k < n; k++) ok = (p[k] & 0xC0) == 0x80;
        if (ok) { jb_raw(j, (const char *)p, (size_t)n); p += n; }
        else { jb_raw(j, "?", 1); p++; }
    }
    jb_raw(j, "\"", 1);
}

/* ---- requête : paramètre p (chemin), décodé ---- */
static int hexv(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static bool query_param(httpd_req_t *r, const char *key, char *out, size_t cap)
{
    char q[3 * PATH_MAX_LEN + 32], raw[3 * PATH_MAX_LEN + 8];
    size_t ql = httpd_req_get_url_query_len(r);
    if (!ql || ql >= sizeof q || httpd_req_get_url_query_str(r, q, sizeof q) != ESP_OK) return false;
    if (httpd_query_key_value(q, key, raw, sizeof raw) != ESP_OK) return false;
    size_t n = 0;
    for (const char *p = raw; *p; p++) {
        char c = *p;
        if (c == '+') c = ' ';
        else if (c == '%') {
            int h1 = hexv(p[1]), h2 = h1 >= 0 ? hexv(p[2]) : -1;
            if (h1 < 0 || h2 < 0) return false;
            c = (char)(h1 * 16 + h2);
            p += 2;
        }
        if (!c || n + 1 >= cap) return false;              /* NUL caché ou trop long */
        out[n++] = c;
    }
    out[n] = 0;
    return true;
}

/* ------------------------------------------------------------------ */
/* Connexion                                                            */
/* ------------------------------------------------------------------ */
static esp_err_t nonce_get(httpd_req_t *r)
{
    if (!host_ok(r)) return err_json(r, "403 Forbidden", "host");
    uint8_t b[16];
    esp_fill_random(b, sizeof b);
    int oldest = 0;
    for (int i = 1; i < NONCE_MAX; i++) if (s_nonce[i].t < s_nonce[oldest].t) oldest = i;
    hex(b, sizeof b, s_nonce[oldest].n);
    s_nonce[oldest].t = esp_timer_get_time();
    char j[64];
    snprintf(j, sizeof j, "{\"n\":\"%s\"}", s_nonce[oldest].n);
    return send_json(r, j);
}

static esp_err_t login_post(httpd_req_t *r)
{
    if (!host_ok(r) || !csrf_ok(r)) return err_json(r, "403 Forbidden", "csrf");
    int64_t now = esp_timer_get_time();
    if (now < s_lock_until) {
        char j[48];
        snprintf(j, sizeof j, "{\"err\":\"locked\",\"s\":%d}", (int)((s_lock_until - now) / 1000000) + 1);
        return send_status(r, "429 Too Many Requests", "application/json; charset=utf-8", j);
    }
    if (!read_body(r, 256)) return err_json(r, "400 Bad Request", "size");
    cJSON *js = cJSON_Parse(s_body);
    const cJSON *jn = cJSON_GetObjectItemCaseSensitive(js, "n");
    const cJSON *jh = cJSON_GetObjectItemCaseSensitive(js, "h");
    bool ok = false;
    if (cJSON_IsString(jn) && cJSON_IsString(jh) && strlen(jn->valuestring) == 32 && strlen(jh->valuestring) == 64) {
        for (int i = 0; i < NONCE_MAX; i++) {
            nonce_t *nc = &s_nonce[i];
            if (!nc->n[0] || !ct_equal(nc->n, jn->valuestring, 32)) continue;
            bool fresh = now - nc->t < NONCE_LIFE_US;
            char pw[PW_LEN + 1];
            portENTER_CRITICAL(&s_mux);
            memcpy(pw, s_pw, sizeof pw);
            portEXIT_CRITICAL(&s_mux);
            char msg[32 + 1 + PW_LEN + 1];
            int ml = snprintf(msg, sizeof msg, "%s:%s", nc->n, pw);
            memset(nc, 0, sizeof *nc);                     /* défi à usage unique */
            uint8_t d[32];
            char dh[65];
            if (fresh && pw[0] && mbedtls_sha256((const uint8_t *)msg, (size_t)ml, d, 0) == 0) {
                hex(d, sizeof d, dh);
                ok = ct_equal(dh, jh->valuestring, 64);
            }
            memset(msg, 0, sizeof msg);
            memset(pw, 0, sizeof pw);
            break;
        }
    }
    cJSON_Delete(js);
    if (!ok) {
        s_fails++;
        if (s_fails >= 5) {
            int64_t w = 30LL * 1000000 << (s_fails - 5 < 5 ? s_fails - 5 : 5);
            if (w > 15LL * 60 * 1000000) w = 15LL * 60 * 1000000;
            s_lock_until = now + w;
            ESP_LOGW(TAG, "%d essais ratés : connexion bloquée %d s", s_fails, (int)(w / 1000000));
            ble_diag("web : %d essais ratés", s_fails);
        }
        return err_json(r, "401 Unauthorized", "auth");
    }
    s_fails = 0;
    uint8_t b[16];
    esp_fill_random(b, sizeof b);
    char tok[33];
    hex(b, sizeof b, tok);
    portENTER_CRITICAL(&s_mux);
    int slot = 0;
    for (int i = 0; i < SESS_MAX; i++) {
        if (!s_sess[i].tok[0]) { slot = i; break; }
        if (s_sess[i].last < s_sess[slot].last) slot = i;      /* la plus ancienne cède sa place */
    }
    memcpy(s_sess[slot].tok, tok, sizeof tok);
    s_sess[slot].last = s_sess[slot].born = now;
    portEXIT_CRITICAL(&s_mux);
    EXT_RAM_BSS_ATTR static char ck[96];
    snprintf(ck, sizeof ck, "tw_s=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=43200", tok);
    httpd_resp_set_hdr(r, "Set-Cookie", ck);
    ESP_LOGI(TAG, "session ouverte");
    return send_json(r, "{\"ok\":1}");
}

static esp_err_t logout_post(httpd_req_t *r)
{
    if (!host_ok(r) || !csrf_ok(r)) return err_json(r, "403 Forbidden", "csrf");
    char tok[33];
    if (cookie_token(r, tok)) {
        portENTER_CRITICAL(&s_mux);
        for (int i = 0; i < SESS_MAX; i++)
            if (s_sess[i].tok[0] && ct_equal(s_sess[i].tok, tok, 32)) memset(&s_sess[i], 0, sizeof s_sess[i]);
        portEXIT_CRITICAL(&s_mux);
    }
    httpd_resp_set_hdr(r, "Set-Cookie", "tw_s=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
    return send_json(r, "{\"ok\":1}");
}

/* ------------------------------------------------------------------ */
/* Page, état                                                           */
/* ------------------------------------------------------------------ */
static esp_err_t index_get(httpd_req_t *r)
{
    if (!host_ok(r)) return send_status(r, "403 Forbidden", "text/plain", "host");
    sec_headers(r);
    httpd_resp_set_type(r, "text/html; charset=utf-8");
    return httpd_resp_send(r, WEB_INDEX, WEB_INDEX_END - WEB_INDEX - 1);
}

static esp_err_t to_index(httpd_req_t *r)
{
    sec_headers(r);
    httpd_resp_set_status(r, "303 See Other");
    httpd_resp_set_hdr(r, "Location", "/");
    return httpd_resp_send(r, NULL, 0);
}

static const char *sd_state_name(sd_state_t s)
{
    switch (s) {
    case SD_MOUNTED: return "mounted";
    case SD_EJECTED: return "ejected";
    case SD_FAILED:  return "failed";
    case SD_SHARED:  return "shared";
    default:         return "absent";
    }
}

static esp_err_t status_get(httpd_req_t *r)
{
    if (!authed(r)) return err_json(r, "401 Unauthorized", "auth");
    jb_t j = { s_buf, 0, CHUNK, false };
    const settings_t *st = settings_get();
    wifi_status_t ws;
    wifi_get_status(&ws);
    sd_info_t si;
    sd_get_info(&si);
    mc_status_t ms;
    mc_get_status(&ms);
    char cl[16];
    bool tcp = mc_tcp_client(cl);
    jb_fmt(&j, "{\"v\":");
    jb_str(&j, esp_app_get_description()->version);
    jb_fmt(&j, ",\"up\":%lld,\"bat\":%d,\"usb\":%s,\"chg\":%s,\"ssid\":", (long long)(esp_timer_get_time() / 1000000),
           power_percent(), power_vbus() ? "true" : "false", power_charging() ? "true" : "false");
    jb_str(&j, ws.ssid);
    jb_fmt(&j, ",\"ip\":\"%s\",\"rssi\":%d,\"bt\":%s,\"mesh\":%s,\"node\":", ws.ip, ws.rssi,
           ble_running() ? "true" : "false", mc_running() ? "true" : "false");
    jb_str(&j, ms.name);
    jb_fmt(&j, ",\"mfreq\":%.3f,\"mbw\":%.1f,\"msf\":%u,\"mcr\":%u,\"mtx\":%d,\"mcontacts\":%d,\"mqueue\":%d,"
               "\"mapp\":%s,\"mtcp\":%s,\"mclient\":\"%s\",\"sd\":\"%s\",\"sdfree\":%llu,\"sdtot\":%llu,\"websd\":%s,"
               "\"heap\":%u,\"psram\":%u}",
           ms.freq_mhz, ms.bw_khz, ms.sf, ms.cr, ms.tx_dbm, ms.contacts, ms.app_queue,
           ms.app_connected ? "true" : "false", mc_tcp_running() ? "true" : "false", tcp ? cl : "",
           sd_state_name(si.state), (unsigned long long)si.free, (unsigned long long)si.total,
           st->web_sd ? "true" : "false",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    if (j.full) return err_json(r, "500 Internal Server Error", "full");
    return send_json(r, j.b);
}

/* ------------------------------------------------------------------ */
/* Réglages                                                             */
/* ------------------------------------------------------------------ */
typedef struct { const char *key; size_t off; uint8_t kind; int lo, hi; } field_t;
enum { F_BOOL, F_U8, F_LIST };
#define FO(f) offsetof(settings_t, f)
static const field_t FIELDS[] = {
    { "bright", FO(brightness),       F_U8,   10, 255 },
    { "tmo",    FO(screen_timeout_s), F_LIST, 0, 0 },
    { "face",   FO(watchface),        F_U8,   0, 2 },
    { "lang",   FO(lang),             F_U8,   0, 1 },
    { "vib",    FO(vibrate),          F_BOOL, 0, 1 },
    { "nwake",  FO(notif_wake),       F_BOOL, 0, 1 },
    { "tap",    FO(tap_wake),         F_BOOL, 0, 1 },
    { "dnd",    FO(dnd),              F_BOOL, 0, 1 },
    { "silent", FO(silent),           F_BOOL, 0, 1 },
    { "vol",    FO(volume),           F_U8,   0, 100 },
    { "wrist",  FO(wrist_wake),       F_BOOL, 0, 1 },
    { "wsens",  FO(wrist_sens),       F_U8,   0, 2 },
    { "glance", FO(glance),           F_BOOL, 0, 1 },
    { "gpct",   FO(glance_pct),       F_LIST, 1, 0 },
    { "gdur",   FO(glance_s),         F_LIST, 2, 0 },
    { "steps",  FO(step_count),       F_BOOL, 0, 1 },
    { "bt",     FO(ble_enabled),      F_BOOL, 0, 1 },
    { "mesh",   FO(meshcore),         F_BOOL, 0, 1 },
    { "ant",    FO(lora_ext_ant),     F_BOOL, 0, 1 },
    { "mtcp",   FO(mesh_tcp),         F_BOOL, 0, 1 },
    { "wpwr",   FO(wifi_pwr),         F_U8,   0, 3 },
    { "wlow",   FO(wifi_lowbatt),     F_BOOL, 0, 1 },
    { "wnet",   FO(wifi_net),         F_U8,   0, WRX_NETS },
    { "websd",  FO(web_sd),           F_BOOL, 0, 1 },
};
#define NFIELDS (sizeof FIELDS / sizeof FIELDS[0])
/* valeurs permises des listes (comme les sélecteurs de la montre) */
static const uint8_t LIST_TMO[] = { 5, 8, 10, 15, 30 }, LIST_GPCT[] = { 10, 25, 50 }, LIST_GDUR[] = { 3, 4, 6 };

static bool list_ok(int which, int v)
{
    const uint8_t *l = which == 0 ? LIST_TMO : which == 1 ? LIST_GPCT : LIST_GDUR;
    size_t n = which == 0 ? sizeof LIST_TMO : which == 1 ? sizeof LIST_GPCT : sizeof LIST_GDUR;
    for (size_t i = 0; i < n; i++) if (l[i] == v) return true;
    return false;
}

static esp_err_t settings_get_h(httpd_req_t *r)
{
    if (!authed(r)) return err_json(r, "401 Unauthorized", "auth");
    settings_t st = *settings_get();
    jb_t j = { s_buf, 0, CHUNK, false };
    jb_raw(&j, "{", 1);
    for (size_t i = 0; i < NFIELDS; i++) {
        const uint8_t *p = (const uint8_t *)&st + FIELDS[i].off;
        if (FIELDS[i].kind == F_BOOL) jb_fmt(&j, "%s\"%s\":%s", i ? "," : "", FIELDS[i].key, *(const bool *)p ? "true" : "false");
        else jb_fmt(&j, "%s\"%s\":%u", i ? "," : "", FIELDS[i].key, *p);
    }
    /* réseaux connus (noms seuls, pour le choix du réseau) */
    wifi_status_t ws;
    wifi_get_status(&ws);
    jb_raw(&j, ",\"nets\":[", 9);
    wrx_cfg_t *c = heap_caps_malloc(sizeof *c, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (c) {
        /* lecture NVS : pas depuis cette tâche (pile en PSRAM) ; la tâche app
         * la fait pour nous */
        if (!app_call((void (*)(void *))wrx_cfg_load, c, 3000)) memset(c, 0, sizeof *c);
        for (int k = 0; k < WRX_NETS; k++) {
            if (k) jb_raw(&j, ",", 1);
            jb_str(&j, c->ssid[k]);
        }
        memset(c, 0, sizeof *c);
        free(c);
    }
    jb_raw(&j, "]}", 2);
    if (j.full) return err_json(r, "500 Internal Server Error", "full");
    return send_json(r, j.b);
}

typedef struct { settings_t v; uint32_t mask; } patch_t;

static void apply_patch(void *arg)
{
    const patch_t *p = arg;
    settings_t n = *settings_get();
    for (size_t i = 0; i < NFIELDS; i++) {
        if (!(p->mask & (1u << i))) continue;
        const uint8_t *src = (const uint8_t *)&p->v + FIELDS[i].off;
        uint8_t *dst = (uint8_t *)&n + FIELDS[i].off;
        *dst = *src;
    }
    ui_apply_settings(&n);
}

static esp_err_t settings_post(httpd_req_t *r)
{
    if (!csrf_ok(r)) return err_json(r, "403 Forbidden", "csrf");
    if (!authed(r)) return err_json(r, "401 Unauthorized", "auth");
    if (!read_body(r, BODY_MAX)) return err_json(r, "400 Bad Request", "size");
    cJSON *js = cJSON_Parse(s_body);
    if (!cJSON_IsObject(js)) { cJSON_Delete(js); return err_json(r, "400 Bad Request", "json"); }
    EXT_RAM_BSS_ATTR static patch_t p;
    memset(&p, 0, sizeof p);
    const char *bad = NULL;
    for (size_t i = 0; i < NFIELDS && !bad; i++) {
        const cJSON *it = cJSON_GetObjectItemCaseSensitive(js, FIELDS[i].key);
        if (!it) continue;
        uint8_t *dst = (uint8_t *)&p.v + FIELDS[i].off;
        if (FIELDS[i].kind == F_BOOL) {
            if (!cJSON_IsBool(it)) { bad = FIELDS[i].key; break; }
            *(bool *)dst = cJSON_IsTrue(it);
        } else {
            if (!cJSON_IsNumber(it)) { bad = FIELDS[i].key; break; }
            double d = it->valuedouble;
            int v = (int)d;
            if (d != (double)v) { bad = FIELDS[i].key; break; }
            if (FIELDS[i].kind == F_LIST ? !list_ok(FIELDS[i].lo, v) : (v < FIELDS[i].lo || v > FIELDS[i].hi)) {
                bad = FIELDS[i].key;
                break;
            }
            *dst = (uint8_t)v;
        }
        p.mask |= 1u << i;
    }
    cJSON_Delete(js);
    if (bad) {
        char b[48];
        snprintf(b, sizeof b, "{\"err\":\"value\",\"key\":\"%.16s\"}", bad);
        return send_status(r, "400 Bad Request", "application/json; charset=utf-8", b);
    }
    if (p.mask && !app_call(apply_patch, &p, 5000)) return err_json(r, "503 Service Unavailable", "busy");
    return send_json(r, "{\"ok\":1}");
}

/* ------------------------------------------------------------------ */
/* Mise à jour, réseaux                                                 */
/* ------------------------------------------------------------------ */
static esp_err_t ota_post(httpd_req_t *r)
{
    if (!csrf_ok(r)) return err_json(r, "403 Forbidden", "csrf");
    if (!authed(r)) return err_json(r, "401 Unauthorized", "auth");
    sec_headers(r);
    s_busy_fd = httpd_req_to_sockfd(r);
    esp_err_t e = ota_http_update(r, true);
    s_busy_fd = -1;
    return e;
}

static esp_err_t webrx_get_h(httpd_req_t *r)
{
    if (!authed(r)) return to_index(r);
    sec_headers(r);
    return ota_webrx_page(r);
}

static esp_err_t webrx_post_h(httpd_req_t *r)
{
    if (!csrf_ok(r)) return send_status(r, "403 Forbidden", "text/plain", "csrf");
    if (!authed(r)) return send_status(r, "401 Unauthorized", "text/plain", "auth");
    sec_headers(r);
    return ota_webrx_save(r);
}

/* ------------------------------------------------------------------ */
/* Carte SD                                                             */
/* ------------------------------------------------------------------ */
static int s_xfd = -1;                      /* fichier en transfert : sous sd_lock() seulement */

static void xfer_closer(void)
{
    if (s_xfd >= 0) { close(s_xfd); s_xfd = -1; }
}

/* Chemin relatif à la carte (« / », « /dossier/fichier ») : composants non
 * vides, ni « . » ni « .. », sans caractères interdits en FAT ni espace ou
 * point final (FAT les retire : deux noms pour un fichier). */
static bool sd_path_ok(const char *p, bool allow_root)
{
    size_t l = strlen(p);
    if (p[0] != '/' || l > PATH_MAX_LEN) return false;
    if (l == 1) return allow_root;
    const char *c = p + 1;
    while (*c) {
        const char *e = strchr(c, '/');
        size_t n = e ? (size_t)(e - c) : strlen(c);
        if (n == 0 || n > 128) return false;
        if ((n == 1 && c[0] == '.') || (n == 2 && c[0] == '.' && c[1] == '.')) return false;
        if (c[n - 1] == '.' || c[n - 1] == ' ' || c[0] == ' ') return false;
        for (size_t i = 0; i < n; i++) {
            unsigned char ch = (unsigned char)c[i];
            if (ch < 0x20 || ch == 0x7F || strchr("\\:*?\"<>|", ch)) return false;
        }
        if (!e) break;
        c = e + 1;
        if (!*c) return false;                              /* « / » final */
    }
    return true;
}

/* /meshcore (archive des messages) : lecture seule */
static bool sd_readonly(const char *p)
{
    static const char RO[] = "/meshcore";
    size_t k = sizeof RO - 1;
    return !strncasecmp(p, RO, k) && (p[k] == 0 || p[k] == '/');
}

static void sd_full(const char *p, char *out, size_t cap)
{
    snprintf(out, cap, "%s%s", SD_ROOT, strcmp(p, "/") ? p : "");
}

static bool sd_guard(httpd_req_t *r, bool write, esp_err_t *ret)
{
    if (write && !csrf_ok(r)) { *ret = err_json(r, "403 Forbidden", "csrf"); return false; }
    if (!authed(r)) { *ret = err_json(r, "401 Unauthorized", "auth"); return false; }
    if (!settings_get()->web_sd) { *ret = err_json(r, "403 Forbidden", "sd_off"); return false; }
    if (!sd_mounted()) {
        sd_info_t si;
        sd_get_info(&si);
        *ret = err_json(r, "503 Service Unavailable", si.state == SD_SHARED ? "sd_shared" : "sd_none");
        return false;
    }
    return true;
}

static esp_err_t sd_list(httpd_req_t *r)
{
    esp_err_t ret;
    if (!sd_guard(r, false, &ret)) return ret;
    char p[PATH_MAX_LEN + 1], full[PATH_MAX_LEN + 8], ent[PATH_MAX_LEN + 8 + 258];
    if (!query_param(r, "p", p, sizeof p) || !sd_path_ok(p, true)) return err_json(r, "400 Bad Request", "path");
    sd_full(p, full, sizeof full);
    jb_t j = { s_buf, 0, CHUNK * 4, false };
    jb_raw(&j, "{\"p\":", 5);
    jb_str(&j, p);
    jb_raw(&j, ",\"e\":[", 6);
    if (!sd_lock()) return err_json(r, "503 Service Unavailable", "sd_none");
    DIR *d = opendir(full);
    int n = 0;
    bool more = false;
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            if (n >= LIST_MAX || j.n > j.cap - 1024) { more = true; break; }
            snprintf(ent, sizeof ent, "%s/%s", full, de->d_name);
            struct stat sb;
            bool dir = de->d_type == DT_DIR;
            long long size = 0, mt = 0;
            if (stat(ent, &sb) == 0) { dir = S_ISDIR(sb.st_mode); size = sb.st_size; mt = (long long)sb.st_mtime; }
            jb_raw(&j, n ? ",{\"n\":" : "{\"n\":", n ? 6 : 5);
            jb_str(&j, de->d_name);
            jb_fmt(&j, ",\"d\":%d,\"s\":%lld,\"t\":%lld}", dir ? 1 : 0, dir ? 0LL : size, mt);
            n++;
        }
        closedir(d);
    }
    sd_unlock();
    if (!d) return err_json(r, "404 Not Found", "nodir");
    sd_info_t si;
    sd_get_info(&si);
    jb_fmt(&j, "],\"more\":%s,\"ro\":%s,\"free\":%llu,\"tot\":%llu}", more ? "true" : "false",
           sd_readonly(p) ? "true" : "false", (unsigned long long)si.free, (unsigned long long)si.total);
    if (j.full) return err_json(r, "500 Internal Server Error", "full");
    return send_json(r, j.b);
}

/* nom pour Content-Disposition : ASCII sûr + forme UTF-8 encodée (RFC 6266) */
static void disposition(const char *name, char *out, size_t cap)
{
    char a[96], u[3 * 128 + 1];
    size_t na = 0, nu = 0;
    for (const unsigned char *c = (const unsigned char *)name; *c; c++) {
        if (na + 1 < sizeof a) a[na++] = (*c >= 0x20 && *c < 0x7F && *c != '"' && *c != '\\' && *c != ';') ? (char)*c : '_';
        if (isalnum(*c) || strchr("-._~", *c)) { if (nu + 1 < sizeof u) u[nu++] = (char)*c; }
        else if (nu + 3 < sizeof u) { snprintf(u + nu, 4, "%%%02X", *c); nu += 3; }
    }
    a[na] = u[nu] = 0;
    snprintf(out, cap, "attachment; filename=\"%s\"; filename*=UTF-8''%s", a, u);
}

static void xfer_end(void)
{
    if (sd_lock()) { xfer_closer(); sd_unlock(); }
}

static esp_err_t sd_get(httpd_req_t *r)
{
    esp_err_t ret;
    if (!sd_guard(r, false, &ret)) return ret;
    char p[PATH_MAX_LEN + 1], full[PATH_MAX_LEN + 8];
    if (!query_param(r, "p", p, sizeof p) || !sd_path_ok(p, false)) return err_json(r, "400 Bad Request", "path");
    sd_full(p, full, sizeof full);
    if (!sd_lock()) return err_json(r, "503 Service Unavailable", "sd_none");
    struct stat sb;
    bool ok = stat(full, &sb) == 0 && S_ISREG(sb.st_mode);
    if (ok) { xfer_closer(); s_xfd = open(full, O_RDONLY); ok = s_xfd >= 0; }
    sd_unlock();
    if (!ok) return err_json(r, "404 Not Found", "nofile");
    EXT_RAM_BSS_ATTR static char cd[640];
    disposition(strrchr(p, '/') + 1, cd, sizeof cd);
    sec_headers(r);
    httpd_resp_set_type(r, "application/octet-stream");
    httpd_resp_set_hdr(r, "Content-Disposition", cd);
    s_busy_fd = httpd_req_to_sockfd(r);
    for (;;) {
        if (s_stopping || !sd_lock()) break;
        int n = s_xfd >= 0 ? read(s_xfd, s_buf, SD_IO) : -1;
        sd_unlock();
        if (n < 0) break;
        if (n == 0) {
            xfer_end();
            s_busy_fd = -1;
            return httpd_resp_send_chunk(r, NULL, 0);
        }
        if (httpd_resp_send_chunk(r, s_buf, n) != ESP_OK) break;
    }
    xfer_end();
    s_busy_fd = -1;
    return ESP_FAIL;                                        /* connexion coupée : téléchargement incomplet */
}

static esp_err_t sd_put(httpd_req_t *r)
{
    esp_err_t ret;
    if (!sd_guard(r, true, &ret)) return ret;
    char p[PATH_MAX_LEN + 1], full[PATH_MAX_LEN + 8], tmp[PATH_MAX_LEN + 16], ov[4] = "";
    if (!query_param(r, "p", p, sizeof p) || !sd_path_ok(p, false) || strlen(p) + 5 > PATH_MAX_LEN)
        return err_json(r, "400 Bad Request", "path");
    if (sd_readonly(p)) return err_json(r, "403 Forbidden", "readonly");
    query_param(r, "o", ov, sizeof ov);
    bool overwrite = !strcmp(ov, "1");
    sd_info_t si;
    sd_get_info(&si);
    if ((long long)r->content_len > UPLOAD_MAX) return err_json(r, "413 Payload Too Large", "big");
    if (si.free && (uint64_t)r->content_len + 65536 > si.free) return err_json(r, "507 Insufficient Storage", "space");
    sd_full(p, full, sizeof full);
    snprintf(tmp, sizeof tmp, "%s.part", full);
    /* dossier parent présent, cible pas un dossier */
    char parent[PATH_MAX_LEN + 8];
    strlcpy(parent, full, sizeof parent);
    *strrchr(parent, '/') = 0;
    if (!sd_lock()) return err_json(r, "503 Service Unavailable", "sd_none");
    struct stat sb;
    const char *err = NULL;
    if (stat(parent, &sb) != 0 || !S_ISDIR(sb.st_mode)) err = "nodir";
    else if (stat(full, &sb) == 0) err = S_ISDIR(sb.st_mode) ? "isdir" : overwrite ? NULL : "exists";
    if (!err) {
        xfer_closer();
        s_xfd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (s_xfd < 0) err = "write";
    }
    sd_unlock();
    if (err) return err_json(r, !strcmp(err, "exists") ? "409 Conflict" : "400 Bad Request", err);
    s_busy_fd = httpd_req_to_sockfd(r);
    size_t left = r->content_len;
    int timeouts = 0;
    err = NULL;
    while (left && !err) {
        size_t want = left < CHUNK ? left : CHUNK, got = 0;
        while (got < want) {
            int n = httpd_req_recv(r, s_buf + got, want - got);
            if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 4 && !s_stopping) continue;
            if (n <= 0 || s_stopping) { err = "net"; break; }
            timeouts = 0;
            got += (size_t)n;
        }
        if (err) break;
        if (!sd_lock()) { err = "sd_none"; break; }
        /* par blocs de SD_IO : tampon PSRAM recopié par le pilote dans un
         * tampon DMA de RAM interne de cette taille */
        size_t off = 0;
        while (off < got && s_xfd >= 0) {
            size_t k = got - off < SD_IO ? got - off : SD_IO;
            if (write(s_xfd, s_buf + off, k) != (ssize_t)k) break;
            off += k;
        }
        sd_unlock();
        if (off != got) { err = "write"; break; }
        left -= got;
    }
    if (sd_lock()) {
        bool open_ok = s_xfd >= 0;
        xfer_closer();
        if (!err && !open_ok) err = "sd_none";
        if (!err) {
            if (overwrite) unlink(full);
            if (rename(tmp, full) != 0) err = "write";
        }
        if (err) unlink(tmp);
        sd_unlock();
    } else if (!err) {
        err = "sd_none";
    }
    s_busy_fd = -1;
    if (err) {
        ESP_LOGW(TAG, "envoi de fichier : %s", err);
        if (!strcmp(err, "net")) return ESP_FAIL;
        return err_json(r, "500 Internal Server Error", err);
    }
    ESP_LOGI(TAG, "fichier reçu (%u o)", (unsigned)r->content_len);
    sd_refresh_info();
    return send_json(r, "{\"ok\":1}");
}

static esp_err_t sd_mkdir(httpd_req_t *r)
{
    esp_err_t ret;
    if (!sd_guard(r, true, &ret)) return ret;
    char p[PATH_MAX_LEN + 1], full[PATH_MAX_LEN + 8];
    if (!query_param(r, "p", p, sizeof p) || !sd_path_ok(p, false)) return err_json(r, "400 Bad Request", "path");
    if (sd_readonly(p)) return err_json(r, "403 Forbidden", "readonly");
    sd_full(p, full, sizeof full);
    if (!sd_lock()) return err_json(r, "503 Service Unavailable", "sd_none");
    int e = mkdir(full, 0777);
    sd_unlock();
    return e == 0 ? send_json(r, "{\"ok\":1}") : err_json(r, "409 Conflict", "exists");
}

static esp_err_t sd_del(httpd_req_t *r)
{
    esp_err_t ret;
    if (!sd_guard(r, true, &ret)) return ret;
    char p[PATH_MAX_LEN + 1], full[PATH_MAX_LEN + 8];
    if (!query_param(r, "p", p, sizeof p) || !sd_path_ok(p, false)) return err_json(r, "400 Bad Request", "path");
    if (sd_readonly(p) || !strcasecmp(p, "/logs")) return err_json(r, "403 Forbidden", "readonly");
    sd_full(p, full, sizeof full);
    if (!sd_lock()) return err_json(r, "503 Service Unavailable", "sd_none");
    struct stat sb;
    const char *err = NULL;
    if (stat(full, &sb) != 0) err = "nofile";
    else if (S_ISDIR(sb.st_mode) ? rmdir(full) != 0 : unlink(full) != 0) err = S_ISDIR(sb.st_mode) ? "notempty" : "write";
    sd_unlock();
    if (err) return err_json(r, "409 Conflict", err);
    sd_refresh_info();
    return send_json(r, "{\"ok\":1}");
}

/* ------------------------------------------------------------------ */
/* Serveur                                                              */
/* ------------------------------------------------------------------ */
static esp_err_t not_found(httpd_req_t *r) { return send_status(r, "404 Not Found", "text/plain", "404"); }

bool websrv_running(void) { return s_srv != NULL; }

esp_err_t websrv_start(void)
{
    if (s_srv) return ESP_OK;
    websrv_password();                                      /* créé au premier lancement */
    if (!s_body) s_body = heap_caps_malloc(BODY_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_buf) s_buf = heap_caps_malloc(CHUNK * 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_body || !s_buf) return ESP_ERR_NO_MEM;
    static bool closer;
    if (!closer) { sd_add_closer(xfer_closer); closer = true; }
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.stack_size = 6144;
    hc.task_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;     /* flash : via ota.c (pile interne) */
    hc.task_priority = 4;
    hc.max_uri_handlers = 24;
    hc.max_open_sockets = 4;                                /* chaque socket coûte de la RAM interne */
    hc.lru_purge_enable = true;
    hc.recv_wait_timeout = 5;
    hc.send_wait_timeout = 5;
    hc.max_resp_headers = 12;
    hc.uri_match_fn = httpd_uri_match_wildcard;
    s_stopping = false;
    esp_err_t e = httpd_start(&s_srv, &hc);
    if (e != ESP_OK) {
        s_srv = NULL;
        ESP_LOGW(TAG, "démarrage impossible : %s", esp_err_to_name(e));
        ble_diag("web : %s, int %u Ko", esp_err_to_name(e), (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
        return e;
    }
    static const httpd_uri_t uris[] = {
        { .uri = "/",              .method = HTTP_GET,  .handler = index_get },
        { .uri = "/index.html",    .method = HTTP_GET,  .handler = index_get },
        { .uri = "/update.html",   .method = HTTP_GET,  .handler = to_index },
        { .uri = "/webrx",         .method = HTTP_GET,  .handler = webrx_get_h },
        { .uri = "/webrx",         .method = HTTP_POST, .handler = webrx_post_h },
        { .uri = "/api/nonce",     .method = HTTP_GET,  .handler = nonce_get },
        { .uri = "/api/login",     .method = HTTP_POST, .handler = login_post },
        { .uri = "/api/logout",    .method = HTTP_POST, .handler = logout_post },
        { .uri = "/api/status",    .method = HTTP_GET,  .handler = status_get },
        { .uri = "/api/settings",  .method = HTTP_GET,  .handler = settings_get_h },
        { .uri = "/api/settings",  .method = HTTP_POST, .handler = settings_post },
        { .uri = "/api/ota",       .method = HTTP_POST, .handler = ota_post },
        { .uri = "/api/sd/list",   .method = HTTP_GET,  .handler = sd_list },
        { .uri = "/api/sd/get",    .method = HTTP_GET,  .handler = sd_get },
        { .uri = "/api/sd/put",    .method = HTTP_POST, .handler = sd_put },
        { .uri = "/api/sd/mkdir",  .method = HTTP_POST, .handler = sd_mkdir },
        { .uri = "/api/sd/del",    .method = HTTP_POST, .handler = sd_del },
        { .uri = "/*",             .method = HTTP_GET,  .handler = not_found },
        { .uri = "/*",             .method = HTTP_POST, .handler = not_found },
    };
    for (size_t i = 0; i < sizeof uris / sizeof uris[0]; i++) httpd_register_uri_handler(s_srv, &uris[i]);
    ESP_LOGI(TAG, "serveur web prêt");
    return ESP_OK;
}

void websrv_stop(void)
{
    if (!s_srv) return;
    s_stopping = true;
    ota_http_abort();
    int fd = s_busy_fd;
    if (fd >= 0) shutdown(fd, SHUT_RDWR);                   /* débloque un transfert en cours */
    httpd_stop(s_srv);
    s_srv = NULL;
    sessions_clear();
    memset(s_nonce, 0, sizeof s_nonce);
    ESP_LOGI(TAG, "serveur web arrêté");
}
