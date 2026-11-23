#pragma once
#include <stdbool.h>
#include <stdint.h>
/* true si les polices de texte savent dessiner ce point de code (font_cov.c) */
bool font_cov_has(uint32_t cp);
