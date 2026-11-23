/* fs.cpp — voir FS.h */
#include "FS.h"
#include "SPIFFS.h"
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_vfs_fat.h"
#include "esp_log.h"

namespace fs {

File::H::~H()
{
    if (!f) return;
    bool good = ok && fflush(f) == 0 && fsync(fileno(f)) == 0;
    fclose(f);
    if (!tmp[0]) return;
    if (good) {
        ::remove(path);
        if (::rename(tmp, path) == 0) return;
    }
    ESP_LOGW("mc_fs", "écriture de %s abandonnée", path);
    ::remove(tmp);
}

File::File(FILE *f, const char *path, const char *tmp) : h(std::make_shared<H>())
{
    h->f = f;
    strlcpy(h->path, path, sizeof h->path);
    if (tmp) strlcpy(h->tmp, tmp, sizeof h->tmp);
}

size_t File::write(const uint8_t *buf, size_t n)
{
    if (!h || !n) return 0;
    size_t k = fwrite(buf, 1, n, h->f);
    if (k != n) h->ok = false;
    return k;
}
size_t File::read(uint8_t *buf, size_t n) { return h && n ? fread(buf, 1, n, h->f) : 0; }
int File::read()
{
    uint8_t c;
    return read(&c, 1) == 1 ? c : -1;
}
int File::peek()
{
    if (!h) return -1;
    int c = fgetc(h->f);
    if (c != EOF) ungetc(c, h->f);
    return c == EOF ? -1 : c;
}
int File::available()
{
    if (!h) return 0;
    long p = ftell(h->f), s = (long)size();
    return p >= 0 && s > p ? (int)(s - p) : 0;
}
bool File::seek(uint32_t pos) { return h && fseek(h->f, (long)pos, SEEK_SET) == 0; }
size_t File::position() const { return h ? (size_t)ftell(h->f) : 0; }
size_t File::size() const
{
    if (!h) return 0;
    struct stat st;
    fflush(h->f);
    return fstat(fileno(h->f), &st) == 0 ? (size_t)st.st_size : 0;
}
void File::flush() { if (h) fflush(h->f); }
const char *File::name() const
{
    if (!h) return "";
    const char *s = strrchr(h->path, '/');
    return s ? s + 1 : h->path;
}

void FS::full(const char *path, char *out, size_t n) const
{
    snprintf(out, n, "%s%s%s", root, path[0] == '/' ? "" : "/", path);
}

File FS::open(const char *path, const char *mode, bool create)
{
    char p[72], t[72];
    full(path, p, sizeof p);
    if (strlen(p) + 2 > sizeof p) return File();
    snprintf(t, sizeof t, "%s~", p);
    if (mode[0] == 'w') {
        /* SPIFFS (cible d'origine) a un espace de noms plat : « /identity/x.id »
         * s'y écrit sans dossier. Sur FAT, on crée les dossiers parents,
         * sinon l'identité du nœud n'était jamais sauvée (nouvelle clé à
         * chaque démarrage, l'appli relançait son assistant). */
        (void)create;
        for (char *q = strchr(p + strlen(root) + 1, '/'); q; q = strchr(q + 1, '/')) {
            *q = 0;
            ::mkdir(p, 0775);
            *q = '/';
        }
        FILE *f = fopen(t, "wb");
        if (!f) ESP_LOGW("mc_fs", "ouverture de %s impossible (errno %d)", p, errno);
        return f ? File(f, p, t) : File();
    }
    struct stat st;
    if (stat(p, &st) != 0 && stat(t, &st) == 0)    /* coupure entre remove et rename */
        ::rename(t, p);
    FILE *f = fopen(p, mode[0] == 'a' ? "ab" : strchr(mode, '+') ? "r+b" : "rb");
    if (!f && mode[0] == 'a' && errno == ENOENT) {      /* dossier parent absent */
        for (char *q = strchr(p + strlen(root) + 1, '/'); q; q = strchr(q + 1, '/')) {
            *q = 0;
            ::mkdir(p, 0775);
            *q = '/';
        }
        f = fopen(p, "ab");
    }
    return f ? File(f, p) : File();
}
bool FS::exists(const char *path)
{
    char p[72];
    full(path, p, sizeof p);
    struct stat st;
    return stat(p, &st) == 0;
}
bool FS::remove(const char *path)
{
    char p[72];
    full(path, p, sizeof p);
    return ::unlink(p) == 0;
}
bool FS::rename(const char *from, const char *to)
{
    char a[72], b[72];
    full(from, a, sizeof a);
    full(to, b, sizeof b);
    return ::rename(a, b) == 0;
}
bool FS::mkdir(const char *path)
{
    char p[72];
    full(path, p, sizeof p);
    return ::mkdir(p, 0775) == 0 || errno == EEXIST;
}
bool FS::rmdir(const char *path)
{
    char p[72];
    full(path, p, sizeof p);
    return ::rmdir(p) == 0;
}

/* ------------------------------------------------- partition FAT « storage » */
static wl_handle_t s_wl = WL_INVALID_HANDLE;

bool SPIFFSFS::begin(bool formatOnFail)
{
    if (s_wl != WL_INVALID_HANDLE) return true;
    esp_vfs_fat_mount_config_t mc = {};
    mc.format_if_mount_failed = formatOnFail;
    mc.max_files = 4;
    mc.allocation_unit_size = 4096;
    esp_err_t e = esp_vfs_fat_spiflash_mount_rw_wl(root, "storage", &mc, &s_wl);
    if (e != ESP_OK) ESP_LOGE("mc_fs", "montage de %s : %s", root, esp_err_to_name(e));
    return e == ESP_OK;
}
bool SPIFFSFS::format()
{
    return esp_vfs_fat_spiflash_format_rw_wl(root, "storage") == ESP_OK;
}
size_t SPIFFSFS::totalBytes()
{
    uint64_t tot = 0, fr = 0;
    return esp_vfs_fat_info(root, &tot, &fr) == ESP_OK ? (size_t)tot : 0;
}
size_t SPIFFSFS::usedBytes()
{
    uint64_t tot = 0, fr = 0;
    return esp_vfs_fat_info(root, &tot, &fr) == ESP_OK ? (size_t)(tot - fr) : 0;
}

}  // namespace fs

fs::SPIFFSFS SPIFFS("/mc");
