/* netsvc.c — voir netsvc.h */
#include "netsvc.h"
#include "settings.h"
#include "wifi_mgr.h"
#include "websrv.h"
#include "meshcore/mc_port.h"

void netsvc_update(void)
{
    const settings_t *st = settings_get();
    /* Wi-Fi allumé (connecté ou en reconnexion) : les serveurs écoutent sur
     * toutes les adresses et survivent à une courte coupure */
    wifi_status_t ws;
    wifi_get_status(&ws);
    bool net = (ws.holders & WIFI_HOLD_CONNECT) && ws.state != WIFI_OFF;

    bool tcp = net && st->mesh_tcp && mc_running();
    if (tcp && !mc_tcp_running()) mc_tcp_start();
    else if (!tcp && mc_tcp_running()) mc_tcp_stop();

    bool web = net && st->web_on;
    if (web && !websrv_running()) websrv_start();
    else if (!web && websrv_running()) websrv_stop();
}
