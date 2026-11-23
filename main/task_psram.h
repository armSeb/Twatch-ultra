/*
 * task_psram.h — tâches dont la pile vit en PSRAM.
 *
 * La RAM interne (≈ 340 Ko partagés entre code IRAM, données, Wi-Fi, BLE et
 * DMA) manque dès que la radio Internet tourne avec MeshCore. Toute tâche
 * qui n'a pas besoin de vitesse ni d'accès à la flash prend sa pile en
 * PSRAM ; seul le TCB (≈ 350 o) reste en interne.
 *
 * Règle : une tâche créée ainsi ne doit JAMAIS écrire ni lire la flash
 * par l'API (NVS, FAT, esp_partition_read/write, OTA) : le cache est coupé
 * pendant l'opération et sa pile devient inaccessible (assert dans
 * spi_flash). Les lectures via mmap (pack d'émojis, rodata) sont permises.
 * Elle se termine par task_psram_exit(), jamais vTaskDelete(NULL).
 */
#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"

static inline BaseType_t task_psram(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                                    UBaseType_t prio, TaskHandle_t *out, BaseType_t core)
{
    return xTaskCreatePinnedToCoreWithCaps(fn, name, stack, arg, prio, out, core,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static inline void task_psram_exit(void) { vTaskDeleteWithCaps(NULL); }
