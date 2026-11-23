/*
 * Arduino.h — juste ce dont le code MeshCore a besoin, sur ESP-IDF.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <algorithm>
#include "Stream.h"

using std::min;
using std::max;

#ifndef constrain
#define constrain(x, lo, hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))
#endif
#ifndef PROGMEM
#define PROGMEM
#endif

unsigned long millis(void);
unsigned long micros(void);
void delay(unsigned long ms);
void yield(void);
long random(long max);
long random(long min, long max);
void randomSeed(unsigned long seed);

/* console série : journal ESP (pas d'entrée) */
class HardwareSerialLog : public Stream {
public:
    size_t write(uint8_t c) override;
    size_t write(const uint8_t *buf, size_t n) override;
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
};
extern HardwareSerialLog Serial;
