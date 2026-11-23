/*
 * FS.h — fs::FS / fs::File d'Arduino-ESP32 sur le VFS d'ESP-IDF (FAT).
 * Un FS a une racine (« /mc ») ; les chemins MeshCore (« /contacts3 ») y
 * sont rattachés. File partage son FILE* entre copies, comme sur Arduino.
 *
 * Écriture sûre : un fichier ouvert en "w" est écrit à côté (« nom~ ») et
 * ne remplace l'original qu'à la fermeture. Une coupure d'alimentation en
 * pleine sauvegarde des contacts laisse donc l'ancienne version intacte.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <memory>
#include "Stream.h"

namespace fs {

class File : public Stream {
public:
    File() {}
    File(FILE *f, const char *path, const char *tmp = nullptr);
    explicit operator bool() const { return (bool)h; }
    size_t write(uint8_t c) override { return write(&c, 1); }
    size_t write(const uint8_t *buf, size_t n) override;
    int available() override;
    int read() override;
    int peek() override;
    size_t readBytes(uint8_t *buf, size_t n) override { return read(buf, n); }
    size_t read(uint8_t *buf, size_t n);
    bool seek(uint32_t pos);
    size_t position() const;
    size_t size() const;
    void flush() override;
    void close() { h.reset(); }
    const char *name() const;
    bool isDirectory() const { return false; }
    File openNextFile() { return File(); }
private:
    struct H {
        FILE *f = nullptr;
        bool ok = true;          /* aucune erreur d'écriture */
        char path[72] = { 0 };
        char tmp[72] = { 0 };    /* écriture sûre : fichier provisoire */
        ~H();
    };
    std::shared_ptr<H> h;
};

class FS {
public:
    explicit FS(const char *root) : root(root) {}
    File open(const char *path, const char *mode = "r", bool create = false);
    bool exists(const char *path);
    bool remove(const char *path);
    bool rename(const char *from, const char *to);
    bool mkdir(const char *path);
    bool rmdir(const char *path);
    const char *base() const { return root; }
protected:
    void full(const char *path, char *out, size_t n) const;
    const char *root;
};

class SPIFFSFS : public FS {
public:
    explicit SPIFFSFS(const char *root) : FS(root) {}
    bool begin(bool formatOnFail = false);
    bool format();
    size_t totalBytes();
    size_t usedBytes();
};

}  // namespace fs

using fs::File;
