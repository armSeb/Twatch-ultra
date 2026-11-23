/* api/itoa.h — ltoa d'Arduino (la newlib d'ESP-IDF n'a que itoa/utoa) */
#pragma once
#include <stdio.h>
#include <stdlib.h>

static inline char *ltoa(long v, char *s, int radix)
{
    if (radix == 10) { sprintf(s, "%ld", v); return s; }
    return itoa((int)v, s, radix);
}
