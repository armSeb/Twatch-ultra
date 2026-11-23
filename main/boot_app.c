/* boot_app.c — voir boot_app.h */
#include "boot_app.h"
#include <stdint.h>
#include "esp_attr.h"
#include "esp_system.h"

#define APP_MAGIC 0xA9900000u
static RTC_NOINIT_ATTR uint32_t s_req;
static int s_app = -1;

void boot_app_init(void)
{
    if ((s_req & 0xFFFFFF00u) == APP_MAGIC && esp_reset_reason() == ESP_RST_SW) s_app = (int)(s_req & 0xFF);
    s_req = 0;
}

void boot_app_set(int idx) { s_req = idx >= 0 ? (APP_MAGIC | (uint32_t)(idx & 0xFF)) : 0; }
int  boot_app_get(void) { return s_app; }
