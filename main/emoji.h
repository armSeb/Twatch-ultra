/*
 * emoji.h — émojis en couleur (Twemoji) dans les textes LVGL.
 *
 * Les images sont dans la partition « emoji » (assets/emoji.bin, construite
 * par tools/gen_emoji.py), compressées ; elles sont décodées à la demande
 * dans un cache en PSRAM.
 *
 * Une séquence (ZWJ, drapeau, keycap, ton de peau…) ne se dessine pas
 * caractère par caractère : emoji_text() remplace chaque séquence connue par
 * un seul point de code privé (U+F0000 + rang dans le pack), que les polices
 * emoji_font_20 / emoji_font_28 dessinent ; elles sont les polices de secours
 * (fallback) de font_small, font_text et font_title.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "lvgl.h"

extern lv_font_t emoji_font_20;
extern lv_font_t emoji_font_28;

bool   emoji_init(void);                 /* lit la partition ; false si absente */
int    emoji_count(void);
/* texte UTF-8 → texte à afficher (séquences remplacées, sélecteurs, ZWJ
 * orphelins et tons de peau inconnus retirés). Renvoie la longueur écrite. */
size_t emoji_text(const char *in, char *out, size_t cap);
/* lv_label_set_text() après emoji_text() (tampon interne) */
void   emoji_label_set_text(lv_obj_t *label, const char *utf8);
