/*
 * face_bake.h — décor fixe d'un cadran figé en une image.
 *
 * LVGL redessine l'écran par bandes : un décor riche (échelles de 61 traits,
 * cerclages à grand rayon, dégradé, harpe…) coûtait 30 ms et plus par image
 * pendant les glissements depuis ou vers le cadran. On le dessine une fois
 * dans une image RGB565 en PSRAM ; seules les parties vivantes (aiguilles,
 * guichets, pastilles) restent des objets.
 */
#pragma once
#include "lvgl.h"

/* Remplace base (un conteneur et ses enfants, tous fixes) par une image de
 * même place et même rang. Renvoie l'image, ou base si la mémoire manque
 * (le décor reste alors dessiné à chaque image, comme avant). */
lv_obj_t *face_bake(lv_obj_t *base);
