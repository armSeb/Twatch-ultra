#pragma once
#include "lvgl.h"
/* Polices générées par tools/gen_fonts.sh (lv_font_conv) depuis Montserrat-Medium :
 * Latin-1 + Œœ Ÿ, tirets/guillemets/apostrophes typographiques, … et €,
 * plus les icônes FontAwesome utilisées par LV_SYMBOL_* (texte, titre). */
LV_FONT_DECLARE(font_small)  /* 17 px, Latin-1, petits textes */
LV_FONT_DECLARE(font_text)   /* 24 px, texte courant + icônes */
LV_FONT_DECLARE(font_title)  /* 30 px, titres + icônes */
LV_FONT_DECLARE(font_num)    /* 66 px, chiffres, espace, % + , - . / : */
LV_FONT_DECLARE(font_time)   /* 108 px, chiffres et ':' */
