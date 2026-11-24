/*
 * netsvc.h — services réseau de la montre, lancés quand le Wi-Fi est allumé :
 *   - serveur web (réglages, mise à jour, carte SD) : réglage « Serveur web » ;
 *   - serveur TCP du protocole compagnon MeshCore (port 5000) : réglage des
 *     préférences MeshCore, MeshCore actif.
 * Tous deux sont coupés par défaut. Tâche app uniquement.
 */
#pragma once
#include <stdbool.h>

/* à appeler après tout changement du Wi-Fi (EV_WIFI), des réglages de ces
 * services ou de l'état de MeshCore */
void netsvc_update(void);
