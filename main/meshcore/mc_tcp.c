/*
 * mc_tcp.c — protocole compagnon MeshCore sur TCP (port 5000), comme
 * SerialWifiInterface du firmware MeshCore : l'app MeshCore (connexion
 * TCP/IP) ou meshterm s'y branchent par le réseau Wi-Fi.
 *
 * Trames : app → montre « < » + longueur (2 octets, petit-boutiste) +
 * données ; montre → app « > » + longueur + données. Une seule appli à la
 * fois : une nouvelle connexion remplace l'ancienne (comme l'original).
 *
 * Le protocole n'a pas de mot de passe : le serveur est coupé par défaut et
 * ne tourne que si l'utilisateur l'active, Wi-Fi connecté (netsvc.c).
 * Appli connectée : pas d'économie d'énergie (comme par Bluetooth).
 */
#include "mc_port.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "task_psram.h"
#include "power.h"
#include "wifi_mgr.h"
#include "ble.h"

static const char *TAG = "mctcp";

#define IDLE_POLL_MS 2000                /* vérification de l'arrêt demandé */

static TaskHandle_t s_task;
static volatile bool s_run;
static SemaphoreHandle_t s_wmx;          /* s_csock : envoi (tâche mesh) / fermeture (tâche mctcp) */
static int s_csock = -1;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_peer[16];
static bool s_held;                      /* verrous d'énergie pris pour le client */

static void hold_power(bool on)
{
    if (on == s_held) return;
    s_held = on;
    power_hold_nosleep(on);
    power_hold_cpu_floor(on);
    wifi_perf(WIFI_PERF_MCTCP, on);
}

bool mc_tcp_client(char ip[16])
{
    portENTER_CRITICAL(&s_mux);
    bool c = s_csock >= 0;
    memcpy(ip, s_peer, 16);
    portEXIT_CRITICAL(&s_mux);
    if (!c) ip[0] = 0;
    return c;
}

bool mc_tcp_running(void) { return s_task != NULL; }

/* tâche mesh : une trame, envoyée en entier ou la connexion est coupée */
bool mc_tcp_send(const uint8_t *buf, size_t len)
{
    if (!s_wmx || !len || len > MC_FRAME_MAX) return false;
    uint8_t pkt[3 + MC_FRAME_MAX];
    pkt[0] = '>';
    pkt[1] = (uint8_t)(len & 0xFF);
    pkt[2] = (uint8_t)(len >> 8);
    memcpy(pkt + 3, buf, len);
    xSemaphoreTake(s_wmx, portMAX_DELAY);
    int fd = s_csock;
    bool ok = fd >= 0;
    size_t off = 0, tot = len + 3;
    while (ok && off < tot) {
        int n = send(fd, pkt + off, tot - off, 0);
        if (n <= 0) { ok = false; break; }
        off += (size_t)n;
    }
    if (!ok && fd >= 0) shutdown(fd, SHUT_RDWR);        /* la tâche mctcp ferme et prévient */
    xSemaphoreGive(s_wmx);
    return ok;
}

static void client_close(void)
{
    xSemaphoreTake(s_wmx, portMAX_DELAY);
    int fd = s_csock;
    portENTER_CRITICAL(&s_mux);
    s_csock = -1;
    s_peer[0] = 0;
    portEXIT_CRITICAL(&s_mux);
    xSemaphoreGive(s_wmx);
    if (fd >= 0) {
        close(fd);
        mc_tcp_link(false);
        hold_power(false);
        ESP_LOGI(TAG, "appli déconnectée");
    }
}

static void client_open(int fd, const struct sockaddr_in *from)
{
    client_close();                                     /* la nouvelle remplace l'ancienne */
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
    int idle = 60, intvl = 10, cnt = 3;                 /* appli disparue : ~1 min 30 */
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof intvl);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof cnt);
    struct timeval tv = { .tv_sec = 2 };                /* envoi bloqué : la tâche mesh ne s'arrête pas */
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    char ip[16];
    inet_ntoa_r(from->sin_addr, ip, sizeof ip);
    xSemaphoreTake(s_wmx, portMAX_DELAY);
    portENTER_CRITICAL(&s_mux);
    s_csock = fd;
    memcpy(s_peer, ip, sizeof s_peer);
    portEXIT_CRITICAL(&s_mux);
    xSemaphoreGive(s_wmx);
    hold_power(true);
    mc_tcp_link(true);
    ESP_LOGI(TAG, "appli connectée depuis %s", ip);
    ble_diag("MeshCore TCP : appli %s", ip);
}

/* lecture des trames « < » ; renvoie false si la connexion est finie */
typedef struct {
    uint8_t  hdr[3];
    int      nhdr;
    uint16_t len, got;
    bool     skip;
    uint8_t  buf[MC_FRAME_MAX];
} rxstate_t;

static bool client_read(int fd, rxstate_t *st)
{
    uint8_t tmp[256];
    int n = recv(fd, tmp, sizeof tmp, 0);
    if (n <= 0) return false;
    for (int i = 0; i < n;) {
        if (st->nhdr < 3) {
            st->hdr[st->nhdr++] = tmp[i++];
            if (st->nhdr < 3) continue;
            st->len = (uint16_t)(st->hdr[1] | (st->hdr[2] << 8));
            st->got = 0;
            /* trop longue ou pas de l'app vers la montre : ignorée en entier */
            st->skip = st->hdr[0] != '<' || st->len == 0 || st->len > MC_FRAME_MAX;
            if (st->len == 0) st->nhdr = 0;
            continue;
        }
        int k = n - i;
        if (k > st->len - st->got) k = st->len - st->got;
        if (!st->skip) memcpy(st->buf + st->got, tmp + i, (size_t)k);
        st->got += (uint16_t)k;
        i += k;
        if (st->got < st->len) continue;
        st->nhdr = 0;
        if (st->skip) continue;
        /* file de la tâche mesh pleine : on attend un peu (TCP retient l'app) */
        for (int t = 0; t < 100 && s_run; t++) {
            if (mc_tcp_rx(st->buf, st->len)) break;
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    return true;
}

static void tcp_task(void *arg)
{
    EXT_RAM_BSS_ATTR static rxstate_t rx;               /* tâche mctcp seule (PSRAM) */
    int ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls >= 0) {
        int one = 1;
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(MC_TCP_PORT),
                                 .sin_addr.s_addr = htonl(INADDR_ANY) };
        if (bind(ls, (struct sockaddr *)&a, sizeof a) < 0 || listen(ls, 1) < 0) {
            close(ls);
            ls = -1;
        }
    }
    if (ls < 0) {
        ESP_LOGE(TAG, "port %d indisponible", MC_TCP_PORT);
        s_run = false;
    } else {
        ESP_LOGI(TAG, "serveur compagnon sur le port %d", MC_TCP_PORT);
    }
    while (s_run) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(ls, &rf);
        int cs = s_csock, mx = ls;
        if (cs >= 0) { FD_SET(cs, &rf); if (cs > mx) mx = cs; }
        struct timeval tv = { .tv_sec = IDLE_POLL_MS / 1000 };
        int r = select(mx + 1, &rf, NULL, NULL, &tv);
        if (!s_run) break;
        if (r < 0) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }
        if (r == 0) continue;
        if (FD_ISSET(ls, &rf)) {
            struct sockaddr_in from;
            socklen_t fl = sizeof from;
            int fd = accept(ls, (struct sockaddr *)&from, &fl);
            if (fd >= 0) {
                client_open(fd, &from);
                memset(&rx, 0, sizeof rx);
                continue;
            }
        }
        if (cs >= 0 && FD_ISSET(cs, &rf) && !client_read(cs, &rx)) client_close();
    }
    client_close();
    if (ls >= 0) close(ls);
    ESP_LOGI(TAG, "serveur compagnon arrêté");
    s_task = NULL;
    task_psram_exit();
}

esp_err_t mc_tcp_start(void)
{
    if (s_task) return ESP_OK;
    if (!s_wmx) s_wmx = xSemaphoreCreateMutex();
    if (!s_wmx) return ESP_ERR_NO_MEM;
    s_run = true;
    if (task_psram(tcp_task, "mctcp", 3584, NULL, 5, &s_task, tskNO_AFFINITY) != pdPASS) {
        s_task = NULL;
        s_run = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void mc_tcp_stop(void)
{
    if (!s_task) return;
    s_run = false;
    /* débloque une lecture en cours ; la tâche ferme et prévient MeshCore */
    xSemaphoreTake(s_wmx, portMAX_DELAY);
    if (s_csock >= 0) shutdown(s_csock, SHUT_RDWR);
    xSemaphoreGive(s_wmx);
    for (int i = 0; i < 60 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(50));
    if (s_task) ESP_LOGW(TAG, "arrêt lent du serveur compagnon");
}
