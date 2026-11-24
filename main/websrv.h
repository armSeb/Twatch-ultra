/*
 * websrv.h — serveur web de la montre sur le réseau Wi-Fi (mode station).
 *
 * http://<adresse de la montre>/ : état, réglages, mise à jour du firmware,
 * réseaux Wi-Fi / serveurs WebRX / webradios, et (option) fichiers de la
 * carte SD. Coupé par défaut ; lancé par netsvc.c quand le réglage « Serveur
 * web » est actif et le Wi-Fi allumé.
 *
 * Sécurité (pas de TLS sur la montre) :
 *  - mot de passe tiré au sort sur la montre (10 caractères, ~50 bits),
 *    affiché dans Réglages > Serveur web, jamais envoyé en clair : la page
 *    répond à un défi à usage unique (SHA-256 du défi et du mot de passe) ;
 *  - session : jeton aléatoire de 128 bits en cookie HttpOnly SameSite=Strict,
 *    expirée après 30 min sans activité (12 h au plus), 4 sessions au plus ;
 *  - essais ratés : blocage croissant (30 s, puis le double… 15 min) ;
 *  - toute requête qui modifie quelque chose : POST avec l'en-tête X-TW
 *    (impossible depuis un formulaire d'un autre site), Origin vérifié ;
 *  - en-tête Host vérifié (adresse IP de la montre ou nom local) contre le
 *    « DNS rebinding » ; en-têtes CSP, nosniff, frame DENY, no-store ;
 *  - tailles bornées (réglages 4 Ko, image ≤ emplacement OTA, fichier ≤
 *    place libre et 64 Mo) ; image vérifiée (en-tête, projet, SHA-256) ;
 *  - carte SD : chemins validés composant par composant (ni « .. », ni « . »,
 *    ni caractères interdits en FAT), /meshcore en lecture seule, fichiers
 *    toujours servis en téléchargement (application/octet-stream) ; refus
 *    quand la carte est éjectée ou prêtée au PC par l'USB.
 *
 * Tâche app : start / stop / mot de passe. Pile du serveur en PSRAM (les
 * écritures en flash passent par ota.c, sur une pile interne).
 */
#pragma once
#include <stdbool.h>
#include "esp_err.h"

esp_err_t   websrv_start(void);
void        websrv_stop(void);
bool        websrv_running(void);
const char *websrv_password(void);          /* mot de passe actuel (créé au besoin) */
void        websrv_new_password(void);      /* nouveau mot de passe : sessions fermées */
