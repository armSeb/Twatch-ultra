/*
 * face_bake.h — cadran analogique figé en image.
 *
 * Banc de mesure (0.2.33) : un cadran Pub coûtait 42 ms par image pendant un
 * glissement, dont ~19 ms pour son décor et ~12 à 19 ms pour ses six aiguilles
 * (traits épais à bouts ronds, coûteux en rendu logiciel). Tout ce qui ne bouge
 * pas à chaque seconde (décor, aiguilles des heures, minutes et batterie,
 * guichets) est construit hors écran et rendu dans une image RGB565 en PSRAM,
 * refaite seulement quand il change (une fois par minute). À l'affichage,
 * les carreaux unis de l'image sont remplis directement et seuls les carreaux
 * détaillés sont lus en PSRAM. Seule la trotteuse reste dessinée en direct.
 */
#pragma once
#include "lvgl.h"

/* holder : l'objet du cadran à l'écran, déjà dimensionné et placé.
 * Renvoie le conteneur hors écran, de même taille, où construire tout ce
 * qui est figé. L'image est rendue au prochain passage des minuteurs LVGL
 * (tâche app : la tâche main n'a pas la pile pour un rendu complet). */
lv_obj_t *face_bake_create(lv_obj_t *holder);

/* le contenu figé a changé (aiguilles, guichets…) : nouvelle image bientôt */
void face_bake_dirty(lv_obj_t *holder);

/* refait tout de suite les images en retard (réveil de l'écran, avant la
 * première image) ; tâche app seulement */
void face_bake_flush(void);

/* banc de mesure : l'objet qui affiche l'image, et la part (%) des
 * carreaux détaillés de la dernière image (-1 : aucune encore) */
bool face_bake_is_view(lv_obj_t *o);
int face_bake_detail_pct(void);
