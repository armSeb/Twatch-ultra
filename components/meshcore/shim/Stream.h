/* Stream.h — sous-ensemble de Print/Stream d'Arduino */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

class Print {
public:
    virtual ~Print() {}
    virtual void flush() {}
    virtual size_t write(uint8_t c) = 0;
    virtual size_t write(const uint8_t *buf, size_t n)
    {
        size_t k = 0;
        while (k < n && write(buf[k])) k++;
        return k;
    }
    size_t write(const char *s) { return write((const uint8_t *)s, strlen(s)); }
    size_t print(const char *s) { return write(s); }
    size_t print(char c) { return write((uint8_t)c); }
    size_t print(int v) { char b[16]; snprintf(b, sizeof b, "%d", v); return write(b); }
    size_t print(unsigned v) { char b[16]; snprintf(b, sizeof b, "%u", v); return write(b); }
    size_t print(long v) { char b[24]; snprintf(b, sizeof b, "%ld", v); return write(b); }
    size_t print(unsigned long v) { char b[24]; snprintf(b, sizeof b, "%lu", v); return write(b); }
    size_t print(double v, int d = 2) { char b[32]; snprintf(b, sizeof b, "%.*f", d, v); return write(b); }
    size_t println() { return write("\n"); }
    template <typename T> size_t println(T v) { size_t n = print(v); return n + println(); }
    size_t printf(const char *fmt, ...) __attribute__((format(printf, 2, 3)))
    {
        char b[192];
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(b, sizeof b, fmt, ap);
        va_end(ap);
        if (n < 0) return 0;
        return write((const uint8_t *)b, (size_t)n < sizeof b ? (size_t)n : sizeof b - 1);
    }
};

class Stream : public Print {
public:
    virtual int available() = 0;
    virtual int read() = 0;
    virtual int peek() = 0;
    virtual size_t readBytes(uint8_t *buf, size_t n)
    {
        size_t k = 0;
        int c;
        while (k < n && (c = read()) >= 0) buf[k++] = (uint8_t)c;
        return k;
    }
    size_t readBytes(char *buf, size_t n) { return readBytes((uint8_t *)buf, n); }
};
