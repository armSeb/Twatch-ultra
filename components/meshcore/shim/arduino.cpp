/* arduino.cpp — temps, hasard et journal pour le code MeshCore */
#include "Arduino.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

unsigned long millis(void) { return (unsigned long)(esp_timer_get_time() / 1000); }
unsigned long micros(void) { return (unsigned long)esp_timer_get_time(); }
void delay(unsigned long ms) { vTaskDelay(pdMS_TO_TICKS(ms) ? pdMS_TO_TICKS(ms) : 1); }
void yield(void) { taskYIELD(); }

/* esp_random() est un vrai générateur matériel dès que la radio (BLE) ou
 * le bootloader l'ont amorcé : pas de graine à gérer */
long random(long max)
{
    if (max <= 0) return 0;
    return (long)(esp_random() % (uint32_t)max);
}
long random(long min, long max)
{
    if (max <= min) return min;
    return min + random(max - min);
}
void randomSeed(unsigned long seed) { (void)seed; }

/* journal : une ligne ESP_LOG par ligne de texte */
static char s_line[160];
static size_t s_n;

size_t HardwareSerialLog::write(uint8_t c)
{
    if (c == '\n' || s_n >= sizeof s_line - 1) {
        s_line[s_n] = 0;
        if (s_n) ESP_LOGD("meshcore", "%s", s_line);
        s_n = 0;
        if (c == '\n') return 1;
    }
    s_line[s_n++] = (char)c;
    return 1;
}
size_t HardwareSerialLog::write(const uint8_t *buf, size_t n)
{
    for (size_t i = 0; i < n; i++) write(buf[i]);
    return n;
}
HardwareSerialLog Serial;
