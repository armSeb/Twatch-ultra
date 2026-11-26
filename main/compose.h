/*
 * compose.h — écrire un message sur la montre : grand champ de texte, clavier
 * complet (AZERTY / QWERTY) ou T9, émojis et réponses rapides.
 *
 *   ✕        → Alice           T9       en-tête : fermer, destinataire, mode
 *   ┌──────────────────────┐  ( ➤ )
 *   │ On se retrouve où ?| │   42/158    texte (UTF-8, émojis), envoyer
 *   └──────────────────────┘
 *   a z e r t y u i o p                  le doigt peut glisser : la touche
 *   q s d f g h j k l m                  retenue est celle sous le doigt au
 *   ⇧ w x c v b n ' ⌫                    lâcher, montrée en grand au-dessus ;
 *    123 🙂 💬 [ espace ] , .             appui long : accents (é è ê ë…)
 *
 * T9 : toucher une touche (2 = a b c 2 à â ç) ouvre ses caractères en grand
 * par-dessus le clavier, on touche celui qu'on veut ; appui long : le chiffre.
 *
 * Le module occupe tout l'écran par-dessus l'app (coins arrondis compris :
 * la rangée du bas est resserrée). Tâche app seulement.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

enum { KB_MODE_FULL = 0, KB_MODE_T9 = 1 };
enum { KB_LAYOUT_AZERTY = 0, KB_LAYOUT_QWERTY = 1 };

/* envoi : true si le message est parti (le brouillon est alors effacé et
 * l'écran se ferme), false pour rester (le module n'affiche rien : à
 * l'appelant de prévenir) */
typedef bool (*compose_send_cb_t)(const char *utf8, void *ud);

#define COMPOSE_NO_CAPS 0x01       /* pas de majuscule automatique (noms, identifiants) */

/* scr : écran de l'app ; draft_id : brouillon gardé tant que la montre
 * tourne, retrouvé si on revient écrire au même endroit ; max_bytes :
 * longueur maximale du texte en octets UTF-8 ; flags : COMPOSE_* */
void compose_open(lv_obj_t *scr, const char *title, uint32_t draft_id, int max_bytes,
                  unsigned flags, compose_send_cb_t cb, void *ud);
bool compose_is_open(void);
void compose_close(bool anim);
bool compose_on_back(void);          /* bouton retour : ferme d'abord le panneau ouvert */
void compose_forget(void);           /* l'écran de l'app est détruit */
