/*
 * ui_geom.h — géométrie de l'interface (voir board.h pour la dalle).
 */
#pragma once
#include "board.h"

/* Géométrie. L'interface a été dessinée pour 240×240 : S() la met à
 * l'échelle de l'écran 410×502 (×1,5, comme les polices). Les écrans ont une
 * marge intérieure (UI_PAD_X, UI_PAD_Y) qui garde le contenu hors des coins
 * arrondis de la dalle ; UI_W × UI_H est la zone utile qui reste. */
#define S(px)     ((px) * 3 / 2)
#define UI_K      1.5f
#define UI_PAD_X  BOARD_SAFE_INSET_X
#define UI_PAD_Y  BOARD_SAFE_INSET_Y
#define UI_W      (BOARD_LCD_W - 2 * UI_PAD_X)
#define UI_H      (BOARD_LCD_H - 2 * UI_PAD_Y)
