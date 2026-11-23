/* CayenneLPP.h — les deux champs de télémétrie que MeshCore envoie */
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

class CayenneLPP {
public:
    explicit CayenneLPP(uint8_t size) : cap(size) { buf = (uint8_t *)calloc(size, 1); }
    ~CayenneLPP() { free(buf); }
    CayenneLPP(const CayenneLPP &) = delete;
    CayenneLPP &operator=(const CayenneLPP &) = delete;
    void reset() { len = 0; }
    uint8_t getSize() const { return len; }
    uint8_t *getBuffer() { return buf; }
    uint8_t addVoltage(uint8_t ch, float v) { return add16(ch, 116, (int32_t)lroundf(v * 100.0f)); }
    uint8_t addTemperature(uint8_t ch, float c) { return add16(ch, 103, (int32_t)lroundf(c * 10.0f)); }
private:
    uint8_t add16(uint8_t ch, uint8_t type, int32_t v)
    {
        if (!buf || len + 4 > cap) return 0;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        buf[len++] = ch;
        buf[len++] = type;
        buf[len++] = (uint8_t)((uint16_t)v >> 8);
        buf[len++] = (uint8_t)v;
        return len;
    }
    uint8_t *buf;
    uint8_t cap, len = 0;
};
