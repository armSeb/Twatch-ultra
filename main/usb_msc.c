/* usb_msc.c — voir usb_msc.h */
#include "usb_msc.h"
#include <stdio.h>
#include <string.h>
#include "tusb.h"
#include "esp_private/usb_phy.h"
#include "hal/usb_serial_jtag_ll.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_pm.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "usb";

#define SECTOR      512
#define BOUNCE      CFG_TUD_MSC_EP_BUFSIZE
#define EP_OUT      0x01
#define EP_IN       0x81
#define USB_VID     0x303A      /* Espressif : PID de test TinyUSB */
#define USB_PID     0x4002

static usb_msc_media_t s_m;
static usb_phy_handle_t s_phy;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_done;
static volatile bool s_run, s_seen, s_ejected;
static uint8_t *s_bounce;               /* interne, DMA : la carte ne lit pas en PSRAM */
static esp_pm_lock_handle_t s_pm_apb, s_pm_nosleep;
static char s_serial[13];

/* ---------------- descripteurs ---------------- */

static const tusb_desc_device_t DEV = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0, .bDeviceSubClass = 0, .bDeviceProtocol = 0,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID, .idProduct = USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = 1, .iProduct = 2, .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

#define CFG_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)
static const uint8_t CFG[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, CFG_LEN, 0, 100),        /* 100 mA */
    TUD_MSC_DESCRIPTOR(0, 0, EP_OUT, EP_IN, 64),
};

const uint8_t *tud_descriptor_device_cb(void) { return (const uint8_t *)&DEV; }
const uint8_t *tud_descriptor_configuration_cb(uint8_t index) { (void)index; return CFG; }

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    static uint16_t d[24];
    (void)langid;
    const char *s;
    switch (index) {
    case 0: d[1] = 0x0409; d[0] = (uint16_t)((TUSB_DESC_STRING << 8) | 4); return d;
    case 1: s = "T-Watch Ultra"; break;
    case 2: s = "Carte SD de la montre"; break;
    case 3: s = s_serial; break;
    default: return NULL;
    }
    size_t n = strlen(s);
    if (n > 23) n = 23;
    for (size_t i = 0; i < n; i++) d[1 + i] = (uint8_t)s[i];   /* ASCII seulement */
    d[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * n + 2));
    return d;
}

/* ---------------- stockage de masse ---------------- */

void tud_mount_cb(void) { s_seen = true; }

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16], uint8_t product_rev[4])
{
    (void)lun;
    memcpy(vendor_id, "T-Watch ", 8);
    memcpy(product_id, "Carte SD        ", 16);
    memcpy(product_rev, "1.0 ", 4);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    (void)lun;
    if (s_ejected) {
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);   /* support absent */
        return false;
    }
    return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
    (void)lun;
    *block_count = s_ejected ? 0 : s_m.sectors;
    *block_size = SECTOR;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
{
    (void)lun; (void)power_condition;
    if (load_eject && !start && !s_ejected) {
        s_ejected = true;                   /* « Éjecter » côté PC : la montre reprend la carte */
        if (s_m.ejected) s_m.ejected();
    }
    return true;
}

bool tud_msc_is_writable_cb(uint8_t lun) { (void)lun; return true; }

static bool range_ok(uint32_t lba, uint32_t offset, uint32_t len)
{
    if (s_ejected || offset % SECTOR || len % SECTOR || len > BOUNCE) return false;
    uint64_t end = (uint64_t)lba + (offset + len) / SECTOR;
    return end <= s_m.sectors;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
    (void)lun;
    if (!range_ok(lba, offset, bufsize)) return TUD_MSC_RET_ERROR;
    if (s_m.read(s_bounce, lba + offset / SECTOR, bufsize / SECTOR) != ESP_OK) return TUD_MSC_RET_ERROR;
    memcpy(buffer, s_bounce, bufsize);
    return (int32_t)bufsize;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize)
{
    (void)lun;
    if (!range_ok(lba, offset, bufsize)) return TUD_MSC_RET_ERROR;
    memcpy(s_bounce, buffer, bufsize);
    if (s_m.write(s_bounce, lba + offset / SECTOR, bufsize / SECTOR) != ESP_OK) return TUD_MSC_RET_ERROR;
    return (int32_t)bufsize;
}

int32_t tud_msc_scsi_cb(uint8_t lun, const uint8_t scsi_cmd[16], void *buffer, uint16_t bufsize)
{
    (void)buffer; (void)bufsize;
    if (scsi_cmd[0] == 0x35) return 0;      /* SYNCHRONIZE CACHE : on n'en a pas */
    tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
    return -1;
}

/* ---------------- PHY et tâche ---------------- */

/* rend le PHY interne à la console USB Serial/JTAG */
static void phy_to_console(void)
{
    usb_serial_jtag_ll_phy_enable_external(false);
    usb_serial_jtag_ll_phy_enable_pad(true);
}

static void usb_task(void *arg)
{
    const tusb_rhport_init_t rh = { .role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_FULL };
    if (tusb_rhport_init(0, &rh)) {
        while (s_run) tud_task_ext(200, false);
        tusb_deinit(0);
    } else {
        ESP_LOGE(TAG, "TinyUSB : échec d'initialisation");
        s_run = false;
    }
    xSemaphoreGive(s_done);
    vTaskDelete(NULL);
}

bool usb_msc_active(void) { return s_task != NULL; }
bool usb_msc_host_seen(void) { return s_seen; }

esp_err_t usb_msc_start(const usb_msc_media_t *m)
{
    if (s_task) return ESP_ERR_INVALID_STATE;
    if (!m || !m->read || !m->write || !m->sectors) return ESP_ERR_INVALID_ARG;
    if (!s_done && !(s_done = xSemaphoreCreateBinary())) return ESP_ERR_NO_MEM;
    if (!s_pm_apb) {
        esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, "usb", &s_pm_apb);
        esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "usb", &s_pm_nosleep);
    }
    s_bounce = heap_caps_aligned_alloc(64, BOUNCE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_bounce) return ESP_ERR_NO_MEM;
    if (!s_serial[0]) {
        uint8_t mac[6] = { 0 };
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        for (int i = 0; i < 6; i++) snprintf(&s_serial[2 * i], 3, "%02X", mac[i]);
    }
    s_m = *m;
    s_seen = s_ejected = false;

    usb_phy_config_t pc = {
        .controller = USB_PHY_CTRL_OTG,
        .target = USB_PHY_TARGET_INT,
        .otg_mode = USB_OTG_MODE_DEVICE,
        .otg_speed = USB_PHY_SPEED_FULL,
    };
    esp_err_t e = usb_new_phy(&pc, &s_phy);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "PHY USB : %s", esp_err_to_name(e));
        phy_to_console();
        heap_caps_free(s_bounce);
        s_bounce = NULL;
        return e;
    }
    if (s_pm_apb) esp_pm_lock_acquire(s_pm_apb);
    if (s_pm_nosleep) esp_pm_lock_acquire(s_pm_nosleep);
    s_run = true;
    /* pile interne : la tâche pilote la carte (SPI) depuis sa pile */
    if (xTaskCreatePinnedToCore(usb_task, "usb", 4096, NULL, 4, &s_task, 0) != pdPASS) {
        s_task = NULL;
        s_run = false;
        usb_msc_stop();
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "carte SD partagée sur l'USB (%lu Mo)", (unsigned long)(m->sectors / 2048));
    return ESP_OK;
}

void usb_msc_stop(void)
{
    if (s_task) {
        s_run = false;
        xSemaphoreTake(s_done, portMAX_DELAY);
        s_task = NULL;
    }
    if (s_phy) {
        usb_del_phy(s_phy);
        s_phy = NULL;
        phy_to_console();
        if (s_pm_nosleep) esp_pm_lock_release(s_pm_nosleep);
        if (s_pm_apb) esp_pm_lock_release(s_pm_apb);
    }
    heap_caps_free(s_bounce);
    s_bounce = NULL;
    s_ejected = s_seen = false;
}
