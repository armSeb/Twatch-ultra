/* mc_port.cpp — voir mc_port.h et target.h */
#include "mc_port.h"
#include "mc_hist.h"
#include "mc_text.h"
#include "target.h"
#include <SPIFFS.h>
#include <MyMesh.h>
#include <new>
#include <time.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
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
#include "settings.h"
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
    sx_set_antenna(settings_get()->lora_ext_ant);
    if (sx_start(&c, radio_rx_cb) != ESP_OK) ESP_LOGE(TAG, "radio absente");
}

void TWatchRadio::stop() { sx_stop(); }

/* plusieurs réglages arrivent d'affilée (fréquence puis puissance) : la
 * puce n'est reconfigurée qu'une fois, au tour de boucle suivant */
void TWatchRadio::apply()
{
    if (!dirty) return;
    if (sending || !sx_tx_idle()) {
        static int64_t warned;
        int64_t now = esp_timer_get_time();
        if (now - warned > 5000000) { warned = now; ESP_LOGW(TAG, "réglages radio en attente : émission en cours"); }
        return;
    }
    dirty = false;
    sx_cfg_t c = radio_cfg(freq, bw, sf, cr, power);
    if (sx_set_config(&c) != ESP_OK) ESP_LOGE(TAG, "reconfiguration radio impossible");
}

void TWatchRadio::setParams(float f, float b, uint8_t s, uint8_t c)
{
    if (f == freq && b == bw && s == sf && c == cr) return;
    freq = f; bw = b; sf = s; cr = c;
    dirty = true;
    ESP_LOGI(TAG, "réglages radio demandés : %.3f MHz, BW %.1f kHz, SF%d, CR 4/%d", f, b, s, c);
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
    ESP_LOGI(TAG, "émission demandée : %d o (type %d)", len, len ? (bytes[0] >> 2) & 0x0F : -1);
    if (!sx_send(bytes, (size_t)len)) { ESP_LOGW(TAG, "file radio pleine ou radio arrêtée"); return false; }
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
/* MeshCore suppose une carte sans RTC : au démarrage, MyMesh::begin() met
 * l'horloge à la dernière modification de contact + 1 s
 * (bootstrapRTCfromContacts), ce qui reculait l'heure de la montre (et la
 * RTC) jusqu'à la dernière annonce reçue. Une RTC valide ne recule donc
 * jamais ici ; l'app compagnon ne peut de toute façon qu'avancer l'heure
 * (CMD_SET_DEVICE_TIME refuse un temps passé), l'iPhone (CTS) la corrige
 * dans les deux sens par wclock_set_utc(). */
void TWatchClock::setCurrentTime(uint32_t t)
{
    if (wclock_time_valid() && (time_t)t < time(NULL)) return;
    wclock_set_utc((time_t)t);
}

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

/* ================================================================== app compagnon */
/* Interface « série » du protocole compagnon, par deux chemins :
 *  - Bluetooth (service NUS, ble.c) : une trame = une écriture (RX) ou une
 *    notification (TX), comme le firmware MeshCore pour ESP32 ;
 *  - TCP (mc_tcp.c, port 5000), comme SerialWifiInterface.
 * Les réponses et les avis vont à l'app qui a parlé en dernier (une seule
 * est normalement branchée) ; si elle part, l'autre prend le relais. */
typedef struct { uint8_t len; uint8_t buf[MC_FRAME_MAX]; } frame_t;
enum { SRC_BLE = 0, SRC_TCP = 1 };
static QueueHandle_t s_ble_rxq, s_tcp_rxq;
static volatile bool s_link;            /* l'app est abonnée aux notifications Bluetooth */
static volatile bool s_tcp_link;        /* une app est branchée en TCP */
static volatile uint32_t s_tcp_sess;    /* +1 à chaque connexion TCP (une nouvelle remplace l'ancienne) */

class Companion : public BaseSerialInterface {
public:
    void enable() override { enabled = true; }
    void disable() override { enabled = false; }
    bool isEnabled() const override { return enabled; }
    bool isConnected() const override { return s_link || s_tcp_link; }
    bool isWriteBusy() const override { return n >= TXQ; }
    size_t writeFrame(const uint8_t src[], size_t len) override
    {
        if (!len || len > MC_FRAME_MAX || n >= TXQ) return 0;
        int dst = target();
        if (dst < 0) return 0;
        txf_t &f = q[(head + n) % TXQ];
        f.dst = (uint8_t)dst;
        f.epoch = epoch[dst];
        f.len = (uint8_t)len;
        memcpy(f.buf, src, len);
        n++;
        return len;
    }
    size_t checkRecvFrame(uint8_t dest[]) override
    {
        bool up[2] = { s_link, s_tcp_link };
        uint32_t sess = s_tcp_sess;
        if (sess != was_sess) { was_sess = sess; if (up[SRC_TCP]) was[SRC_TCP] = false; }  /* autre app TCP */
        for (int i = 0; i < 2; i++) {
            if (up[i] == was[i]) continue;              /* l'app arrive ou repart */
            was[i] = up[i];
            epoch[i]++;                                 /* ses trames en attente sont oubliées */
            QueueHandle_t rq = i == SRC_BLE ? s_ble_rxq : s_tcp_rxq;
            if (!up[i] && rq) xQueueReset(rq);
            if (up[i]) active = (uint8_t)i;
        }
        if (!up[active] && up[!active]) active = (uint8_t)!active;
        int64_t now = esp_timer_get_time();
        /* envois : Bluetooth une notification tous les 60 ms au plus, comme
         * l'original (iOS perd des notifications envoyées en rafale) ; TCP
         * plusieurs trames d'affilée */
        for (int k = 0; k < 4 && n; k++) {
            txf_t &f = q[head];
            if (f.epoch != epoch[f.dst] || !up[f.dst]) { pop(); continue; }
            if (f.dst == SRC_TCP) {
                mc_tcp_send(f.buf, f.len);              /* échec : la connexion est coupée */
                pop();
                continue;
            }
            if (now - last_tx < 60000) break;
            if (ble_mc_notify(f.buf, f.len)) pop();
            last_tx = now;                              /* échec (mbufs) : on réessaie */
            break;
        }
        if (n) s_busy_until = now + 100000;
        static frame_t fr;
        for (int i = 0; i < 2; i++) {
            int src = (active + i) % 2;                 /* l'app active d'abord */
            QueueHandle_t rq = src == SRC_BLE ? s_ble_rxq : s_tcp_rxq;
            if (rq && xQueueReceive(rq, &fr, 0) == pdTRUE) {
                active = (uint8_t)src;
                memcpy(dest, fr.buf, fr.len);
                if (uxQueueMessagesWaiting(rq)) s_busy_until = now + 100000;
                return fr.len;
            }
        }
        return 0;
    }
    bool pending() const { return n > 0; }
private:
    typedef struct { uint8_t dst, epoch, len; uint8_t buf[MC_FRAME_MAX]; } txf_t;
    static const int TXQ = 16;
    txf_t q[TXQ];
    int head = 0, n = 0;
    int64_t last_tx = 0;
    bool enabled = false, was[2] = { false, false };
    uint8_t epoch[2] = { 0, 0 };
    uint8_t active = SRC_BLE;
    uint32_t was_sess = 0;
    void pop() { head = (head + 1) % TXQ; n--; }
    int target() const
    {
        if ((active == SRC_BLE ? s_link : s_tcp_link)) return active;
        if (s_link) return SRC_BLE;
        if (s_tcp_link) return SRC_TCP;
        return -1;
    }
};

extern "C" void mc_ble_link(bool up)
{
    s_link = up;
    s_busy_until = esp_timer_get_time() + BUSY_HOLD_MS * 1000LL;
    wake_task();
}

static bool push_rx(QueueHandle_t q, const uint8_t *buf, size_t len)
{
    if (!q || !len || len > MC_FRAME_MAX) return false;
    frame_t f;
    f.len = (uint8_t)len;
    memcpy(f.buf, buf, len);
    if (xQueueSend(q, &f, 0) != pdTRUE) return false;
    s_busy_until = esp_timer_get_time() + BUSY_HOLD_MS * 1000LL;
    wake_task();
    return true;
}

extern "C" bool mc_ble_rx(const uint8_t *buf, size_t len) { return push_rx(s_ble_rxq, buf, len); }

extern "C" void mc_tcp_link(bool up)
{
    if (up) s_tcp_sess = s_tcp_sess + 1;
    s_tcp_link = up;
    s_busy_until = esp_timer_get_time() + BUSY_HOLD_MS * 1000LL;
    wake_task();
}

extern "C" bool mc_tcp_rx(const uint8_t *buf, size_t len) { return push_rx(s_tcp_rxq, buf, len); }

/* ================================================================== historique montre */
/* (mc_hist.c) : chaque message reçu ou envoyé depuis l'app y est copié */
static void fill_pkt(mc_msg_t &m, mesh::Packet *pkt)
{
    m.snr_q4 = pkt->_snr;
    m.hops = pkt->isRouteFlood() ? mc_hops_of((uint8_t)pkt->path_len) : 0xFF;
}

static void channel_peer(const ChannelDetails &ch, uint8_t out[MC_PEER_LEN])
{
    mc_channel_peer(ch.channel.secret, sizeof ch.channel.secret, out);
}

class WatchListener : public MyMesh::Listener {
public:
    void onMessageRecv(mesh::Packet *pkt, const ContactInfo &from, uint8_t txt_type,
                       uint32_t ts, const char *text) override
    {
        if (txt_type != TXT_TYPE_PLAIN && txt_type != TXT_TYPE_SIGNED_PLAIN) return;
        static mc_msg_t m;                          /* tâche mesh seule */
        memset(&m, 0, sizeof m);
        m.kind = MC_CONV_CONTACT;
        m.ts = ts;
        fill_pkt(m, pkt);
        memcpy(m.peer, from.id.pub_key, MC_PEER_LEN);
        mc_copy_utf8(m.from, sizeof m.from, from.name, sizeof from.name);
        mc_copy_utf8(m.text, sizeof m.text, text, SIZE_MAX);
        mch_add(&m, true);
    }
    void onChannelMessageRecv(mesh::Packet *pkt, ChannelDetails &ch, const char *text) override
    {
        static mc_msg_t m;
        memset(&m, 0, sizeof m);
        m.kind = MC_CONV_CHANNEL;                   /* horodatage : pris dans la trame */
        fill_pkt(m, pkt);
        channel_peer(ch, m.peer);
        mc_copy_utf8(m.from, sizeof m.from, ch.name, sizeof ch.name);
        mc_copy_utf8(m.text, sizeof m.text, text, SIZE_MAX);
        mch_add(&m, true);
    }
    void onOfflineFrameQueued(const uint8_t *frame, int len) override { mch_frame_queued(frame, len); }
    void onOfflineFrameSynced(const uint8_t *frame, int len) override { mch_frame_synced(frame, len); }
    void onAppMessageSent(const ContactInfo *to, const ChannelDetails *ch, uint32_t ts,
                          const char *text, int len) override
    {
        if ((!to && !ch) || len <= 0) return;
        static mc_msg_t m;
        memset(&m, 0, sizeof m);
        m.ts = ts;
        m.flags = MC_F_OUT | MC_F_READ | MC_F_APP;
        m.hops = 0xFF;
        if (to) {
            m.kind = MC_CONV_CONTACT;
            memcpy(m.peer, to->id.pub_key, MC_PEER_LEN);
            mc_copy_utf8(m.from, sizeof m.from, to->name, sizeof to->name);
        } else {
            m.kind = MC_CONV_CHANNEL;
            channel_peer(*ch, m.peer);
            mc_copy_utf8(m.from, sizeof m.from, ch->name, sizeof ch->name);
        }
        mc_copy_utf8(m.text, sizeof m.text, text, (size_t)len);
        mch_add(&m, false);
    }
    void onQueueSizeChanged(int n) override { }
    void onACKRecv(uint32_t ack_crc) override;
    void onFloodHeard(const mesh::Packet *pkt) override;
};

/* ================================================================== mesh */
/* MeshCore n'est construit qu'une fois et vit en PSRAM : un arrêt coupe la
 * radio et la tâche, l'état (contacts, file de l'app) reste en mémoire. */
alignas(MyMesh) EXT_RAM_BSS_ATTR static uint8_t s_mesh_mem[sizeof(MyMesh)];
alignas(DataStore) EXT_RAM_BSS_ATTR static uint8_t s_store_mem[sizeof(DataStore)];
alignas(SimpleMeshTables) EXT_RAM_BSS_ATTR static uint8_t s_tables_mem[sizeof(SimpleMeshTables)];
static MyMesh *s_mesh;
static DataStore *s_store;
static Companion *s_ble;
static WatchListener s_listener;
static StdRNG s_fast_rng;
static int64_t s_offq_dirty_at;

static portMUX_TYPE s_st_mux = portMUX_INITIALIZER_UNLOCKED;
static mc_status_t s_st;

static void update_status(void)
{
    mc_status_t t = {};
    NodePrefs *p = s_mesh->getNodePrefs();
    mc_copy_utf8(t.name, sizeof t.name, p->node_name, sizeof p->node_name);
    t.freq_mhz = p->freq;
    t.bw_khz = p->bw;
    t.sf = p->sf;
    t.cr = p->cr;
    t.tx_dbm = p->tx_power_dbm;
    t.contacts = s_mesh->getNumContacts();
    t.app_queue = s_mesh->getOfflineQueueLen();
    t.app_connected = s_link || s_tcp_link;
    t.tcp_connected = s_tcp_link;
    t.repeat = p->isRepeatEn();
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

/* Messages encore dans la file de l'app mais absents de l'historique de la
 * montre (mise à jour depuis un ancien format, historique perdu) : copiés
 * dans l'historique, non lus. Format des trames : voir MyMesh::queueMessage
 * et onChannelMessageRecv. */
static void import_queue(void)
{
    static mc_msg_t m;
    static ChannelDetails ch;
    int got = 0;
    for (int i = 0; i < s_mesh->getOfflineQueueLen(); i++) {
        const uint8_t *f;
        int len = s_mesh->getOfflineQueueFrame(i, &f);
        uint8_t kind;
        uint32_t ts;
        if (!mc_frame_info(f, len, &kind, &ts)) continue;
        uint32_t k = mc_fnv32(f, (size_t)len);
        if (mch_has_qkey(k)) continue;
        bool v3 = f[0] == 16 || f[0] == 17;
        int at = v3 ? 4 : 1;                        /* clé du contact ou n° de canal */
        memset(&m, 0, sizeof m);
        m.kind = kind;
        m.ts = ts;
        m.qkey = k;
        m.snr_q4 = v3 ? (int8_t)f[1] : 0;
        int text_at;
        if (kind == MC_CONV_CONTACT) {
            uint8_t type = f[at + 6 + 1];
            if (type != TXT_TYPE_PLAIN && type != TXT_TYPE_SIGNED_PLAIN) continue;
            m.hops = mc_hops_of(f[at + 6]);
            text_at = at + 6 + 2 + 4 + (type == TXT_TYPE_SIGNED_PLAIN ? 4 : 0);
            memcpy(m.peer, f + at, MC_PEER_LEN);
            ContactInfo *c = s_mesh->lookupContactByPubKey(f + at, MC_PEER_LEN);
            if (c) mc_copy_utf8(m.from, sizeof m.from, c->name, sizeof c->name);
            else snprintf(m.from, sizeof m.from, "%02x%02x%02x", f[at], f[at + 1], f[at + 2]);
        } else {
            if (!s_mesh->getChannel(f[at], ch) || !ch.name[0]) continue;
            m.hops = mc_hops_of(f[at + 1]);
            text_at = at + 1 + 2 + 4;
            channel_peer(ch, m.peer);
            mc_copy_utf8(m.from, sizeof m.from, ch.name, sizeof ch.name);
        }
        if (text_at >= len) continue;
        mc_copy_utf8(m.text, sizeof m.text, (const char *)f + text_at, (size_t)(len - text_at));
        if (mch_import(&m)) got++;
    }
    if (got) ESP_LOGI(TAG, "%d message(s) de la file de l'app copiés dans l'historique", got);
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

/* contacts (discussion, salons) et canaux connus, pour les onglets de l'app */
static void peers_snapshot(void)
{
    EXT_RAM_BSS_ATTR static mch_peer_t p[MCH_PEERS_MAX];    /* tâche mesh seule */
    static ContactInfo c;
    static ChannelDetails ch;
    int n = 0, nc = s_mesh->getNumContacts() + MAX_ANON_CONTACTS;   /* index bruts : réservés en tête */
    for (int i = 0; i < nc && n < MCH_PEERS_MAX; i++) {
        if (!s_mesh->getContactByIdx(i, c) || (c.type != ADV_TYPE_CHAT && c.type != ADV_TYPE_ROOM)) continue;
        p[n].pub = false;
        p[n].kind = MC_CONV_CONTACT;
        memcpy(p[n].peer, c.id.pub_key, MC_PEER_LEN);
        mc_copy_utf8(p[n].name, sizeof p[n].name, c.name, sizeof c.name);
        n++;
    }
    static const uint8_t zero[sizeof ch.channel.secret] = {};
    /* clé du canal public de MeshCore (« izOH6cXN6mrJ5e26oRXNcg== ») */
    static const uint8_t pub_key[16] = { 0x8b, 0x33, 0x87, 0xe9, 0xc5, 0xcd, 0xea, 0x6a,
                                         0xc9, 0xe5, 0xed, 0xba, 0xa1, 0x15, 0xcd, 0x72 };
    for (int i = 0; i < MAX_GROUP_CHANNELS && n < MCH_PEERS_MAX; i++) {
        if (!s_mesh->getChannel(i, ch) || !ch.name[0] || !memcmp(ch.channel.secret, zero, sizeof zero)) continue;
        p[n].pub = !memcmp(ch.channel.secret, pub_key, sizeof pub_key) &&
                   !memcmp(ch.channel.secret + sizeof pub_key, zero, sizeof ch.channel.secret - sizeof pub_key);
        p[n].kind = MC_CONV_CHANNEL;
        channel_peer(ch, p[n].peer);
        mc_copy_utf8(p[n].name, sizeof p[n].name, ch.name, sizeof ch.name);
        n++;
    }
    mch_peers_set(p, n);
}


/* ================================================================== envoi depuis la montre */
/* Demandes de la tâche app, exécutées par la tâche mesh. Les envois en
 * cours (accusé ou relais attendus) vivent dans s_pend, tâche mesh seule. */
enum { REQ_TEXT, REQ_RESEND, REQ_ADVERT };
typedef struct {
    uint8_t  op, kind;
    uint8_t  peer[MC_PEER_LEN];
    bool     flood;
    uint32_t seq;
    char     text[MC_TEXT_MAX + 1];
} send_req_t;

#define PEND_N         8
#define TRIES_MAX      5
#define RELAY_WAIT_MS  60000    /* canal : relais comptés une minute */
#define ACK_MARGIN_MS  2000     /* privé : marge sur le délai estimé par MeshCore */

typedef struct {
    bool     used;
    uint8_t  kind;
    uint8_t  attempt, tries;
    uint8_t  relays;
    uint8_t  peer[MC_PEER_LEN];
    uint8_t  hash[MAX_HASH_SIZE];   /* canal : empreinte du paquet émis */
    uint32_t seq, ts;
    uint32_t acks[TRIES_MAX];       /* privé : accusé attendu de chaque tentative */
    int64_t  sent_us, deadline_us;
    char     text[MC_TEXT_MAX + 1];
} pend_t;

static QueueHandle_t s_cmdq;
EXT_RAM_BSS_ATTR static pend_t s_pend[PEND_N];
static int s_npend_chan;                /* envois de canal qui attendent des relais */

static uint8_t tries_setting(void)
{
    uint8_t t = settings_get()->mesh_tries;
    return t < 1 ? 1 : t > TRIES_MAX ? TRIES_MAX : t;
}

static int find_channel(const uint8_t peer[MC_PEER_LEN], ChannelDetails &ch)
{
    uint8_t p[MC_PEER_LEN];
    for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
        if (!s_mesh->getChannel(i, ch) || !ch.name[0]) continue;
        mc_channel_peer(ch.channel.secret, sizeof ch.channel.secret, p);
        if (!memcmp(p, peer, MC_PEER_LEN)) return i;
    }
    return -1;
}

static void pend_state(pend_t &p, uint8_t flags, int8_t rtt)
{
    uint8_t hops = p.kind == MC_CONV_CHANNEL ? p.relays : p.attempt;
    mch_set_send_state(p.seq, flags, hops, rtt);
}

static void pend_fail(pend_t &p)
{
    pend_state(p, MC_F_FAIL, 0);
    if (p.kind == MC_CONV_CHANNEL && s_npend_chan > 0) s_npend_chan--;
    p.used = false;
}

/* une tentative (privé) ; false : impossible, l'envoi est en échec */
static bool dm_attempt(pend_t &p)
{
    ContactInfo *c = s_mesh->lookupContactByPubKey(p.peer, MC_PEER_LEN);
    if (!c) { ESP_LOGW(TAG, "envoi : contact inconnu"); return false; }
    /* dernière tentative, chemin direct resté muet : on repart en inondation
     * (l'app iPhone fait de même) */
    if (p.attempt > 0 && p.attempt + 1 == p.tries && c->out_path_len != OUT_PATH_UNKNOWN) {
        ESP_LOGI(TAG, "envoi : chemin direct sans réponse, inondation");
        s_mesh->resetPathTo(*c);
    }
    uint32_t ack = 0, est = 0;
    s_mesh->twatchScopeBegin();
    int r = s_mesh->sendMessage(*c, p.ts, p.attempt, p.text, ack, est);
    s_mesh->twatchScopeEnd();
    if (r == MSG_SEND_FAILED) { ESP_LOGW(TAG, "envoi : file radio pleine"); return false; }
    p.acks[p.attempt] = ack;
    p.sent_us = esp_timer_get_time();
    p.deadline_us = p.sent_us + (int64_t)(est + ACK_MARGIN_MS) * 1000;
    ESP_LOGI(TAG, "privé : tentative %u/%u (%s), accusé attendu sous %lu ms", p.attempt + 1, p.tries,
             r == MSG_SEND_SENT_FLOOD ? "inondation" : "direct", (unsigned long)est);
    pend_state(p, MC_F_PEND, 0);
    return true;
}

static bool chan_send(pend_t &p)
{
    static ChannelDetails ch;
    if (find_channel(p.peer, ch) < 0) { ESP_LOGW(TAG, "envoi : canal inconnu"); return false; }
    NodePrefs *np = s_mesh->getNodePrefs();
    s_mesh->twatchScopeBegin();
    bool ok = s_mesh->sendGroupMessage(p.ts, ch.channel, np->node_name, p.text, (int)strlen(p.text));
    s_mesh->twatchScopeEnd();
    if (!ok) return false;
    memcpy(p.hash, s_mesh->twatchLastFloodHash(), sizeof p.hash);
    p.sent_us = esp_timer_get_time();
    p.deadline_us = p.sent_us + RELAY_WAIT_MS * 1000LL;
    p.relays = 0;
    s_npend_chan++;
    pend_state(p, MC_F_PEND, 0);
    return true;
}

static pend_t *pend_alloc(void)
{
    for (int i = 0; i < PEND_N; i++) if (!s_pend[i].used) return &s_pend[i];
    return NULL;
}

static void pend_start(pend_t *p)
{
    p->used = true;
    p->attempt = 0;
    p->tries = p->kind == MC_CONV_CHANNEL ? 1 : tries_setting();
    memset(p->acks, 0, sizeof p->acks);
    bool ok = p->kind == MC_CONV_CHANNEL ? chan_send(*p) : dm_attempt(*p);
    if (!ok) {
        pend_state(*p, MC_F_FAIL, 0);
        p->used = false;
    }
}

static void do_request(const send_req_t &r)
{
    static mc_msg_t m;
    if (r.op == REQ_ADVERT) {
        bool ok = s_mesh->twatchAdvert(r.flood);
        ESP_LOGI(TAG, "annonce %s %s", r.flood ? "flood" : "0 saut", ok ? "envoyée" : "impossible");
        return;
    }
    pend_t *p = pend_alloc();
    if (r.op == REQ_RESEND) {
        if (!mch_get(r.seq, &m) || (m.flags & (MC_F_WATCH | MC_F_FAIL)) != (MC_F_WATCH | MC_F_FAIL)) return;
        for (int i = 0; i < PEND_N; i++) if (s_pend[i].used && s_pend[i].seq == r.seq) return;
        if (!p) return;
        memset(p, 0, sizeof *p);
        p->seq = m.seq;
        p->kind = m.kind;
        memcpy(p->peer, m.peer, MC_PEER_LEN);
        memcpy(p->text, m.text, sizeof p->text);
        /* nouvel horodatage : même paquet sinon, ignoré comme doublon */
        p->ts = s_mesh->getRTCClock()->getCurrentTimeUnique();
        pend_start(p);
        return;
    }
    /* nouveau message : dans l'historique d'abord (la conversation l'affiche
     * tout de suite), puis émis */
    memset(&m, 0, sizeof m);
    m.kind = r.kind;
    m.ts = s_mesh->getRTCClock()->getCurrentTimeUnique();
    m.flags = MC_F_OUT | MC_F_READ | MC_F_WATCH | (p ? MC_F_PEND : MC_F_FAIL);
    m.hops = 0;
    memcpy(m.peer, r.peer, MC_PEER_LEN);
    if (r.kind == MC_CONV_CONTACT) {
        ContactInfo *c = s_mesh->lookupContactByPubKey(r.peer, MC_PEER_LEN);
        if (c) mc_copy_utf8(m.from, sizeof m.from, c->name, sizeof c->name);
    } else {
        static ChannelDetails ch;
        if (find_channel(r.peer, ch) >= 0) mc_copy_utf8(m.from, sizeof m.from, ch.name, sizeof ch.name);
    }
    mc_copy_utf8(m.text, sizeof m.text, r.text, sizeof r.text);
    mch_add(&m, false);
    if (!p) { ESP_LOGW(TAG, "envoi : trop d'envois en cours"); return; }
    memset(p, 0, sizeof *p);
    p->seq = m.seq;
    p->kind = m.kind;
    p->ts = m.ts;
    memcpy(p->peer, m.peer, MC_PEER_LEN);
    memcpy(p->text, m.text, sizeof p->text);
    pend_start(p);
}

/* tâche mesh, à chaque tour */
static void pend_tick(int64_t now)
{
    if (s_cmdq) {
        EXT_RAM_BSS_ATTR static send_req_t r;
        while (xQueueReceive(s_cmdq, &r, 0) == pdTRUE) do_request(r);
    }
    for (int i = 0; i < PEND_N; i++) {
        pend_t &p = s_pend[i];
        if (!p.used || now < p.deadline_us) continue;
        if (p.kind == MC_CONV_CHANNEL) {
            if (p.relays) {
                if (s_npend_chan > 0) s_npend_chan--;
                p.used = false;                 /* état ACK déjà posé */
            } else {
                pend_fail(p);
            }
            continue;
        }
        if (p.attempt + 1 < p.tries) {
            p.attempt++;
            if (!dm_attempt(p)) pend_fail(p);
        } else {
            ESP_LOGI(TAG, "privé : pas d'accusé après %u tentative(s)", p.tries);
            pend_fail(p);
        }
    }
}

void WatchListener::onACKRecv(uint32_t ack_crc)
{
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < PEND_N; i++) {
        pend_t &p = s_pend[i];
        if (!p.used || p.kind != MC_CONV_CONTACT) continue;
        for (int a = 0; a <= p.attempt && a < TRIES_MAX; a++) {
            if (!p.acks[a] || p.acks[a] != ack_crc) continue;
            int64_t rtt = (now - p.sent_us) / 100000;           /* 1/10 s */
            pend_state(p, MC_F_ACK, (int8_t)(rtt > 127 ? 127 : rtt < 1 ? 1 : rtt));
            ESP_LOGI(TAG, "privé : reçu (tentative %u)", a + 1);
            p.used = false;
            return;
        }
    }
}

void WatchListener::onFloodHeard(const mesh::Packet *pkt)
{
    if (!s_npend_chan || pkt->getPayloadType() != PAYLOAD_TYPE_GRP_TXT) return;
    uint8_t h[MAX_HASH_SIZE];
    pkt->calculatePacketHash(h);
    for (int i = 0; i < PEND_N; i++) {
        pend_t &p = s_pend[i];
        if (!p.used || p.kind != MC_CONV_CHANNEL || memcmp(p.hash, h, sizeof h)) continue;
        if (p.relays < 250) p.relays++;
        pend_state(p, MC_F_ACK, 0);
        ESP_LOGI(TAG, "canal : relais entendu (%u)", p.relays);
        return;
    }
}

static bool post_req(const send_req_t &r)
{
    if (!s_run || !s_cmdq) return false;
    if (xQueueSend(s_cmdq, &r, 0) != pdTRUE) return false;
    s_busy_until = esp_timer_get_time() + BUSY_HOLD_MS * 1000LL;
    wake_task();
    return true;
}

extern "C" int mc_text_max(uint8_t kind)
{
    if (kind != MC_CONV_CHANNEL) return MAX_TEXT_LEN - 2;      /* octet de tentative au-delà de 3 */
    mc_status_t st;
    mc_get_status(&st);
    int n = MAX_TEXT_LEN - (int)strlen(st.name) - 2;           /* « nom: » */
    return n < 0 ? 0 : n > MC_TEXT_MAX ? MC_TEXT_MAX : n;
}

extern "C" bool mc_send_text(uint8_t kind, const uint8_t peer[MC_PEER_LEN], const char *text)
{
    EXT_RAM_BSS_ATTR static send_req_t r;       /* tâche app seule */
    if (!text || !text[0] || (int)strlen(text) > mc_text_max(kind)) return false;
    memset(&r, 0, sizeof r);
    r.op = REQ_TEXT;
    r.kind = kind == MC_CONV_CHANNEL ? MC_CONV_CHANNEL : MC_CONV_CONTACT;
    memcpy(r.peer, peer, MC_PEER_LEN);
    mc_copy_utf8(r.text, sizeof r.text, text, SIZE_MAX);
    return post_req(r);
}

extern "C" bool mc_resend(uint32_t seq)
{
    EXT_RAM_BSS_ATTR static send_req_t r;
    memset(&r, 0, sizeof r);
    r.op = REQ_RESEND;
    r.seq = seq;
    return post_req(r);
}

extern "C" bool mc_send_advert(bool flood)
{
    EXT_RAM_BSS_ATTR static send_req_t r;
    memset(&r, 0, sizeof r);
    r.op = REQ_ADVERT;
    r.flood = flood;
    return post_req(r);
}

static void mesh_task(void *arg)
{
    radio_driver.start();
    int64_t beat = esp_timer_get_time();
    while (s_run) {
        if (esp_timer_get_time() - beat > 120000000) {     /* toutes les 2 min : marge de pile, RAM */
            beat = esp_timer_get_time();
            ESP_LOGI(TAG, "pile libre %u o, RAM interne libre %u o (bloc %u o)",
                     (unsigned)uxTaskGetStackHighWaterMark(NULL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        }
        s_mesh->loop();
        rtc_clock.tick();

        int64_t now = esp_timer_get_time();
        pend_tick(now);
        if (s_mesh->isOfflineQueueDirty()) {
            if (!s_offq_dirty_at) s_offq_dirty_at = now;
            /* l'app vide la file : on attend la fin de la synchro */
            if (now - s_offq_dirty_at >= OFFQ_SAVE_MS * 1000LL && !s_ble->pending()) offq_save();
        }
        mch_save_if_due(now);
        if (mch_peers_wanted()) peers_snapshot();
        update_status();

        bool busy = now < s_busy_until || s_mesh->hasPendingWork() || s_ble->pending() ||
                    (s_cmdq && uxQueueMessagesWaiting(s_cmdq)) ||
                    !sx_tx_idle() || (s_rxq && uxQueueMessagesWaiting(s_rxq));
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(busy ? BUSY_TICK_MS : IDLE_TICK_MS));
    }
    if (s_mesh->isOfflineQueueDirty()) offq_save();
    mch_save_now();
    radio_driver.stop();
    s_task = NULL;
    vTaskDelete(NULL);
}

extern "C" esp_err_t mc_start(void)
{
    if (s_run) return ESP_OK;
    if (!s_mesh) {
        if (!SPIFFS.begin(true)) return ESP_FAIL;
        /* tout ce que MeshCore alloue ici (16 paquets, files, tampon BLE)
         * n'est touché que par les tâches mesh et lora : en PSRAM. Le seuil
         * global (petits blocs en interne) est rétabli juste après. */
        heap_caps_malloc_extmem_enable(16);
        mch_init();
        s_rxq = xQueueCreate(RXQ_LEN, sizeof(rxpkt_t));
        s_ble_rxq = xQueueCreate(6, sizeof(frame_t));
        s_tcp_rxq = xQueueCreate(6, sizeof(frame_t));
        s_cmdq = xQueueCreate(4, sizeof(send_req_t));
        s_ble = new (std::nothrow) Companion();
        if (!s_rxq || !s_ble_rxq || !s_tcp_rxq || !s_cmdq || !s_ble) {
            heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);
            return ESP_ERR_NO_MEM;
        }
        auto *tables = new (s_tables_mem) SimpleMeshTables();
        s_store = new (s_store_mem) DataStore(SPIFFS, rtc_clock);
        s_mesh = new (s_mesh_mem) MyMesh(radio_driver, s_fast_rng, rtc_clock, *tables, *s_store);
        s_store->begin();
        s_mesh->begin();                    /* identité, préférences, contacts, canaux */
        offq_load();
        mch_load();
        import_queue();
        s_mesh->startInterface(*s_ble);
        s_mesh->setListener(&s_listener);
        heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);
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
