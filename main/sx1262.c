/* sx1262.c — voir sx1262.h. Commandes d'après la fiche Semtech SX1261/2 (rév. 2.1). */
#include "sx1262.h"
#include "esp_attr.h"
#include "task_psram.h"
#include <string.h>
#include "board.h"
#include "axp2101.h"
#include "xl9555.h"
#include "spi_bus.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "sx1262";

/* commandes */
#define C_SET_SLEEP        0x84
#define C_SET_STANDBY      0x80
#define C_SET_TX           0x83
#define C_SET_RX           0x82
#define C_SET_REGULATOR    0x96
#define C_CALIBRATE        0x89
#define C_CALIBRATE_IMAGE  0x98
#define C_SET_PA_CONFIG    0x95
#define C_WRITE_REG        0x0D
#define C_READ_REG         0x1D
#define C_WRITE_BUF        0x0E
#define C_READ_BUF         0x1E
#define C_SET_DIO_IRQ      0x08
#define C_GET_IRQ          0x12
#define C_CLR_IRQ          0x02
#define C_DIO2_RF_SW       0x9D
#define C_DIO3_TCXO        0x97
#define C_SET_RF_FREQ      0x86
#define C_SET_PKT_TYPE     0x8A
#define C_SET_TX_PARAMS    0x8E
#define C_SET_MOD_PARAMS   0x8B
#define C_SET_PKT_PARAMS   0x8C
#define C_SET_BUF_BASE     0x8F
#define C_GET_RX_BUF       0x13
#define C_GET_PKT_STATUS   0x14
#define C_GET_RSSI_INST    0x15
#define C_GET_STATUS       0xC0
#define C_GET_ERRORS       0x17
#define C_CLR_ERRORS       0x07

/* registres */
#define R_SYNC_WORD        0x0740
#define R_IQ_POLARITY      0x0736
#define R_RX_GAIN          0x08AC
#define R_OCP              0x08E7
#define R_TX_CLAMP         0x08D8

/* IRQ */
#define IRQ_TX_DONE        0x0001
#define IRQ_RX_DONE        0x0002
#define IRQ_PREAMBLE       0x0004
#define IRQ_HEADER_VALID   0x0010
#define IRQ_HEADER_ERR     0x0020
#define IRQ_CRC_ERR        0x0040
#define IRQ_TIMEOUT        0x0200

#define TXQ_LEN 6
typedef struct { uint8_t len; uint8_t buf[SX_MAX_PKT]; } txreq_t;

static spi_device_handle_t s_dev;
static TaskHandle_t s_task;
static QueueHandle_t s_txq;
static volatile bool s_run;
static sx_cfg_t s_cfg;
static sx_rx_cb_t s_cb;
static sx_stats_t s_stats;
static int16_t s_noise = INT16_MIN;
static volatile int64_t s_rx_busy_until;   /* préambule ou en-tête vu : réception en cours */
static volatile bool s_tx_busy;            /* paquet sorti de la file, pas encore émis */
static bool s_ext_ant;
static esp_err_t s_last_err = ESP_OK;      /* dernier échec de démarrage */                     /* antenne externe (connecteur USB-C) */

/* ------------------------------------------------------------------ SPI */
static bool wait_busy(int ms)
{
    int64_t end = esp_timer_get_time() + ms * 1000LL;
    while (gpio_get_level(BOARD_LORA_BUSY)) {
        if (esp_timer_get_time() > end) return false;
        if (ms > 2) vTaskDelay(1); else esp_rom_delay_us(10);
    }
    return true;
}

static esp_err_t xfer(const uint8_t *tx, uint8_t *rx, size_t n)
{
    if (!wait_busy(20)) { ESP_LOGW(TAG, "BUSY bloqué"); return ESP_ERR_TIMEOUT; }
    spi_transaction_t t = { .length = n * 8, .tx_buffer = tx, .rx_buffer = rx };
    return spi_device_polling_transmit(s_dev, &t);
}

static esp_err_t cmd(uint8_t op, const uint8_t *arg, size_t n)
{
    uint8_t b[16];
    if (n + 1 > sizeof b) return ESP_ERR_INVALID_SIZE;
    b[0] = op;
    if (n) memcpy(b + 1, arg, n);
    return xfer(b, NULL, n + 1);
}

/* commande à réponse : op, NOP de statut, puis n octets */
static esp_err_t cmd_read(uint8_t op, uint8_t *out, size_t n)
{
    uint8_t tx[8] = { op }, rx[8];
    if (n + 2 > sizeof tx) return ESP_ERR_INVALID_SIZE;
    esp_err_t e = xfer(tx, rx, n + 2);
    if (e == ESP_OK) memcpy(out, rx + 2, n);
    return e;
}

static esp_err_t reg_write(uint16_t addr, const uint8_t *v, size_t n)
{
    uint8_t b[8] = { C_WRITE_REG, addr >> 8, addr & 0xFF };
    if (n + 3 > sizeof b) return ESP_ERR_INVALID_SIZE;
    memcpy(b + 3, v, n);
    return xfer(b, NULL, n + 3);
}

static uint8_t reg_read8(uint16_t addr)
{
    uint8_t tx[5] = { C_READ_REG, addr >> 8, addr & 0xFF, 0, 0 }, rx[5] = { 0 };
    xfer(tx, rx, 5);
    return rx[4];
}

static void reg_write8(uint16_t addr, uint8_t v) { reg_write(addr, &v, 1); }

static void irq_clear(uint16_t m) { uint8_t a[2] = { m >> 8, m & 0xFF }; cmd(C_CLR_IRQ, a, 2); }

static uint16_t irq_get(void)
{
    uint8_t v[2] = { 0 };
    cmd_read(C_GET_IRQ, v, 2);
    return (uint16_t)(v[0] << 8 | v[1]);
}

static uint8_t status(void)
{
    uint8_t tx[2] = { C_GET_STATUS, 0 }, rx[2] = { 0 };
    xfer(tx, rx, 2);
    return rx[1];
}

static uint16_t errors(void)
{
    uint8_t v[2] = { 0 };
    cmd_read(C_GET_ERRORS, v, 2);
    return (uint16_t)(v[0] << 8 | v[1]);
}

static void standby(void) { uint8_t a = 0; cmd(C_SET_STANDBY, &a, 1); }   /* RC 13 MHz */

/* ------------------------------------------------------------------ configuration */
static uint8_t bw_code(uint32_t bw)
{
    static const struct { uint32_t hz; uint8_t c; } T[] = {
        { 7800, 0x00 }, { 10400, 0x08 }, { 15600, 0x01 }, { 20800, 0x09 }, { 31250, 0x02 },
        { 41700, 0x0A }, { 62500, 0x03 }, { 125000, 0x04 }, { 250000, 0x05 }, { 500000, 0x06 },
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++)
        if (bw <= T[i].hz + T[i].hz / 50) return T[i].c;
    return 0x06;
}

static float symbol_ms(void) { return (float)(1u << s_cfg.sf) * 1000.0f / (float)s_cfg.bw_hz; }

static void set_packet(uint8_t len)
{
    uint8_t p[6] = { s_cfg.preamble >> 8, s_cfg.preamble & 0xFF, 0x00 /* en-tête explicite */,
                     len, 0x01 /* CRC */, 0x00 /* IQ standard */ };
    cmd(C_SET_PKT_PARAMS, p, 6);
}

static esp_err_t apply(void)
{
    standby();
    uint8_t t = 0x01;                                          /* LoRa */
    cmd(C_SET_PKT_TYPE, &t, 1);

    /* calibration de l'image pour la bande */
    uint32_t f = s_cfg.freq_hz;
    uint8_t ci[2];
    if (f > 900000000)      { ci[0] = 0xE1; ci[1] = 0xE9; }
    else if (f > 850000000) { ci[0] = 0xD7; ci[1] = 0xDB; }
    else if (f > 770000000) { ci[0] = 0xC1; ci[1] = 0xC5; }
    else if (f > 460000000) { ci[0] = 0x75; ci[1] = 0x81; }
    else                    { ci[0] = 0x6B; ci[1] = 0x6F; }
    cmd(C_CALIBRATE_IMAGE, ci, 2);

    uint32_t r = (uint32_t)(((uint64_t)f << 25) / 32000000ULL);
    uint8_t fr[4] = { r >> 24, r >> 16, r >> 8, r };
    cmd(C_SET_RF_FREQ, fr, 4);

    /* PA haute puissance (+22 dBm), puis limite de courant 140 mA */
    uint8_t pa[4] = { 0x04, 0x07, 0x00, 0x01 };
    cmd(C_SET_PA_CONFIG, pa, 4);
    reg_write8(R_OCP, 0x38);
    int p = s_cfg.power_dbm < -9 ? -9 : s_cfg.power_dbm > 22 ? 22 : s_cfg.power_dbm;
    uint8_t tp[2] = { (uint8_t)(int8_t)p, 0x04 };              /* rampe 200 µs */
    cmd(C_SET_TX_PARAMS, tp, 2);

    bool ldro = symbol_ms() >= 16.0f;
    uint8_t mp[4] = { s_cfg.sf, bw_code(s_cfg.bw_hz), (uint8_t)(s_cfg.cr - 4), ldro };
    cmd(C_SET_MOD_PARAMS, mp, 4);
    set_packet(SX_MAX_PKT);

    /* errata 15.4 : polarité IQ standard → bit 2 du registre 0x0736 à 1 */
    reg_write8(R_IQ_POLARITY, reg_read8(R_IQ_POLARITY) | 0x04);

    uint8_t sw = s_cfg.sync_word;
    uint8_t swr[2] = { (uint8_t)((sw & 0xF0) | 0x04), (uint8_t)(((sw & 0x0F) << 4) | 0x04) };
    reg_write(R_SYNC_WORD, swr, 2);

    reg_write8(R_RX_GAIN, 0x96);                               /* réception à gain renforcé */

    uint8_t base[2] = { 0, 0 };
    cmd(C_SET_BUF_BASE, base, 2);

    uint16_t m = IRQ_TX_DONE | IRQ_RX_DONE | IRQ_PREAMBLE | IRQ_HEADER_VALID | IRQ_HEADER_ERR |
                 IRQ_CRC_ERR | IRQ_TIMEOUT;
    uint16_t d1 = IRQ_TX_DONE | IRQ_RX_DONE | IRQ_HEADER_VALID | IRQ_CRC_ERR | IRQ_HEADER_ERR | IRQ_TIMEOUT;
    uint8_t ip[8] = { m >> 8, m & 0xFF, d1 >> 8, d1 & 0xFF, 0, 0, 0, 0 };
    return cmd(C_SET_DIO_IRQ, ip, 8);
}

static void rx_continuous(void)
{
    set_packet(SX_MAX_PKT);
    irq_clear(0xFFFF);
    uint8_t to[3] = { 0xFF, 0xFF, 0xFF };
    cmd(C_SET_RX, to, 3);
}

uint32_t sx_airtime_ms(size_t len)
{
    /* fiche Semtech §6.1.4 : en-tête explicite, CRC */
    float ts = symbol_ms();
    int sf = s_cfg.sf ? s_cfg.sf : 8;
    bool ldro = ts >= 16.0f;
    int num = 8 * (int)len - 4 * sf + 28 + 16;
    int den = 4 * (sf - 2 * ldro);
    int n = num > 0 ? (num + den - 1) / den : 0;
    float sym = (s_cfg.preamble + 4.25f) + 8 + n * (s_cfg.cr);
    return (uint32_t)(sym * ts + 0.5f);
}

/* ------------------------------------------------------------------ tâche */
/* DIO1 reste haute tant que l'IRQ n'est pas acquittée : interruption en
 * niveau (aussi source de réveil du light sleep), masquée ici et rétablie
 * par la tâche après l'acquittement */
static void IRAM_ATTR dio1_isr(void *arg)
{
    BaseType_t hp = pdFALSE;
    gpio_intr_disable(BOARD_LORA_IRQ);
    if (s_task) vTaskNotifyGiveFromISR(s_task, &hp);
    if (hp) portYIELD_FROM_ISR();
}

static void handle_rx(uint16_t irq)
{
    if (irq & (IRQ_CRC_ERR | IRQ_HEADER_ERR)) {
        s_stats.rx_crc++;
        ESP_LOGW(TAG, "paquet reçu corrompu (%s)", irq & IRQ_CRC_ERR ? "CRC" : "en-tête");
        return;
    }
    if (!(irq & IRQ_RX_DONE)) return;
    uint8_t st[2], ps[3];
    cmd_read(C_GET_RX_BUF, st, 2);
    cmd_read(C_GET_PKT_STATUS, ps, 3);
    uint8_t len = st[0], off = st[1];
    if (!len) return;
    static uint8_t tx[SX_MAX_PKT + 3], rx[SX_MAX_PKT + 3];
    memset(tx, 0, (size_t)len + 3);
    tx[0] = C_READ_BUF; tx[1] = off;
    if (xfer(tx, rx, (size_t)len + 3) != ESP_OK) return;
    s_stats.rx_ok++;
    int16_t rssi = -(int16_t)ps[0] / 2;
    int8_t snr = (int8_t)ps[1];                                 /* ×4 */
    ESP_LOGI(TAG, "reçu %u o, RSSI %d dBm, SNR %.1f dB", len, rssi, snr / 4.0f);
    if (s_cb) s_cb(rx + 3, len, rssi, snr);
}

static bool transmit(const txreq_t *r)
{
    standby();
    set_packet(r->len);
    static uint8_t b[SX_MAX_PKT + 2];
    b[0] = C_WRITE_BUF; b[1] = 0;
    memcpy(b + 2, r->buf, r->len);
    if (xfer(b, NULL, (size_t)r->len + 2) != ESP_OK) return false;
    irq_clear(0xFFFF);
    uint8_t to[3] = { 0, 0, 0 };                                /* pas de délai matériel */
    cmd(C_SET_TX, to, 3);
    uint32_t air = sx_airtime_ms(r->len), lim = air + 500;
    int64_t t0 = esp_timer_get_time();
    ulTaskNotifyTake(pdTRUE, 0);
    bool ok = false;
    int64_t end = esp_timer_get_time() + lim * 1000LL;
    while (esp_timer_get_time() < end) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
        if (irq_get() & IRQ_TX_DONE) { ok = true; break; }
    }
    irq_clear(0xFFFF);
    gpio_intr_enable(BOARD_LORA_IRQ);
    if (ok) {
        s_stats.tx_ok++;
        ESP_LOGI(TAG, "émis %u o en %d ms (prévu %u)", r->len, (int)((esp_timer_get_time() - t0) / 1000), (unsigned)air);
    } else {
        s_stats.tx_fail++;
        ESP_LOGW(TAG, "émission sans TxDone (statut 0x%02x, erreurs 0x%04x)", status(), errors());
    }
    return ok;
}

static void lora_task(void *arg)
{
    EXT_RAM_BSS_ATTR static txreq_t tx_slot;                                     /* une seule tâche lora à la fois */
    txreq_t *r = &tx_slot;
    rx_continuous();
    int64_t beat = esp_timer_get_time();
    while (s_run) {
        /* pouls toutes les 30 s : mode de la puce (5 = réception), compteurs */
        if (esp_timer_get_time() - beat > 30000000) {
            beat = esp_timer_get_time();
            uint8_t st = status();
            ESP_LOGI(TAG, "mode %d, cmd %d, erreurs 0x%04x, TX %lu/%lu ratés, RX %lu/%lu abîmés, bruit %d dBm, DIO1 %d",
                     (st >> 4) & 7, (st >> 1) & 7, errors(), (unsigned long)s_stats.tx_ok,
                     (unsigned long)s_stats.tx_fail, (unsigned long)s_stats.rx_ok, (unsigned long)s_stats.rx_crc,
                     s_noise, gpio_get_level(BOARD_LORA_IRQ));
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        if (!s_run) break;
        uint16_t irq = irq_get();
        if (irq) s_stats.irq++;
        if (irq & (IRQ_PREAMBLE | IRQ_HEADER_VALID))
            s_rx_busy_until = esp_timer_get_time() + (int64_t)sx_airtime_ms(SX_MAX_PKT) * 1000;
        if (irq & (IRQ_RX_DONE | IRQ_CRC_ERR | IRQ_HEADER_ERR | IRQ_TIMEOUT)) {
            s_rx_busy_until = 0;
            handle_rx(irq);
            irq_clear(irq);
        } else if (irq) {
            irq_clear(irq);                                     /* préambule, en-tête : DIO1 retombe */
        }
        gpio_intr_enable(BOARD_LORA_IRQ);
        if (!irq && s_rx_busy_until == 0) {                     /* au calme : bruit de fond */
            uint8_t v = 0;
            if (cmd_read(C_GET_RSSI_INST, &v, 1) == ESP_OK) {
                int16_t n = -(int16_t)v / 2;
                s_noise = s_noise == INT16_MIN ? n : (int16_t)((s_noise * 7 + n) / 8);
            }
        }
        /* émission : seulement si aucune réception n'est en cours */
        if (r && uxQueueMessagesWaiting(s_txq) && esp_timer_get_time() >= s_rx_busy_until) {
            s_tx_busy = true;
            if (xQueueReceive(s_txq, r, 0) == pdTRUE) {
                transmit(r);
                rx_continuous();
            }
            s_tx_busy = false;
        } else if (uxQueueMessagesWaiting(s_txq)) {
            xTaskNotifyGive(xTaskGetCurrentTaskHandle());       /* on réessaie bientôt */
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    s_task = NULL;
    task_psram_exit();
}

/* ------------------------------------------------------------------ allumage */
/* La carte SD (ALDO1) et le NFC (DLDO1) partagent le bus SPI. Hors tension,
 * leurs diodes de protection tirent MISO, MOSI et SCK vers la masse : la
 * SX1262 répondait 0x00. On les alimente, CS tenus hauts (désélectionnés),
 * le temps que la radio tourne, comme LilyGoLib. */
static void bus_peers(bool on)
{
    const gpio_num_t cs[] = { BOARD_SD_CS, BOARD_NFC_CS };
    if (on) {
        axp_rail(AXP_RAIL_SD, true);
        axp_rail(AXP_RAIL_NFC, true);
        for (int i = 0; i < 2; i++) {
            gpio_set_level(cs[i], 1);
            gpio_config_t o = { .pin_bit_mask = 1ULL << cs[i], .mode = GPIO_MODE_OUTPUT,
                                .pull_up_en = GPIO_PULLUP_ENABLE };
            gpio_config(&o);
            gpio_set_level(cs[i], 1);
            gpio_sleep_sel_dis(cs[i]);
        }
    } else {
        for (int i = 0; i < 2; i++) {
            gpio_reset_pin(cs[i]);
            gpio_set_pull_mode(cs[i], GPIO_FLOATING);
            gpio_set_direction(cs[i], GPIO_MODE_DISABLE);
            gpio_sleep_sel_en(cs[i]);
        }
        axp_rail(AXP_RAIL_SD, false);
        axp_rail(AXP_RAIL_NFC, false);
    }
}

static void pins_idle(void)
{
    const gpio_num_t p[] = { BOARD_LORA_CS, BOARD_LORA_RST, BOARD_LORA_BUSY, BOARD_LORA_IRQ };
    for (int i = 0; i < 4; i++) {
        gpio_reset_pin(p[i]);
        gpio_set_pull_mode(p[i], GPIO_FLOATING);
        gpio_set_direction(p[i], GPIO_MODE_DISABLE);
        gpio_sleep_sel_en(p[i]);
    }
}

static esp_err_t chip_init(void)
{
    /* light sleep : CS et reset doivent rester hauts, BUSY et DIO1 lisibles
     * (IDF isole sinon toutes les broches pendant le sommeil) */
    const gpio_num_t keep[] = { BOARD_LORA_CS, BOARD_LORA_RST, BOARD_LORA_BUSY, BOARD_LORA_IRQ };
    for (int i = 0; i < 4; i++) gpio_sleep_sel_dis(keep[i]);
    gpio_config_t o = { .pin_bit_mask = 1ULL << BOARD_LORA_RST, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&o);
    gpio_config_t in = { .pin_bit_mask = 1ULL << BOARD_LORA_BUSY, .mode = GPIO_MODE_INPUT };
    gpio_config(&in);
    gpio_set_level(BOARD_LORA_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(2));
    gpio_set_level(BOARD_LORA_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    if (!wait_busy(100)) return ESP_ERR_TIMEOUT;

    uint8_t st = 0;
    for (int i = 0; i < 3; i++) {                               /* la puce peut rater le 1er ordre */
        standby();
        st = status();
        if (((st >> 4) & 7) == 2) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (((st >> 4) & 7) != 2) {                                 /* mode STDBY_RC attendu */
        ESP_LOGE(TAG, "pas de réponse (statut 0x%02x, BUSY %d, MISO %d, ALDO3 %d, SD %d, NFC %d)", st,
                 gpio_get_level(BOARD_LORA_BUSY), gpio_get_level(BOARD_SPI_MISO), axp_rail_is_on(AXP_RAIL_LORA),
                 axp_rail_is_on(AXP_RAIL_SD), axp_rail_is_on(AXP_RAIL_NFC));
        return ESP_ERR_NOT_FOUND;
    }
    uint8_t reg = 0x01;                                         /* DC-DC */
    cmd(C_SET_REGULATOR, &reg, 1);
    int mv = BOARD_LORA_TCXO_MV;
    uint8_t code = mv <= 1600 ? 0 : mv <= 1700 ? 1 : mv <= 1800 ? 2 : mv <= 2200 ? 3 : mv <= 2400 ? 4 :
                   mv <= 2700 ? 5 : mv <= 3000 ? 6 : 7;
    uint8_t tc[4] = { code, 0x00, 0x01, 0x40 };                 /* démarrage 5 ms (320 × 15,625 µs) */
    cmd(C_DIO3_TCXO, tc, 4);
    uint8_t cal = 0x7F;
    cmd(C_CALIBRATE, &cal, 1);
    wait_busy(50);
    uint8_t clr[2] = { 0, 0 };
    cmd(C_CLR_ERRORS, clr, 2);
#if BOARD_LORA_DIO2_RF_SW
    uint8_t one = 1;
    cmd(C_DIO2_RF_SW, &one, 1);
#endif
    /* errata 15.2 : protection du PA sur antenne désadaptée */
    reg_write8(R_TX_CLAMP, reg_read8(R_TX_CLAMP) | 0x1E);
    return ESP_OK;
}

esp_err_t sx_start(const sx_cfg_t *cfg, sx_rx_cb_t cb)
{
    if (s_run) return sx_set_config(cfg);
    s_cfg = *cfg;
    s_cb = cb;
    bus_peers(true);
    if (axp_rail(AXP_RAIL_LORA, true) != ESP_OK) ESP_LOGE(TAG, "rail ALDO3 : écriture I2C ratée");
    xl9555_output(XIO_LORA_RF_SW, s_ext_ant);                   /* bas : antenne intégrée, haut : externe
                                                                 * (essayé sur la montre, LilyGoLib dit l'inverse) */
    vTaskDelay(pdMS_TO_TICKS(20));                              /* montée du rail, POR de la puce */
    esp_err_t e = spi_shared_get();
    if (e != ESP_OK) goto off;
    spi_device_interface_config_t dc = {
        .mode = 0,
        .clock_speed_hz = BOARD_LORA_SPI_HZ,
        .spics_io_num = BOARD_LORA_CS,
        .queue_size = 1,
    };
    e = spi_bus_add_device(BOARD_SPI_HOST, &dc, &s_dev);
    if (e != ESP_OK) { spi_shared_put(); goto off; }
    if ((e = chip_init()) != ESP_OK || (e = apply()) != ESP_OK) {
        spi_bus_remove_device(s_dev); s_dev = NULL; spi_shared_put();
        goto off;
    }
    uint8_t st0 = status();                                     /* avant la tâche, seule à parler ensuite */
    uint16_t er0 = errors();
    if (!s_txq) s_txq = xQueueCreate(TXQ_LEN, sizeof(txreq_t));
    xQueueReset(s_txq);

    gpio_config_t irq = { .pin_bit_mask = 1ULL << BOARD_LORA_IRQ, .mode = GPIO_MODE_INPUT,
                          .pull_down_en = GPIO_PULLDOWN_ENABLE, .intr_type = GPIO_INTR_HIGH_LEVEL };
    gpio_config(&irq);
    gpio_install_isr_service(0);                                /* déjà fait ailleurs : sans effet */
    gpio_isr_handler_add(BOARD_LORA_IRQ, dio1_isr, NULL);
    gpio_wakeup_enable(BOARD_LORA_IRQ, GPIO_INTR_HIGH_LEVEL);

    s_run = true;
    s_last_err = ESP_OK;
    if (task_psram(lora_task, "lora", 4096, NULL, 6, &s_task, 0) != pdPASS) {
        s_run = false;
        sx_stop();
        ESP_LOGE(TAG, "tâche lora : mémoire insuffisante");
        s_last_err = ESP_ERR_NO_MEM;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "%.3f MHz, BW %.1f kHz, SF%d, CR 4/%d, %d dBm, synchro 0x%02x, préambule %u, antenne %s, "
             "statut 0x%02x, erreurs 0x%04x", s_cfg.freq_hz / 1e6, s_cfg.bw_hz / 1e3, s_cfg.sf, s_cfg.cr,
             s_cfg.power_dbm, s_cfg.sync_word, s_cfg.preamble, s_ext_ant ? "externe" : "intégrée", st0, er0);
    return ESP_OK;
off:
    xl9555_output(XIO_LORA_RF_SW, false);
    axp_rail(AXP_RAIL_LORA, false);
    bus_peers(false);
    pins_idle();
    ESP_LOGE(TAG, "démarrage impossible : %s", esp_err_to_name(e));
    s_last_err = e;
    return e;
}

void sx_stop(void)
{
    if (s_run) {
        s_run = false;
        if (s_task) xTaskNotifyGive(s_task);
        for (int i = 0; i < 100 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(10));
    }
    gpio_isr_handler_remove(BOARD_LORA_IRQ);
    gpio_wakeup_disable(BOARD_LORA_IRQ);
    if (s_dev) {
        uint8_t sl = 0x00;                                      /* démarrage à froid */
        cmd(C_SET_SLEEP, &sl, 1);
        spi_bus_remove_device(s_dev);
        s_dev = NULL;
        spi_shared_put();
    }
    xl9555_output(XIO_LORA_RF_SW, false);
    axp_rail(AXP_RAIL_LORA, false);
    bus_peers(false);
    pins_idle();
    s_noise = INT16_MIN;
}

bool sx_running(void) { return s_run; }

esp_err_t sx_set_config(const sx_cfg_t *cfg)
{
    if (!s_run) { s_cfg = *cfg; return ESP_OK; }
    /* la tâche possède la puce : on l'arrête le temps de reconfigurer */
    sx_rx_cb_t cb = s_cb;
    sx_stop();
    return sx_start(cfg, cb);
}

bool sx_send(const uint8_t *buf, size_t len)
{
    if (!s_run || !s_txq || !len || len > SX_MAX_PKT) return false;
    txreq_t r;
    r.len = (uint8_t)len;
    memcpy(r.buf, buf, len);
    if (xQueueSend(s_txq, &r, 0) != pdTRUE) return false;
    if (s_task) xTaskNotifyGive(s_task);
    return true;
}

int16_t sx_noise_floor(void) { return s_noise; }

bool sx_rx_active(void) { return s_run && esp_timer_get_time() < s_rx_busy_until; }

bool sx_tx_idle(void) { return !s_run || (!s_tx_busy && (!s_txq || !uxQueueMessagesWaiting(s_txq))); }

void sx_get_stats(sx_stats_t *out) { *out = s_stats; }

void sx_set_antenna(bool external)
{
    s_ext_ant = external;
    if (s_run) xl9555_output(XIO_LORA_RF_SW, external);
}

esp_err_t sx_last_error(void) { return s_last_err; }
