#include "settings.h"
#include "nvs.h"
#include "esp_log.h"
#include "webrx_cfg.h"

static const char *TAG = "settings";
static settings_t s_set = {
    .brightness = 120,
    .screen_timeout_s = 8,
    .vibrate = true,
    .ble_enabled = true,
    .tz_offset_min = 60,      /* Europe/Paris hiver, corrigé par le CTS */
    .watchface = 0,
    .notif_wake = true,
    .tap_wake = true,
    .wrist_sens = 1,
    .glance = true,
    .glance_pct = 25,
    .glance_s = 4,
    .wifi_pwr = 3,
    .volume = 100,            /* 100 % = niveaux d'avant le réglage */
    .wifi_lowbatt = true,
    .mesh_tries = 3,
    .rec_hq = true,
};

void settings_load(void)
{
    nvs_handle_t h;
    if (nvs_open("tw", NVS_READONLY, &h) != ESP_OK) return;
    uint8_t u8; int16_t i16;
    if (nvs_get_u8(h, "bright", &u8) == ESP_OK) s_set.brightness = u8 < 10 ? 10 : u8;
    if (nvs_get_u8(h, "tmo", &u8) == ESP_OK && u8 >= 5 && u8 <= 60) s_set.screen_timeout_s = u8;
    if (nvs_get_u8(h, "vib", &u8) == ESP_OK) s_set.vibrate = u8;
    if (nvs_get_u8(h, "bt", &u8) == ESP_OK) s_set.ble_enabled = u8;
    if (nvs_get_i16(h, "tz", &i16) == ESP_OK) s_set.tz_offset_min = i16;
    if (nvs_get_u8(h, "face", &u8) == ESP_OK && u8 <= 2) s_set.watchface = u8;
    if (nvs_get_u8(h, "nwake", &u8) == ESP_OK) s_set.notif_wake = u8;
    if (nvs_get_u8(h, "dnd", &u8) == ESP_OK) s_set.dnd = u8;
    if (nvs_get_u8(h, "lang", &u8) == ESP_OK && u8 <= 1) s_set.lang = u8;
    if (nvs_get_u8(h, "tap", &u8) == ESP_OK) s_set.tap_wake = u8;
    if (nvs_get_u8(h, "steps", &u8) == ESP_OK) s_set.step_count = u8;
    if (nvs_get_u8(h, "wrist", &u8) == ESP_OK) s_set.wrist_wake = u8;
    if (nvs_get_u8(h, "wsens", &u8) == ESP_OK && u8 <= 2) s_set.wrist_sens = u8;
    if (nvs_get_u8(h, "glance", &u8) == ESP_OK) s_set.glance = u8;
    if (nvs_get_u8(h, "gpct", &u8) == ESP_OK && u8 >= 5 && u8 <= 100) s_set.glance_pct = u8;
    if (nvs_get_u8(h, "gdur", &u8) == ESP_OK && u8 >= 2 && u8 <= 15) s_set.glance_s = u8;
    if (nvs_get_u8(h, "mesh", &u8) == ESP_OK) s_set.meshcore = u8;
    if (nvs_get_u8(h, "lant", &u8) == ESP_OK) s_set.lora_ext_ant = u8;
    if (nvs_get_u8(h, "wpwr", &u8) == ESP_OK && u8 <= 3) s_set.wifi_pwr = u8;
    if (nvs_get_u8(h, "vol", &u8) == ESP_OK && u8 <= 100) s_set.volume = u8;
    if (nvs_get_u8(h, "mute", &u8) == ESP_OK) s_set.silent = u8;
    if (nvs_get_u8(h, "wifi", &u8) == ESP_OK) s_set.wifi_on = u8;
    if (nvs_get_u8(h, "wnet", &u8) == ESP_OK && u8 <= WRX_NETS) s_set.wifi_net = u8;
    if (nvs_get_u8(h, "wlow", &u8) == ESP_OK) s_set.wifi_lowbatt = u8;
    if (nvs_get_u8(h, "wnob", &u8) == ESP_OK) s_set.wifi_noble = u8;
    if (nvs_get_u8(h, "wap", &u8) == ESP_OK) s_set.wifi_ap = u8;
    if (nvs_get_u8(h, "mtcp", &u8) == ESP_OK) s_set.mesh_tcp = u8;
    if (nvs_get_u8(h, "web", &u8) == ESP_OK) s_set.web_on = u8;
    if (nvs_get_u8(h, "websd", &u8) == ESP_OK) s_set.web_sd = u8;
    if (nvs_get_u8(h, "mtry", &u8) == ESP_OK && u8 >= 1 && u8 <= 5) s_set.mesh_tries = u8;
    if (nvs_get_u8(h, "kbm", &u8) == ESP_OK && u8 <= 1) s_set.kb_mode = u8;
    if (nvs_get_u8(h, "kbl", &u8) == ESP_OK && u8 <= 1) s_set.kb_layout = u8;
    if (nvs_get_u8(h, "rhq", &u8) == ESP_OK) s_set.rec_hq = u8;
    nvs_close(h);
    ESP_LOGI(TAG, "lum=%u veille=%us vib=%d bt=%d tz=%+d min", s_set.brightness,
             s_set.screen_timeout_s, s_set.vibrate, s_set.ble_enabled, s_set.tz_offset_min);
}

void settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open("tw", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "bright", s_set.brightness);
    nvs_set_u8(h, "tmo", s_set.screen_timeout_s);
    nvs_set_u8(h, "vib", s_set.vibrate);
    nvs_set_u8(h, "bt", s_set.ble_enabled);
    nvs_set_i16(h, "tz", s_set.tz_offset_min);
    nvs_set_u8(h, "face", s_set.watchface);
    nvs_set_u8(h, "nwake", s_set.notif_wake);
    nvs_set_u8(h, "dnd", s_set.dnd);
    nvs_set_u8(h, "lang", s_set.lang);
    nvs_set_u8(h, "tap", s_set.tap_wake);
    nvs_set_u8(h, "steps", s_set.step_count);
    nvs_set_u8(h, "wrist", s_set.wrist_wake);
    nvs_set_u8(h, "wsens", s_set.wrist_sens);
    nvs_set_u8(h, "glance", s_set.glance);
    nvs_set_u8(h, "gpct", s_set.glance_pct);
    nvs_set_u8(h, "gdur", s_set.glance_s);
    nvs_set_u8(h, "mesh", s_set.meshcore);
    nvs_set_u8(h, "lant", s_set.lora_ext_ant);
    nvs_set_u8(h, "wpwr", s_set.wifi_pwr);
    nvs_set_u8(h, "vol", s_set.volume);
    nvs_set_u8(h, "mute", s_set.silent);
    nvs_set_u8(h, "wifi", s_set.wifi_on);
    nvs_set_u8(h, "wnet", s_set.wifi_net);
    nvs_set_u8(h, "wlow", s_set.wifi_lowbatt);
    nvs_set_u8(h, "wnob", s_set.wifi_noble);
    nvs_set_u8(h, "wap", s_set.wifi_ap);
    nvs_set_u8(h, "mtcp", s_set.mesh_tcp);
    nvs_set_u8(h, "web", s_set.web_on);
    nvs_set_u8(h, "websd", s_set.web_sd);
    nvs_set_u8(h, "mtry", s_set.mesh_tries);
    nvs_set_u8(h, "kbm", s_set.kb_mode);
    nvs_set_u8(h, "kbl", s_set.kb_layout);
    nvs_set_u8(h, "rhq", s_set.rec_hq);
    nvs_commit(h);
    nvs_close(h);
}

settings_t *settings_get(void) { return &s_set; }
