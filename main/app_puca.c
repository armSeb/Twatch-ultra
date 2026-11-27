/*
 * app_puca.c — « Púca » : animal virtuel du folklore irlandais.
 *
 * Plein écran, coins compris :
 *
 *      (‹)      Cheval · 4 j       (♪)
 *     [🥔 ▬▬▬] [♥ ▬▬▬] [⚡ ▬▬▬]           satiété, humeur, énergie
 *     [☘ 3]                    ☀
 *          ciel, montagnes, collines, menhir, champignons
 *                 le púca qui se promène
 *       ( 🥔 )  ( ⚽ )  ( 💧 )  ( ☾ )      gros boutons ronds
 *
 * Le modèle (puca_model.c) vit sans tâche de fond : il est avancé à chaque
 * affichage. La vue :
 *  - le décor (ciel, relief, menhir, champignons, soleil ou lune, étoiles)
 *    est calculé une fois en pleine résolution dans une image RGB565 en
 *    PSRAM, puis rééclairé (heure du jour, lumière éteinte) seulement quand
 *    l'éclairage change ;
 *  - les dessins en pixel art (púca, objets, particules) sont agrandis ×6
 *    avec lissage (Scale3x puis Scale2x deux fois, puis moyenne 2×2 : bords
 *    arrondis et anticrénelés), mis en cache en PSRAM, et affichés par des
 *    objets image LVGL : LVGL ne redessine que les zones qui bougent ;
 *  - la nuit, les dessins sont assombris par la teinte d'image de LVGL,
 *    sauf ceux qui brillent (yeux, étoiles, Z…).
 * Animation à 25 images/s tant que l'écran est allumé.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "app.h"
#include "ui_kit.h"
#include "i18n.h"
#include "settings.h"
#include "wclock.h"
#include "motion.h"
#include "power.h"
#include "nvs.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_random.h"
#include "puca_model.h"
#include "puca_gfx.h"
#include "puca_sfx.h"

#define K       6                        /* pixels écran par pixel de dessin */
#define SW      BOARD_LCD_W
#define SH      BOARD_LCD_H
#define LW      ((float)SW / K)          /* largeur de la scène en pixels de dessin */
#define GY      366                      /* ligne sous les pattes (écran) */
#define FEET    (GY / K)                 /* même ligne en pixels de dessin */
#define TICK_MS 40
#define DT      (TICK_MS / 80.0f)        /* vitesses réglées pour un pas de 80 ms */
#define NPART   28

/* mise en page (pixels écran), en retrait des coins arrondis de la dalle
 * (rayon visible d'environ 110 px sur la montre) */
#define BTN_D   84                       /* diamètre des boutons ronds */
#define BTN_STEP 88                      /* écart entre leurs centres */
#define BTN_CY  420                      /* centre de la rangée de boutons */
#define TOP_CY  58                       /* centre de la barre du haut */
#define TOP_X   56                       /* bord des boutons retour et son */
#define GAUGE_W 104
#define GAUGE_X ((SW - 3 * GAUGE_W - 16) / 2)
#define GAUGE_Y 92
#define CLOV_Y  146
#define MSG_Y   198

/* ------------------------------------------------------------------ modèle */
static puca_t m;
static bool m_loaded;

static void load(void)
{
    if (m_loaded) return;
    m_loaded = true;
    nvs_handle_t h;
    size_t n = sizeof m;
    bool ok = false;
    if (nvs_open("puca", NVS_READONLY, &h) == ESP_OK) {
        memset(&m, 0, sizeof m);
        ok = nvs_get_blob(h, "st", &m, &n) == ESP_OK &&
             ((n == sizeof m && m.ver == 2) || (n == PUCA_V1_SIZE && m.ver == 1));
        if (ok) m.ver = 2;                     /* v1 : « bed » à 0 */
        nvs_close(h);
    }
    if (!ok) {
        memset(&m, 0, sizeof m);
        m.rng = esp_random() | 1;
        puca_new(&m);
    }
}

static void save(void)
{
    nvs_handle_t h;
    if (nvs_open("puca", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "st", &m, sizeof m);
    nvs_commit(h);
    nvs_close(h);
}

static int32_t tz(void) { return wclock_tz_offset_min(); }

static void model_update(void)
{
    if (!wclock_time_valid()) return;
    time_t now = time(NULL);
    uint32_t day = (uint32_t)(((int64_t)now + tz() * 60) / 86400);
    puca_update(&m, now, tz(), motion_steps_on() ? motion_steps_today() : 0, day);
}

static bool asleep(void) { return wclock_time_valid() && puca_asleep(&m, time(NULL), tz()); }
static bool alive(void) { return m.stage >= PS_BABY && m.stage <= PS_ADULT; }

/* ------------------------------------------------------------------ dessins lissés (cache PSRAM) */
typedef struct {
    lv_image_dsc_t dsc;
    uint32_t *px;
    const puca_spr_t *spr;
    uint8_t sc, flip, glow;
} img_t;
#define NIMG 96
EXT_RAM_BSS_ATTR static img_t s_img[NIMG];
static int s_nimg;

static void *ps_alloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }

/* Scale2x / Scale3x (AdvMAME) sur des indices de palette : arrondit les
 * escaliers en gardant les aplats nets */
static uint8_t *scale2x(const uint8_t *s, int w, int h)
{
    uint8_t *o = ps_alloc((size_t)w * h * 4);
    if (!o) return NULL;
    int ow = w * 2;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t E = s[y * w + x];
            uint8_t B = y > 0 ? s[(y - 1) * w + x] : E, H = y < h - 1 ? s[(y + 1) * w + x] : E;
            uint8_t D = x > 0 ? s[y * w + x - 1] : E, F = x < w - 1 ? s[y * w + x + 1] : E;
            uint8_t e0 = E, e1 = E, e2 = E, e3 = E;
            if (B != H && D != F) {
                if (D == B) e0 = D;
                if (B == F) e1 = F;
                if (D == H) e2 = D;
                if (H == F) e3 = F;
            }
            uint8_t *d = o + (y * 2) * ow + x * 2;
            d[0] = e0; d[1] = e1; d[ow] = e2; d[ow + 1] = e3;
        }
    return o;
}

static uint8_t *scale3x(const uint8_t *s, int w, int h)
{
    uint8_t *o = ps_alloc((size_t)w * h * 9);
    if (!o) return NULL;
    int ow = w * 3;
#define P(xx, yy) s[((yy) < 0 ? 0 : (yy) >= h ? h - 1 : (yy)) * w + ((xx) < 0 ? 0 : (xx) >= w ? w - 1 : (xx))]
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t A = P(x - 1, y - 1), B = P(x, y - 1), C = P(x + 1, y - 1);
            uint8_t D = P(x - 1, y), E = P(x, y), F = P(x + 1, y);
            uint8_t G = P(x - 1, y + 1), H = P(x, y + 1), I = P(x + 1, y + 1);
            uint8_t e[9] = { E, E, E, E, E, E, E, E, E };
            if (B != H && D != F) {
                e[0] = D == B ? D : E;
                e[1] = (D == B && E != C) || (B == F && E != A) ? B : E;
                e[2] = B == F ? F : E;
                e[3] = (D == B && E != G) || (D == H && E != A) ? D : E;
                e[5] = (B == F && E != I) || (H == F && E != C) ? F : E;
                e[6] = D == H ? D : E;
                e[7] = (D == H && E != I) || (H == F && E != G) ? H : E;
                e[8] = H == F ? F : E;
            }
            uint8_t *d = o + (y * 3) * ow + x * 3;
            for (int j = 0; j < 3; j++) memcpy(d + j * ow, e + j * 3, 3);
        }
#undef P
    return o;
}

/* dessin agrandi ×sc (2, 3, 4 ou 6), lissé et anticrénelé, éventuellement
 * retourné ; NULL si la mémoire manque */
static img_t *img(const puca_spr_t *s, int sc, bool flip)
{
    if (!s) return NULL;
    for (int i = 0; i < s_nimg; i++)
        if (s_img[i].spr == s && s_img[i].sc == sc && s_img[i].flip == flip) return &s_img[i];
    if (s_nimg >= NIMG) return NULL;
    int w = s->w, h = s->h;
    uint8_t *cur = ps_alloc((size_t)w * h);
    if (!cur) return NULL;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) cur[y * w + x] = s->px[y * w + (flip ? w - 1 - x : x)];
    /* chaîne vers 2×sc, puis moyenne 2×2 */
    static const uint8_t CH[7][4] = { {0}, {0}, { 2, 2 }, { 3, 2 }, { 2, 2, 2 }, {0}, { 3, 2, 2 } };
    if (sc < 2 || sc > 6 || !CH[sc][0]) { heap_caps_free(cur); return NULL; }
    for (int k = 0; k < 4 && CH[sc][k]; k++) {
        uint8_t *n = CH[sc][k] == 2 ? scale2x(cur, w, h) : scale3x(cur, w, h);
        heap_caps_free(cur);
        if (!n) return NULL;
        w *= CH[sc][k]; h *= CH[sc][k];
        cur = n;
    }
    int ow = w / 2, oh = h / 2;
    uint32_t *px = ps_alloc((size_t)ow * oh * 4);
    if (!px) { heap_caps_free(cur); return NULL; }
    int nglow = 0, nlit = 0;
    for (int y = 0; y < oh; y++)
        for (int x = 0; x < ow; x++) {
            uint32_t r = 0, g = 0, b = 0, a = 0;
            for (int j = 0; j < 2; j++)
                for (int i = 0; i < 2; i++) {
                    uint8_t c = cur[(y * 2 + j) * w + x * 2 + i];
                    if (!c) continue;
                    uint32_t v = PUCA_PAL[c];
                    r += v >> 16 & 255; g += v >> 8 & 255; b += v & 255; a++;
                    if (c != PC_OUTLINE) { nlit++; nglow += PUCA_GLOW[c]; }
                }
            px[y * ow + x] = a ? (a * 255 / 4) << 24 | (r / a) << 16 | (g / a) << 8 | (b / a) : 0;
        }
    heap_caps_free(cur);
    img_t *im = &s_img[s_nimg++];
    memset(im, 0, sizeof *im);
    im->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    im->dsc.header.cf = LV_COLOR_FORMAT_ARGB8888;
    im->dsc.header.w = ow;
    im->dsc.header.h = oh;
    im->dsc.header.stride = ow * 4;
    im->dsc.data = (const uint8_t *)px;
    im->dsc.data_size = ow * oh * 4;
    im->px = px;
    im->spr = s;
    im->sc = sc;
    im->flip = flip;
    im->glow = nlit && nglow * 2 >= nlit;
    return im;
}

static const lv_image_dsc_t *icon(const puca_spr_t *s, int sc)
{
    img_t *im = img(s, sc, false);
    return im ? &im->dsc : NULL;
}

/* ------------------------------------------------------------------ vue : état */
static lv_obj_t *v_root, *v_bg, *v_msg, *v_hint, *v_stage, *v_spk, *v_spk_lbl, *v_clov, *v_clov_lbl;
static lv_obj_t *v_bar[3], *v_btn[4], *v_light_img, *v_care_img, *v_btnrow, *v_gamerow, *v_feed, *v_gone, *v_noclock;
static lv_obj_t *v_shadow, *v_flash, *v_cloud[2];
static lv_timer_t *v_tmr;
static uint32_t v_ms;                    /* horloge de l'animation */
static uint32_t v_msg_until;

/* objets image des dessins : on ne touche LVGL que si quelque chose change */
enum {
    SL_POOP = 0, SL_STINK = SL_POOP + PUCA_POOPS, SL_EGG = SL_STINK + PUCA_POOPS,
    SL_BODY, SL_EYE1, SL_EYE2, SL_MOUTH, SL_HEAD, SL_RAIN, SL_ITEM,
    SL_DOT, SL_PART = SL_DOT + 5, SL_COUNT = SL_PART + NPART
};
typedef struct {
    lv_obj_t *o;
    const lv_image_dsc_t *src;
    int16_t x, y;
    uint8_t vis, used, white;
    lv_opa_t rop;
} spr_obj_t;
static spr_obj_t s_so[SL_COUNT];

/* éclairage courant des dessins (teinte d'image LVGL) */
static lv_color_t s_tint;
static lv_opa_t s_tint_opa;

typedef struct { const puca_spr_t *s; float x, y, vx, vy, g, life; } part_t;
static part_t s_part[NPART];

/* le púca à l'écran (pixels de dessin) */
static float p_x = 26, p_tx = 26, p_jump;
static int p_dir = 1;
static uint32_t p_pause, p_blink, p_next_blink;

/* animations */
enum { A_IDLE, A_HATCH, A_EGGTAP, A_EVOLVE, A_EAT, A_TREAT, A_HAPPY, A_REFUSE, A_CLEAN, A_HEAL, A_PET, A_GAME };
static int a_kind;
static uint32_t a_t0;
static int a_from_stage, a_from_form;        /* évolution : ancienne forme */
static float a_item_x, a_item_y;

/* jeu « gauche ou droite » */
#define G_ROUNDS 5
static int g_round, g_wins, g_phase, g_guess, g_look;
static uint8_t g_bits;                    /* manche i gagnée : bit i */
static uint32_t g_t;

/* ------------------------------------------------------------------ petits outils */
static uint32_t rnd(void) { return puca_rand(&m); }
static void sfx(puca_sfx_t s) { if (m.sound) puca_sfx(s); }

/* bulle de texte : largeur du texte, retour à la ligne au-delà de l'écran */
static void bubble_text(lv_obj_t *l, const char *txt)
{
    lv_point_t sz;
    int maxw = SW - 60, pad = 2 * lv_obj_get_style_pad_left(l, 0) + 2;
    lv_text_get_size(&sz, txt, lv_obj_get_style_text_font(l, 0), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_obj_set_width(l, sz.x + pad < maxw ? sz.x + pad : maxw);
    lv_label_set_text(l, txt);
}

static void say(const char *txt, uint32_t ms)
{
    if (!v_msg) return;
    bubble_text(v_msg, txt);
    lv_obj_remove_flag(v_msg, LV_OBJ_FLAG_HIDDEN);
    v_msg_until = v_ms + ms;
}

static void anim(int kind) { a_kind = kind; a_t0 = v_ms; }

static void part_add(const puca_spr_t *s, float x, float y, float vx, float vy, float g, int life)
{
    for (int i = 0; i < NPART; i++) {
        if (s_part[i].life <= 0) {
            s_part[i] = (part_t){ s, x, y, vx, vy, g, (float)life };
            return;
        }
    }
}

static float frand(float a, float b) { return a + (b - a) * (float)(rnd() % 1000) / 1000.0f; }

static void hide(lv_obj_t *o) { if (o) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN); }
static void show(lv_obj_t *o, bool on) { if (o) { if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN); else hide(o); } }

/* ------------------------------------------------------------------ objets image */
/* pose le dessin s (pixels de dessin x, y : coin haut-gauche) ; white =
 * silhouette blanche (éclosion, évolution) */
static void put(int slot, const puca_spr_t *s, float x, float y, bool flip, bool white)
{
    spr_obj_t *so = &s_so[slot];
    if (!so->o) return;
    img_t *im = img(s, K, flip);
    if (!im) return;
    so->used = 1;
    if (so->src != &im->dsc) { lv_image_set_src(so->o, &im->dsc); so->src = &im->dsc; }
    int16_t px = (int16_t)lroundf(x * K), py = (int16_t)lroundf(y * K);
    if (px != so->x || py != so->y) { lv_obj_set_pos(so->o, px, py); so->x = px; so->y = py; }
    lv_opa_t rop = white ? LV_OPA_COVER : im->glow ? LV_OPA_TRANSP : s_tint_opa;
    if (rop != so->rop || white != so->white) {
        lv_obj_set_style_image_recolor(so->o, white ? lv_color_white() : s_tint, 0);
        lv_obj_set_style_image_recolor_opa(so->o, rop, 0);
        so->rop = rop;
        so->white = white;
    }
    if (!so->vis) { lv_obj_remove_flag(so->o, LV_OBJ_FLAG_HIDDEN); so->vis = 1; }
}

/* ------------------------------------------------------------------ décor pleine résolution */
static uint16_t *s_base;                 /* relief de jour, RGB565 (PSRAM) */
static uint8_t *s_cov;                   /* 0 = ciel, 255 = relief (bords anticrénelés) */
static uint16_t *s_out;                  /* décor éclairé, affiché (PSRAM) */
static lv_image_dsc_t s_bg_dsc;
static uint32_t s_light_key = 0xFFFFFFFFu;

static inline uint32_t rgb(uint16_t c) { return (uint32_t)((c >> 11) * 255 / 31) << 16 | ((c >> 5 & 63) * 255 / 63) << 8 | (c & 31) * 255 / 31; }
static inline uint16_t c565(uint32_t c) { return (uint16_t)((c >> 8 & 0xF800) | (c >> 5 & 0x07E0) | (c >> 3 & 0x001F)); }

static inline uint32_t mixi(uint32_t a, uint32_t b, int t /* 0..256 */)
{
    int r = ((a >> 16 & 255) * (256 - t) + (b >> 16 & 255) * t) >> 8;
    int g = ((a >> 8 & 255) * (256 - t) + (b >> 8 & 255) * t) >> 8;
    int bl = ((a & 255) * (256 - t) + (b & 255) * t) >> 8;
    return (uint32_t)(r << 16 | g << 8 | bl);
}

/* mélange a → b (t de 0 à 1), en entiers : appelé pour chaque pixel du décor */
static inline uint32_t mix(uint32_t a, uint32_t b, float t)
{
    if (t <= 0) return a;
    if (t >= 1) return b;
    return mixi(a, b, (int)(t * 256));
}

static uint32_t hash(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}

/* profils du relief : montagnes lointaines, collines, pré */
static float prof_far(int x)  { return 236 + 20 * sinf(x * 0.011f + 0.6f) + 11 * sinf(x * 0.029f + 2.1f) + 4 * sinf(x * 0.083f); }
static float prof_mid(int x)  { return 286 + 13 * sinf(x * 0.017f + 3.0f) + 6 * sinf(x * 0.043f + 0.3f); }
static float prof_near(int x) { return 326 + 6 * sinf(x * 0.023f + 1.3f) + 3 * sinf(x * 0.061f); }

/* pose un dessin lissé dans le relief de jour */
static void base_sprite(const puca_spr_t *s, int sc, int x0, int y0, bool flip)
{
    img_t *im = img(s, sc, flip);
    if (!im) return;
    int w = im->dsc.header.w, h = im->dsc.header.h;
    for (int y = 0; y < h; y++) {
        int yy = y0 + y;
        if (yy < 0 || yy >= SH) continue;
        for (int x = 0; x < w; x++) {
            int xx = x0 + x;
            if (xx < 0 || xx >= SW) continue;
            uint32_t p = im->px[y * w + x];
            uint32_t a = p >> 24;
            if (!a) continue;
            size_t i = (size_t)yy * SW + xx;
            uint32_t under = rgb(s_base[i]);
            s_base[i] = c565(mixi(under, p & 0xFFFFFF, (int)(a * 256 / 255)));
            if (a > s_cov[i]) s_cov[i] = (uint8_t)a;
        }
    }
}

/* Le relief de jour (et le décor éclairé) est calculé à la première
 * ouverture puis gardé en PSRAM (~1 Mo, elle en a 8) : les ouvertures
 * suivantes sont immédiates. */
static bool make_base(void)
{
    if (s_base && s_cov && s_out) return true;
    s_base = ps_alloc((size_t)SW * SH * 2);
    s_cov = ps_alloc((size_t)SW * SH);
    s_out = ps_alloc((size_t)SW * SH * 2);
    if (!s_base || !s_cov || !s_out) return false;
    for (int x = 0; x < SW; x++) {
        float f = prof_far(x), mi = prof_mid(x), n = prof_near(x);
        for (int y = 0; y < SH; y++) {
            size_t i = (size_t)y * SW + x;
            uint32_t c;
            float cov;
            /* couche la plus proche qui couvre ce pixel, bord adouci */
            float dn = y + 0.5f - n, dm = y + 0.5f - mi, df = y + 0.5f - f;
            if (dn > -1) {
                float t = (y - n) / (float)(SH - n);
                c = y < GY - 6 ? mix(0x56a84b, 0x449a3e, (y - n) / (GY - 6 - n))
                               : mix(0x3f9339, 0x26622a, t * t * 1.4f);
                /* herbe : brins clairs et sombres, plus serrés au premier plan */
                uint32_t hsh = hash((uint32_t)(x * 7919 + y * 104729));
                int dens = y > GY ? 9 : 23;
                if ((hsh % dens) == 0) c = mix(c, 0x9ad66a, 0.45f);
                else if ((hsh % dens) == 1) c = mix(c, 0x17401c, 0.35f);
                if (y < n + 2.5f) c = mix(c, 0x8fd06a, 0.5f);           /* liseré de lumière */
                cov = dn < 0 ? dn + 1 : 1;
                if (cov < 1) {                                          /* bord : sur la colline derrière */
                    uint32_t back = mix(0x4c9a4f, 0x3f8a45, 0.2f);
                    c = mix(back, c, cov);
                    cov = 1;
                }
            } else if (dm > -1) {
                float t = (y - mi) / 50.0f;
                c = mix(0x5fac5e, 0x3f8a45, t);
                if (((x / 2 + y) & 7) == 0 && y > mi + 3) c = mix(c, 0x2f7a3a, 0.35f);
                if (y < mi + 2.5f) c = mix(c, 0x86c779, 0.55f);
                cov = dm < 0 ? dm + 1 : 1;
                if (cov < 1) { c = mix(0x7fb0b2, c, cov); cov = 1; }
            } else if (df > -1) {
                float t = (y - f) / 70.0f;
                c = mix(0x8fbfc4, 0x6fa69c, t);                         /* bleuté : lointain */
                if (y < f + 2) c = mix(c, 0xc8e4e6, 0.4f);
                cov = df < 0 ? df + 1 : 1;
            } else {
                c = 0;
                cov = 0;
            }
            s_base[i] = c565(c);
            s_cov[i] = (uint8_t)(cov * 255);
        }
    }
    /* sentier clair sous le púca */
    for (int y = GY - 10; y < GY + 16; y++)
        for (int x = 0; x < SW; x++) {
            float dy = (y - GY - 2) / 13.0f;
            float a = 0.22f * (1 - dy * dy);
            if (a <= 0) continue;
            size_t i = (size_t)y * SW + x;
            s_base[i] = c565(mix(rgb(s_base[i]), 0xa8c070, a));
        }
    /* fleurs du pré */
    for (int k = 0; k < 26; k++) {
        uint32_t hs = hash(0x51ed + k * 977);
        int x = 20 + (int)(hs % (SW - 40)), y = GY + 14 + (int)((hs >> 12) % 70);
        if (y > SH - 20) continue;
        uint32_t col = (hs >> 24) & 1 ? 0xfff6e6 : 0xffd84a;
        for (int j = -2; j <= 2; j++)
            for (int i = -2; i <= 2; i++) {
                int d = i * i + j * j;
                if (d > 4) continue;
                size_t p = (size_t)(y + j) * SW + x + i;
                s_base[p] = c565(d == 0 ? 0xffb020 : mix(rgb(s_base[p]), col, d == 4 ? 0.5f : 1));
            }
    }
    /* buissons sur les collines, menhir, cercle de champignons (le tertre des fées) */
    static const struct { int16_t x; int8_t dy; } BUSH[] = { { 40, 2 }, { 150, 0 }, { 300, 3 }, { 360, 1 } };
    for (size_t k = 0; k < sizeof BUSH / sizeof BUSH[0]; k++) {
        int x = BUSH[k].x;
        base_sprite(&puca_it_bush, 3, x, (int)prof_mid(x + 18) - 12 + BUSH[k].dy, k & 1);
    }
    base_sprite(&puca_it_stone, K, 14, GY - 62, false);
    base_sprite(&puca_it_shroom, 4, 318, GY - 30, false);
    base_sprite(&puca_it_shroom, K, 350, GY - 22, false);
    base_sprite(&puca_it_shroom, 4, 290, GY - 14, true);
    memset(&s_bg_dsc, 0, sizeof s_bg_dsc);
    s_bg_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_bg_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_bg_dsc.header.w = SW;
    s_bg_dsc.header.h = SH;
    s_bg_dsc.header.stride = SW * 2;
    s_bg_dsc.data = (const uint8_t *)s_out;
    s_bg_dsc.data_size = SW * SH * 2;
    return true;
}

/* ciel : couleurs (haut, bas) et luminosité de la scène selon l'heure,
 * calées sur les vrais lever et coucher du soleil du jour */
typedef struct { float h; uint32_t top, bot; float lum; } skykey_t;
#define SK_NIGHT 0x070b26, 0x26306a, 0.50f
#define SK_DAWN  0x4d47a8, 0xffb38a, 0.80f
#define SK_DAY   0x2f8fe0, 0xbfe6fa, 1.00f
#define SK_DUSK  0x3a2c78, 0xff8f5e, 0.80f
static skykey_t SKY[8];
static int s_sky_day = -1;
static float s_rise = 7, s_set = 19;

static void sky_keys(const struct tm *t)
{
    if (t->tm_yday == s_sky_day) return;
    s_sky_day = t->tm_yday;
    puca_sun(t->tm_yday, tz(), &s_rise, &s_set);
    const skykey_t k[8] = {
        { 0.0f, SK_NIGHT }, { s_rise - 1.0f, SK_NIGHT }, { s_rise, SK_DAWN }, { s_rise + 1.3f, SK_DAY },
        { s_set - 1.0f, SK_DAY }, { s_set, SK_DUSK }, { s_set + 1.1f, SK_NIGHT }, { 24.0f, SK_NIGHT },
    };
    memcpy(SKY, k, sizeof SKY);
}

static float day_hour(void)
{
    struct tm t;
    if (!wclock_time_valid()) { memset(&t, 0, sizeof t); t.tm_yday = 172; sky_keys(&t); return 12; }
    wclock_local_tm(&t);
    sky_keys(&t);
    return (float)t.tm_hour + t.tm_min / 60.0f;
}

typedef struct {
    uint32_t top, bot, tint;
    float lum, hr;
    bool night;
} light_t;

static light_t s_li;

static void light_now(light_t *L)
{
    float hr = day_hour();
    int k = 0;
    while (k < (int)(sizeof SKY / sizeof SKY[0]) - 2 && hr >= SKY[k + 1].h) k++;
    float t = (hr - SKY[k].h) / (SKY[k + 1].h - SKY[k].h);
    L->hr = hr;
    L->top = mix(SKY[k].top, SKY[k + 1].top, t);
    L->bot = mix(SKY[k].bot, SKY[k + 1].bot, t);
    L->lum = SKY[k].lum + (SKY[k + 1].lum - SKY[k].lum) * t;
    L->night = L->lum < 0.6f;
    L->tint = 0x0a0c22;                       /* teinte vers laquelle on assombrit */
    bool sleeping = alive() && asleep();
    if (sleeping && m.light_off) {            /* nuit noire : seuls brillent les yeux, la lune, les étoiles */
        L->lum = 0.30f; L->top = mix(L->top, 0x02030a, 0.6f); L->bot = mix(L->bot, 0x05060f, 0.7f);
    } else if (sleeping) {                    /* lampe allumée : lumière chaude */
        L->lum = 0.80f; L->tint = 0x3a2208;
    }
}

/* astre : position selon l'heure (lever à gauche, coucher à droite) */
static void astre_pos(const light_t *L, int *cx, int *cy)
{
    float u;
    if (!L->night) u = (L->hr - s_rise) / (s_set - s_rise);
    else {
        float n = s_rise + 24 - s_set, h = L->hr < s_rise ? L->hr + 24 : L->hr;
        u = (h - s_set) / n;
    }
    if (u < 0) u = 0;
    if (u > 1) u = 1;
    *cx = 60 + (int)(290 * u);
    *cy = 238 - (int)(92 * sinf((float)M_PI * u));
}

static const uint8_t BAYER[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };

/* recalcule le décor éclairé si l'éclairage a changé (ou force) */
static void relight(bool force)
{
    if (!s_out || !v_bg) return;
    light_t L;
    light_now(&L);
    int ax, ay;
    astre_pos(&L, &ax, &ay);
    uint32_t key = (L.top ^ L.bot * 31 ^ L.tint * 7) + (uint32_t)(L.lum * 64) * 131 + (uint32_t)(ax / 3) * 7919 + (uint32_t)(ay / 3) * 104729 + L.night;
    if (!force && key == s_light_key) return;
    s_light_key = key;
    s_li = L;

    /* teinte du relief : tables par canal */
    float f = L.lum < 0.99f ? (1.0f - L.lum) * 0.9f : 0;
    int tf = (int)(f * 256);
    uint8_t lr[32], lg[64], lb[32];
    int trr = L.tint >> 16 & 255, tgg = L.tint >> 8 & 255, tbb = L.tint & 255;
    for (int i = 0; i < 32; i++) {
        lr[i] = (uint8_t)(((i * 255 / 31) * (256 - tf) + trr * tf) >> 8 >> 3);
        lb[i] = (uint8_t)(((i * 255 / 31) * (256 - tf) + tbb * tf) >> 8 >> 3);
    }
    for (int i = 0; i < 64; i++) lg[i] = (uint8_t)(((i * 255 / 63) * (256 - tf) + tgg * tf) >> 8 >> 2);

    int horizon = 300;
    uint32_t glowc = L.night ? 0xc8d2ff : 0xfff2c8;
    float glowa = L.night ? 0.22f : 0.5f;
    int gr = L.night ? 70 : 110;
    for (int y = 0; y < SH; y++) {
        float ty = y >= horizon ? 1.0f : (float)y / horizon;
        uint32_t sky = mix(L.top, L.bot, ty * ty * 0.3f + ty * 0.7f);
        int sr = sky >> 16 & 255, sg = sky >> 8 & 255, sb = sky & 255;
        uint16_t *o = s_out + (size_t)y * SW;
        const uint16_t *b = s_base + (size_t)y * SW;
        const uint8_t *cv = s_cov + (size_t)y * SW;
        int dy = y - ay;
        for (int x = 0; x < SW; x++) {
            uint8_t a = cv[x];
            uint16_t land = 0;
            if (a) { uint16_t c = b[x]; land = (uint16_t)(lr[c >> 11] << 11 | lg[c >> 5 & 63] << 5 | lb[c & 31]); }
            if (a == 255) { o[x] = land; continue; }
            /* ciel tramé (pas de bandes en RGB565), halo de l'astre */
            int r = sr, g = sg, bb = sb;
            int dx = x - ax;
            int d2 = dx * dx + dy * dy;
            if (d2 < gr * gr) {
                float d = sqrtf((float)d2) / gr;
                float h = (1 - d) * (1 - d) * glowa;
                r += (int)(((glowc >> 16 & 255) - r) * h);
                g += (int)(((glowc >> 8 & 255) - g) * h);
                bb += (int)(((glowc & 255) - bb) * h);
            }
            int th = BAYER[y & 3][x & 3];
            r = (r + (th >> 1)) >> 3; g = (g + (th >> 2)) >> 2; bb = (bb + (th >> 1)) >> 3;
            if (r > 31) r = 31;
            if (g > 63) g = 63;
            if (bb > 31) bb = 31;
            uint16_t s = (uint16_t)(r << 11 | g << 5 | bb);
            if (!a) { o[x] = s; continue; }
            uint32_t m2 = mixi(rgb(s), rgb(land), a);
            o[x] = c565(m2);
        }
    }
    /* étoiles, puis soleil ou lune (derrière le relief) */
    if (L.night) {
        for (int k = 0; k < 46; k++) {
            uint32_t hs = hash(0xa11ce + k * 2654435761u);
            int x = 10 + (int)(hs % (SW - 20)), y = 8 + (int)((hs >> 10) % 240);
            size_t i = (size_t)y * SW + x;
            if (s_cov[i] || y + 1 >= SH || x + 1 >= SW) continue;
            bool big = (hs >> 28) == 0;
            uint32_t sc = (hs >> 24) & 1 ? 0xffffff : 0xfff0c0;
            float a = 0.5f + ((hs >> 20) & 7) / 14.0f;
            s_out[i] = c565(mix(rgb(s_out[i]), sc, a));
            if (big) {
                s_out[i + 1] = c565(mix(rgb(s_out[i + 1]), sc, a * 0.5f));
                s_out[i - 1] = c565(mix(rgb(s_out[i - 1]), sc, a * 0.5f));
                s_out[i + SW] = c565(mix(rgb(s_out[i + SW]), sc, a * 0.5f));
                s_out[i - SW] = c565(mix(rgb(s_out[i - SW]), sc, a * 0.5f));
            }
        }
    }
    int rr = L.night ? 16 : 21;
    uint32_t disc = L.night ? 0xfff4d6 : 0xfff6c0;
    for (int y = ay - rr - 2; y <= ay + rr + 2; y++) {
        if (y < 0 || y >= SH) continue;
        for (int x = ax - rr - 2; x <= ax + rr + 2; x++) {
            if (x < 0 || x >= SW) continue;
            size_t i = (size_t)y * SW + x;
            float d = sqrtf((float)((x - ax) * (x - ax) + (y - ay) * (y - ay)));
            float a = rr + 0.5f - d;                  /* bord anticrénelé */
            if (L.night) {                            /* croissant : on retire un disque décalé */
                float d2 = sqrtf((float)((x - ax - 8) * (x - ax - 8) + (y - ay + 5) * (y - ay + 5)));
                float b = d2 - (rr - 2) + 0.5f;
                if (b < a) a = b;
            }
            if (a <= 0) continue;
            if (a > 1) a = 1;
            a *= 1.0f - s_cov[i] / 255.0f;
            s_out[i] = c565(mix(rgb(s_out[i]), disc, a));
        }
    }
    lv_obj_invalidate(v_bg);

    /* dessins : teinte d'image */
    s_tint = lv_color_hex(L.tint);
    s_tint_opa = L.lum < 0.99f ? (lv_opa_t)(f * 255) : LV_OPA_TRANSP;
    for (int i = 0; i < 2; i++) {
        if (!v_cloud[i]) continue;
        lv_obj_set_style_image_recolor(v_cloud[i], s_tint, 0);
        lv_obj_set_style_image_recolor_opa(v_cloud[i], s_tint_opa, 0);
        show(v_cloud[i], !L.night);
    }
}

/* nuages doux (ARGB, bords flous), calculés une fois */
static lv_image_dsc_t s_cloud_dsc[2];
static uint32_t *s_cloud_px[2];
static float s_cloud_x[2] = { 30, 230 };

static void make_cloud(int k, int w, int h, uint32_t seed)
{
    if (s_cloud_px[k]) return;                 /* gardés d'une ouverture à l'autre */
    uint32_t *px = ps_alloc((size_t)w * h * 4);
    if (!px) return;
    typedef struct { float x, y, r; } blob_t;
    blob_t c[6];
    int n = 0;
    float r0 = h * 0.34f;
    c[n++] = (blob_t){ w * 0.5f, h * 0.48f, r0 * 1.25f };
    c[n++] = (blob_t){ w * 0.3f, h * 0.58f, r0 };
    c[n++] = (blob_t){ w * 0.7f, h * 0.6f, r0 * 0.9f };
    c[n++] = (blob_t){ w * 0.16f, h * 0.7f, r0 * 0.6f };
    c[n++] = (blob_t){ w * 0.84f, h * 0.72f, r0 * 0.55f };
    c[n++] = (blob_t){ w * (0.38f + (seed & 7) * 0.02f), h * 0.36f, r0 * 0.9f };
    float base = h * 0.84f;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float best = -99;
            for (int i = 0; i < n; i++) {
                float d = c[i].r - sqrtf((x - c[i].x) * (x - c[i].x) + (y - c[i].y) * (y - c[i].y));
                if (d > best) best = d;
            }
            float flat = base - y;                       /* dessous plat */
            if (flat < best) best = flat;
            float a = best / 3.0f;                       /* bord flou sur 3 px */
            if (a <= 0) { px[y * w + x] = 0; continue; }
            if (a > 1) a = 1;
            uint32_t col = mix(0xffffff, 0xc4d0e4, (float)y / h);
            px[y * w + x] = (uint32_t)(a * 235) << 24 | col;
        }
    lv_image_dsc_t *d = &s_cloud_dsc[k];
    memset(d, 0, sizeof *d);
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    d->header.cf = LV_COLOR_FORMAT_ARGB8888;
    d->header.w = w;
    d->header.h = h;
    d->header.stride = w * 4;
    d->data = (const uint8_t *)px;
    d->data_size = w * h * 4;
    s_cloud_px[k] = px;
}

/* ------------------------------------------------------------------ dessin d'une image */
static const puca_body_t *body_of(int stage, int form)
{
    if (stage == PS_BABY) return &puca_body_baby;
    if (stage == PS_TEEN) return &puca_body_teen;
    if (form == PF_HARE) return &puca_body_hare;
    if (form == PF_GOAT) return &puca_body_goat;
    return &puca_body_horse;
}

static int body_w(void) { return body_of(m.stage, m.form)->frames[0]->w; }

/* décalage vertical de la tête d'une image de respiration par rapport à la
 * première : premier pixel plein de la colonne de l'œil (les ancres des
 * yeux et de la bouche sont données pour la première image) */
static int head_dy(const puca_body_t *b, int frame)
{
    const puca_spr_t *f0 = b->frames[0], *f = b->frames[frame % b->nframes];
    if (f == f0) return 0;
    int x = b->ex1 + 1, t0 = 0, t1 = 0;
    while (t0 < f0->h && !f0->px[t0 * f0->w + x]) t0++;
    while (t1 < f->h && !f->px[t1 * f->w + x]) t1++;
    return t1 - t0;
}

static void put_puca(const puca_body_t *b, int frame, const puca_spr_t *eye, const puca_spr_t *mouth,
                     float x, float y, bool flip, bool white)
{
    const puca_spr_t *f = b->frames[frame % b->nframes];
    put(SL_BODY, f, x, y, flip, white);
    if (white) return;
    y += head_dy(b, frame);                    /* yeux et bouche suivent la tête */
    int w = f->w;
#define FX(px, gw) (flip ? w - (px) - (gw) : (px))
    if (eye) {
        put(SL_EYE1, eye, x + FX(b->ex1, 3), y + b->ey1, flip, false);
        if (b->ex2 >= 0) put(SL_EYE2, eye, x + FX(b->ex2, 3), y + b->ey2, flip, false);
    }
    if (mouth && mouth != &puca_mouth_none) put(SL_MOUTH, mouth, x + FX(b->mx, 3), y + b->my, flip, false);
#undef FX
}

static const float POOP_X[PUCA_POOPS] = { 44, 36, 52, 28 };
static int16_t s_sh_x = -1000, s_sh_w;

static void render(void)
{
    if (!v_root) return;
    for (int i = 0; i < SL_COUNT; i++) s_so[i].used = 0;
    bool sleeping = alive() && asleep();
    uint32_t at = v_ms - a_t0;

    /* nuages qui passent */
    if (!s_li.night)
        for (int i = 0; i < 2; i++) {
            if (!v_cloud[i]) continue;
            int w = s_cloud_dsc[i].header.w;
            s_cloud_x[i] += (i ? 0.16f : 0.22f) * DT;
            if (s_cloud_x[i] > SW + 10) s_cloud_x[i] = -(float)w - 10;
            int x = (int)s_cloud_x[i];
            if (x != lv_obj_get_x(v_cloud[i])) lv_obj_set_x(v_cloud[i], x);
        }

    /* crottes, avec mouches d'odeur */
    for (int i = 0; i < m.poops && i < PUCA_POOPS; i++) {
        if (a_kind == A_CLEAN && a_item_x + 10 > POOP_X[i]) continue;
        put(SL_POOP + i, &puca_it_poop, POOP_X[i], FEET - 7, i & 1, false);
        if ((v_ms / 300 + i) % 2)
            put(SL_STINK + i, &puca_it_stink, POOP_X[i] + 3, FEET - 12 - (float)((v_ms / 150 + i * 3) % 3), false, false);
    }

    /* l'œuf ou le púca */
    float sh_x = -1, sh_w = 0;
    if (m.stage == PS_EGG || a_kind == A_HATCH) {
        bool hatched = a_kind == A_HATCH && at > 900;
        if (!hatched) {
            const puca_spr_t *e = m.egg_taps >= 4 ? &puca_egg_2 : m.egg_taps >= 2 ? &puca_egg_1 : &puca_egg_0;
            float dx = 0;
            if (a_kind == A_EGGTAP && at < 300) dx = (at / 60) % 2 ? 0.7f : -0.7f;
            else if (a_kind == A_HATCH) dx = (at / 40) % 2 ? 0.7f : -0.7f;
            else if ((v_ms / 1800) % 3 == 0 && (v_ms / 120) % 4 < 2) dx = (v_ms / 120) % 2 ? 0.5f : -0.5f;
            float ex = (LW - e->w) / 2;
            put(SL_EGG, e, ex + dx, FEET - e->h, false, a_kind == A_HATCH && at > 800);
            sh_x = ex; sh_w = e->w;
        }
    }
    if (alive()) {
        const puca_body_t *b = body_of(m.stage, m.form);
        int frame = sleeping ? (v_ms / 1100) % 2 : (v_ms / 450) % 2;
        const puca_spr_t *eye = &puca_eye_open, *mouth = &puca_mouth_smile;
        if (m.stage == PS_ADULT && m.form != PF_GOAT) mouth = &puca_mouth_none;
        if (sleeping) { eye = &puca_eye_closed; mouth = &puca_mouth_none; }
        else if (m.sick) { eye = &puca_eye_sick; mouth = &puca_mouth_flat; }
        else if (m.mood < 250 || m.hunger < 150) { eye = &puca_eye_sad; mouth = &puca_mouth_frown; }
        else if (v_ms < p_blink) eye = &puca_eye_blink;

        switch (a_kind) {
        case A_EAT: case A_TREAT:
            if (at > 500 && at < 1700) { eye = &puca_eye_happy; mouth = (at / 180) % 2 ? &puca_mouth_open : &puca_mouth_chew; }
            else if (at >= 1700) { eye = &puca_eye_happy; mouth = &puca_mouth_smile; }
            break;
        case A_HAPPY: case A_PET: eye = &puca_eye_happy; mouth = &puca_mouth_open; break;
        case A_REFUSE: eye = &puca_eye_blink; mouth = &puca_mouth_flat; break;
        case A_HEAL: eye = at < 900 ? &puca_eye_wide : &puca_eye_happy; mouth = &puca_mouth_smile; break;
        case A_HATCH: eye = &puca_eye_happy; mouth = &puca_mouth_open; break;
        case A_GAME:
            if (g_phase == 1) { eye = &puca_eye_wide; mouth = &puca_mouth_open; }
            else if (g_phase == 2) { eye = g_wins * 2 > G_ROUNDS ? &puca_eye_happy : &puca_eye_sad;
                                     mouth = g_wins * 2 > G_ROUNDS ? &puca_mouth_open : &puca_mouth_frown; }
            break;
        default: break;
        }
        int bw = b->frames[0]->w, bh = b->frames[0]->h;
        float x = p_x, y = FEET - bh + p_jump;
        bool flip = p_dir < 0;
        if (a_kind == A_EVOLVE && at < 2600) {
            /* évolution : l'ancienne et la nouvelle silhouette s'échangent de plus en plus vite */
            const puca_body_t *ob = body_of(a_from_stage, a_from_form);
            uint32_t per = at < 1200 ? 260 : at < 2000 ? 130 : 60;
            bool newer = (at / per) % 2;
            const puca_body_t *sb = newer ? b : ob;
            float sx = p_x + (bw - sb->frames[0]->w) / 2.0f;
            put_puca(sb, 0, NULL, NULL, sx, FEET - sb->frames[0]->h, flip, true);
            sh_x = sx; sh_w = sb->frames[0]->w;
        } else if (a_kind == A_HATCH && at <= 900) {
            /* encore dans l'œuf */
        } else {
            put_puca(b, frame, eye, mouth, x, y, flip, false);
            sh_x = x; sh_w = bw;
        }
        if (!sleeping && m.sick && a_kind != A_HEAL)
            put(SL_HEAD, &puca_it_dizzy, x + bw / 2.0f - 2, y - 6 - (float)((v_ms / 250) % 2), false, false);
        if (!sleeping && !m.sick && m.mood < 250 && a_kind == A_IDLE) {
            put(SL_HEAD, &puca_it_raincloud, x + bw / 2.0f - 6, y - 9, false, false);
            if ((v_ms / 200) % 2) put(SL_RAIN, &puca_it_rain, x + bw / 2.0f - 3 + (float)(v_ms / 200 % 4) * 2, y - 3, false, false);
        }
    }

    /* ombre douce sous le púca ou l'œuf */
    if (v_shadow) {
        int16_t w = sh_w > 0 ? (int16_t)(sh_w * K * 0.86f - (p_jump < 0 ? -p_jump * 3 : 0)) : 0;
        int16_t xx = (int16_t)lroundf((sh_x + sh_w / 2.0f) * K) - w / 2;
        if (w <= 0) hide(v_shadow);
        else {
            if (xx != s_sh_x || w != s_sh_w) {
                lv_obj_set_size(v_shadow, w, 16);
                lv_obj_set_pos(v_shadow, xx, GY - 9);
                s_sh_x = xx; s_sh_w = w;
            }
            show(v_shadow, true);
        }
    }

    /* objets en mouvement */
    if (a_kind == A_EAT || a_kind == A_TREAT) {
        bool cake = a_kind == A_TREAT;
        const puca_spr_t *s = cake ? (at < 900 ? &puca_it_cake : at < 1300 ? &puca_it_cake2 : &puca_it_cake3)
                                   : (at < 900 ? &puca_it_potato : at < 1300 ? &puca_it_potato2 : &puca_it_potato3);
        if (at < 1700) put(SL_ITEM, s, a_item_x, a_item_y, false, false);
    } else if (a_kind == A_CLEAN) {
        put(SL_ITEM, &puca_it_wave, a_item_x, FEET - 6, false, false);
    } else if (a_kind == A_HEAL && at < 900) {
        put(SL_ITEM, &puca_it_potion, a_item_x, a_item_y, false, false);
    } else if (a_kind == A_GAME && g_phase < 2) {
        /* manches : cœurs gagnés, points restants */
        for (int i = 0; i < G_ROUNDS; i++) {
            float xx = LW / 2 - 14 + i * 6;
            if (i < g_round || (i == g_round && g_phase == 1))
                put(SL_DOT + i, (g_bits >> i) & 1 ? &puca_it_heart_s : &puca_it_dot, xx, 21, false, false);
            else put(SL_DOT + i, &puca_it_dot_off, xx, 21, false, false);
        }
    }

    /* particules */
    for (int i = 0; i < NPART; i++) {
        part_t *p = &s_part[i];
        if (p->life <= 0) continue;
        if (p->life > 2 || (v_ms / 80) % 2) put(SL_PART + i, p->s, p->x, p->y, false, false);
    }

    for (int i = 0; i < SL_COUNT; i++)
        if (!s_so[i].used && s_so[i].vis) { lv_obj_add_flag(s_so[i].o, LV_OBJ_FLAG_HIDDEN); s_so[i].vis = 0; }

    /* éclair blanc (éclosion, évolution) */
    lv_opa_t fl = 0;
    if (a_kind == A_HATCH && at > 800 && at < 1000) fl = (lv_opa_t)(255 - (at - 800) * 255 / 200);
    if (a_kind == A_EVOLVE && at > 2550 && at < 2850) fl = (lv_opa_t)(255 - (at - 2550) * 255 / 300);
    if (v_flash) {
        if (fl) { lv_obj_set_style_bg_opa(v_flash, fl, 0); show(v_flash, true); }
        else if (!lv_obj_has_flag(v_flash, LV_OBJ_FLAG_HIDDEN)) hide(v_flash);
    }
}

/* ------------------------------------------------------------------ comportement */
static void refresh_ui(void);

static void finish_anim(void)
{
    a_kind = A_IDLE;
    p_jump = 0;
    refresh_ui();
}

static void step_puca(void)
{
    uint32_t at = v_ms - a_t0;
    bool sleeping = asleep();
    int bw = alive() ? body_w() : 14;

    /* promenade */
    if (a_kind == A_IDLE && !sleeping && alive()) {
        if (v_ms >= p_pause) {
            float speed = (m.sick || m.energy < 200 ? 0.15f : 0.35f) * DT;
            if (fabsf(p_tx - p_x) < 0.5f) {
                p_tx = frand(3, LW - bw - 3);
                p_pause = v_ms + 800 + rnd() % 3500;
            } else {
                p_dir = p_tx > p_x ? 1 : -1;
                p_x += p_dir * speed;
                if (fabsf(p_tx - p_x) < speed) p_x = p_tx;
            }
        }
        if (v_ms >= p_next_blink) { p_blink = v_ms + 140; p_next_blink = v_ms + 1800 + rnd() % 3500; }
    }
    if (sleeping && a_kind == A_IDLE && v_ms % 1280 < TICK_MS)
        part_add(&puca_it_z, p_x + bw * (p_dir > 0 ? 0.7f : 0.1f), FEET - 18, 0.12f * p_dir, -0.18f, 0, 30);

    switch (a_kind) {
    case A_EGGTAP:
        if (at > 320) finish_anim();
        break;
    case A_HATCH:
        if (at > 900 && at - TICK_MS <= 900) {
            float cx = LW / 2;
            for (int i = 0; i < 8; i++) part_add(&puca_it_shell_l, cx, FEET - 10, frand(-0.9f, 0.9f), frand(-1.4f, -0.6f), 0.08f, 25);
            for (int i = 0; i < 6; i++) part_add(&puca_it_spark, frand(cx - 12, cx + 12), frand(FEET - 27, FEET - 7), 0, -0.1f, 0, 14);
            p_x = (LW - body_w()) / 2; p_dir = 1;
        }
        if (at > 1800) finish_anim();
        break;
    case A_EVOLVE:
        if (v_ms % 240 < TICK_MS && at < 2600)
            part_add(&puca_it_spark_s, p_x + frand(-6, (float)bw + 4), frand(FEET - 32, FEET - 2), 0, -0.2f, 0, 10);
        if (at > 2600 && at - TICK_MS <= 2600) {
            for (int i = 0; i < 10; i++) part_add(&puca_it_spark, p_x + bw / 2.0f, FEET - 17, frand(-1.2f, 1.2f), frand(-1.2f, 0.5f), 0.02f, 20);
            sfx(SFX_HAPPY);
        }
        if (at > 3600) { m.prev_stage = m.stage; save(); finish_anim(); }
        break;
    case A_EAT: case A_TREAT:
        if (at < 900) {
            float tx = p_x + (p_dir > 0 ? bw - 2 : -7);
            a_item_x = tx;
            a_item_y = -8 + (FEET - 8 + 8) * (at / 900.0f) * (at / 900.0f);
            if (a_item_y > FEET - 8) a_item_y = FEET - 8;
        }
        if (a_kind == A_TREAT && at > 1300 && at - TICK_MS <= 1300)
            for (int i = 0; i < 4; i++) part_add(&puca_it_heart, p_x + bw / 2.0f + frand(-6, 6), FEET - 25, frand(-0.3f, 0.3f), -0.35f, 0, 22);
        if (at > 2200) finish_anim();
        break;
    case A_HAPPY: case A_PET: {
        int hops = a_kind == A_PET ? 1 : 2;
        float per = 520.0f;
        if (at < per * hops) {
            float u = fmodf((float)at, per) / per;
            p_jump = -10.0f * 4 * u * (1 - u) * (a_kind == A_PET ? 0.5f : 1.0f);
        } else p_jump = 0;
        if (at > per * hops + 300) finish_anim();
        break;
    }
    case A_REFUSE:
        p_dir = (at / 160) % 2 ? -1 : 1;
        if (at > 900) finish_anim();
        break;
    case A_CLEAN:
        a_item_x = -14 + (LW + 16) * (at / 1400.0f);
        if (at > 1400) { for (int i = 0; i < 6; i++) part_add(&puca_it_spark_s, frand(26, 58), frand(FEET - 11, FEET - 1), 0, -0.15f, 0, 12); finish_anim(); }
        break;
    case A_HEAL:
        if (at < 900) { a_item_x = p_x + bw / 2.0f - 3; a_item_y = -8 + (FEET - 30) * (at / 900.0f); }
        if (at > 900 && at - TICK_MS <= 900)
            for (int i = 0; i < 8; i++) part_add(&puca_it_spark_s, p_x + bw / 2.0f, FEET - 18, cosf(i * 0.785f) * 0.6f, sinf(i * 0.785f) * 0.6f, 0, 16);
        if (at > 1800) finish_anim();
        break;
    default: break;
    }

    if (m.stage == PS_GONE && v_ms % 400 < TICK_MS)        /* le tertre des fées scintille */
        part_add((rnd() & 1) ? &puca_it_spark_s : &puca_it_spark, frand(48, 64), frand(FEET - 12, FEET - 3), 0, -0.12f, 0, 16);

    /* particules */
    for (int i = 0; i < NPART; i++) {
        part_t *p = &s_part[i];
        if (p->life <= 0) continue;
        p->x += p->vx * DT; p->y += p->vy * DT; p->vy += p->g * DT;
        p->life -= DT;
    }
}

/* ------------------------------------------------------------------ jeu « gauche ou droite » */
static void game_end(void)
{
    puca_play_done(&m, g_wins, G_ROUNDS);
    save();
    bool won = g_wins * 2 > G_ROUNDS;
    char b[48];
    if (won) {
        sfx(SFX_WIN);
        part_add(&puca_it_clover, p_x + 4, FEET - 30, 0, -0.25f, 0, 30);
        snprintf(b, sizeof b, _("Gagné %d/%d ! +1 trèfle"), g_wins, G_ROUNDS);
        say(b, 2200);
    } else {
        sfx(SFX_LOSE);
        snprintf(b, sizeof b, _("Perdu… %d/%d"), g_wins, G_ROUNDS);
        say(b, 2000);
    }
}

static void game_step(void)
{
    if (a_kind != A_GAME) return;
    uint32_t at = v_ms - g_t;
    if (g_phase == 0) {                      /* il hésite : tourne la tête */
        if ((v_ms / 260) % 2) p_dir = (v_ms / 520) % 2 ? 1 : -1;
    } else if (g_phase == 1) {               /* il a choisi */
        p_dir = g_look;
        if (at > 900) {
            g_round++;
            if (g_round >= G_ROUNDS) {
                g_phase = 2; g_t = v_ms;
                game_end();
                if (g_wins * 2 > G_ROUNDS) { anim(A_GAME); }
            } else {
                g_phase = 0; g_t = v_ms;
            }
        }
    } else if (g_phase == 2) {
        if (g_wins * 2 > G_ROUNDS && at < 1100) {
            float u = fmodf((float)at, 550) / 550.0f;
            p_jump = -9.0f * 4 * u * (1 - u);
        } else p_jump = 0;
        if (at > 1800) {
            hide(v_gamerow);
            show(v_btnrow, true);
            finish_anim();
        }
    }
}

static void guess_cb(lv_event_t *e)
{
    if (a_kind != A_GAME || g_phase != 0) return;
    g_guess = (int)(intptr_t)lv_event_get_user_data(e);
    g_look = (rnd() & 1) ? 1 : -1;
    g_phase = 1;
    g_t = v_ms;
    if (g_look == g_guess) {
        g_wins++;
        g_bits |= (uint8_t)(1u << g_round);
        sfx(SFX_RIGHT);
        part_add(&puca_it_heart_s, p_x + body_w() / 2.0f, FEET - 28, 0, -0.3f, 0, 12);
    } else {
        sfx(SFX_WRONG);
    }
}

/* ------------------------------------------------------------------ actions */
static const char *res_text(puca_res_t r)
{
    switch (r) {
    case PR_ASLEEP:    return _("Chut, il dort…");
    case PR_FULL:      return _("Il n'a plus faim");
    case PR_NO_CLOVER: return _("Plus de trèfle : marche ou joue");
    case PR_TIRED:     return _("Trop fatigué pour jouer");
    case PR_SICK:      return _("Malade : soigne-le d'abord");
    default:           return "";
    }
}

static bool busy_anim(void) { return a_kind != A_IDLE; }

static void refuse(puca_res_t r)
{
    say(res_text(r), 1600);
    if (r != PR_ASLEEP) { anim(A_REFUSE); sfx(SFX_REFUSE); }
}

static void feed_choice_cb(lv_event_t *e)
{
    int treat = (int)(intptr_t)lv_event_get_user_data(e);
    hide(v_feed);
    time_t now = time(NULL);
    puca_res_t r = treat ? puca_treat(&m, now, tz()) : puca_feed(&m, now, tz());
    if (r != PR_OK) { refuse(r); return; }
    save();
    anim(treat ? A_TREAT : A_EAT);
    sfx(treat ? SFX_TREAT : SFX_EAT);
    refresh_ui();
}

enum { B_FEED, B_PLAY, B_CARE, B_LIGHT };

static void act_cb(lv_event_t *e)
{
    int a = (int)(intptr_t)lv_event_get_user_data(e);
    if (busy_anim() || !alive()) return;
    time_t now = time(NULL);
    puca_res_t r;
    switch (a) {
    case B_FEED:                             /* nourrir : repas ou gâteau */
        if (asleep()) { refuse(PR_ASLEEP); return; }
        show(v_feed, true);
        refresh_ui();
        return;
    case B_PLAY:                             /* jouer */
        r = puca_play_ok(&m, now, tz());
        if (r != PR_OK) { refuse(r); return; }
        g_round = g_wins = g_phase = 0;
        g_bits = 0;
        g_t = v_ms;
        anim(A_GAME);
        hide(v_btnrow);
        show(v_gamerow, true);
        say(_("Gauche ou droite ?"), 1800);
        return;
    case B_CARE:                             /* soigner s'il est malade, sinon laver */
        if (puca_heal(&m) == PR_OK) { save(); anim(A_HEAL); sfx(SFX_HEAL); break; }
        if (puca_clean(&m) == PR_OK) { save(); anim(A_CLEAN); sfx(SFX_CLEAN); break; }
        say(_("Il va très bien"), 1400);
        return;
    case B_LIGHT:                            /* lumière */
        if (puca_bedtime(&m, now, tz()) == PR_TOO_EARLY) { say(_("Trop tôt pour dormir"), 1600); return; }
        save(); sfx(SFX_LIGHT);
        relight(true);
        break;
    }
    refresh_ui();
}

static void root_cb(lv_event_t *e)
{
    if (m.stage == PS_EGG && a_kind != A_HATCH) {
        if (!wclock_time_valid()) return;
        if (puca_egg_tap(&m, time(NULL))) {
            save();
            anim(A_HATCH);
            sfx(SFX_HATCH);
            if (settings_get()->vibrate) power_vibrate(VIB_SHORT);
            hide(v_hint);
        } else {
            anim(A_EGGTAP);
            sfx(SFX_TAP);
        }
        refresh_ui();
        return;
    }
    if (!alive() || busy_anim() || asleep()) return;
    /* une caresse : petit bond et un cœur */
    lv_point_t pt;
    lv_indev_get_point(lv_indev_active(), &pt);
    float lx = (float)pt.x / K, ly = (float)pt.y / K;
    const puca_body_t *b = body_of(m.stage, m.form);
    if (lx >= p_x - 4 && lx <= p_x + body_w() + 4 && ly >= FEET - b->frames[0]->h - 6 && ly <= FEET + 4) {
        anim(A_PET);
        part_add(&puca_it_heart_s, p_x + body_w() / 2.0f, FEET - 24, 0, -0.3f, 0, 14);
        sfx(SFX_TAP);
    }
}

static void new_egg_cb(lv_event_t *e)
{
    memset(s_part, 0, sizeof s_part);
    puca_new(&m);
    save();
    hide(v_gone);
    show(v_btnrow, true);
    refresh_ui();
}

static void spk_cb(lv_event_t *e)
{
    m.sound = !m.sound;
    save();
    refresh_ui();
    if (m.sound) puca_sfx(SFX_TAP);
}

static void back_cb(lv_event_t *e) { app_close(); }

static void feed_close_cb(lv_event_t *e) { hide(v_feed); }

/* ------------------------------------------------------------------ zones à redessiner */
/* Tant que l'app est affichée, toute zone à redessiner prend la largeur de
 * l'écran : la dalle ne reçoit que des bandes pleine largeur (comme le
 * reste de l'interface et les jeux), jamais de fenêtres étroites côte à
 * côte, qui laissaient des traits noirs et des pixels parasites derrière le
 * púca (0.3.10). Les grandes bandes partent aussi calées sur le TE. */
static bool s_wide;

static void wide_cb(lv_event_t *e)
{
    lv_area_t *a = lv_event_get_param(e);
    a->x1 = 0;
    a->x2 = SW - 1;
}

static void wide_inval(bool on)
{
    lv_display_t *d = lv_display_get_default();
    if (!d || on == s_wide) return;
    if (on) lv_display_add_event_cb(d, wide_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    else lv_display_remove_event_cb_with_user_data(d, wide_cb, NULL);
    s_wide = on;
}

/* ------------------------------------------------------------------ interface */
static const char *stage_name(void)
{
    switch (m.stage) {
    case PS_EGG:  return _("Œuf");
    case PS_BABY: return _("Bébé");
    case PS_TEEN: return _("Jeune");
    case PS_GONE: return "";
    default: return m.form == PF_HARE ? _("Lièvre") : m.form == PF_GOAT ? _("Bouc") : _("Cheval");
    }
}

static lv_color_t c_btn(void) { return lv_color_hex(0x10131c); }

static void refresh_ui(void)
{
    if (!v_root) return;
    char b[48];
    uint32_t h = wclock_time_valid() ? puca_age_h(&m, time(NULL)) : 0;
    if (alive()) {
        if (h < 48) snprintf(b, sizeof b, "%s · %lu h", stage_name(), (unsigned long)h);
        else snprintf(b, sizeof b, _("%s · %lu j"), stage_name(), (unsigned long)(h / 24));
    } else snprintf(b, sizeof b, "%s", stage_name());
    lv_label_set_text(v_stage, b);
    show(v_stage, b[0] != 0);
    lv_label_set_text(v_spk_lbl, m.sound ? LV_SYMBOL_VOLUME_MAX : LV_SYMBOL_MUTE);

    bool al = alive();
    snprintf(b, sizeof b, "%u", (unsigned)m.clovers);
    lv_label_set_text(v_clov_lbl, b);
    show(v_clov, al);

    const uint16_t vals[3] = { m.hunger, m.mood, m.energy };
    for (int i = 0; i < 3; i++) lv_bar_set_value(v_bar[i], al ? vals[i] / 10 : 0, LV_ANIM_ON);

    bool sl = asleep();
    bool eve = wclock_time_valid() && puca_evening(time(NULL), tz());
    const bool en[4] = { al && !sl, al && !sl && !m.sick, al && (m.poops > 0 || m.sick), al && (sl || eve) };
    for (int i = 0; i < 4; i++) lv_obj_set_style_opa(v_btn[i], en[i] ? LV_OPA_COVER : LV_OPA_50, 0);
    /* soin : potion violette s'il est malade, eau brune s'il faut laver */
    lv_obj_set_style_bg_color(v_btn[B_CARE], m.sick ? lv_color_hex(0x6a2fa0) : m.poops >= 2 ? lv_color_hex(0x6a4520) : c_btn(), 0);
    lv_obj_set_style_bg_opa(v_btn[B_CARE], m.sick || m.poops >= 2 ? LV_OPA_90 : LV_OPA_60, 0);
    if (v_care_img) lv_image_set_src(v_care_img, icon(m.sick ? &puca_it_b_heal : &puca_it_b_clean, 4));
    if (v_light_img) lv_image_set_src(v_light_img, icon(m.light_off ? &puca_it_b_sun : &puca_it_b_moon, 4));

    show(v_hint, m.stage == PS_EGG && a_kind != A_HATCH);
    if (m.stage == PS_GONE) { show(v_gone, true); hide(v_btnrow); }
    show(v_noclock, !wclock_time_valid());
}

static void handle_events(void)
{
    /* ce qui s'est passé pendant l'absence (ou depuis la dernière mise à jour) */
    if (m.events & PE_GONE) {
        memset(s_part, 0, sizeof s_part);
        show(v_gone, true);
    } else if ((m.events & PE_EVOLVED) || (m.stage != m.prev_stage && m.stage >= PS_TEEN && m.stage <= PS_ADULT)) {
        a_from_stage = m.prev_stage == PS_EGG ? PS_BABY : m.prev_stage;
        a_from_form = PF_NONE;
        p_x = (LW - body_w()) / 2.0f;
        anim(A_EVOLVE);
        sfx(SFX_EVOLVE);
        say(_("Il grandit !"), 3400);
    } else if (m.events & PE_SICK) {
        say(_("Il est tombé malade"), 2000);
    } else if (m.events & PE_CLOVER) {
        say(_("Des trèfles grâce à tes pas !"), 2000);
        part_add(&puca_it_clover, 6, 22, 0, -0.2f, 0, 20);
        sfx(SFX_CLOVER);
    }
    m.events = 0;
    save();
}

static void tick_cb(lv_timer_t *t)
{
    v_ms += TICK_MS;
    step_puca();
    game_step();
    if (v_ms % 1000 < TICK_MS) relight(false);
    render();
    if (v_msg_until && v_ms >= v_msg_until) { hide(v_msg); v_msg_until = 0; }
    if ((v_ms % 30000) < TICK_MS && a_kind == A_IDLE) {        /* toutes les 30 s */
        model_update();
        if (m.events & PE_POOP) sfx(SFX_TAP);
        if (m.events) handle_events();
        refresh_ui();
    }
}

/* pastille sombre translucide, lisible sur le ciel comme sur l'herbe */
static lv_obj_t *pill(lv_obj_t *par, int w, int h)
{
    lv_obj_t *p = lv_obj_create(par);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, w, h);
    lv_obj_set_style_radius(p, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(p, c_btn(), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_60, 0);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    ui_bubble(p);
    return p;
}

static lv_obj_t *round_btn(lv_obj_t *par, int d, lv_event_cb_t cb, intptr_t ud)
{
    lv_obj_t *b = lv_button_create(par);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, d, d);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, c_btn(), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_60, 0);
    lv_obj_set_style_border_color(b, lv_color_white(), 0);
    lv_obj_set_style_border_opa(b, LV_OPA_30, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x3a4258), LV_STATE_PRESSED);
    lv_obj_set_style_transform_scale(b, 236, LV_STATE_PRESSED);     /* s'enfonce sous le doigt */
    lv_obj_set_style_transform_pivot_x(b, d / 2, 0);
    lv_obj_set_style_transform_pivot_y(b, d / 2, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)ud);
    ui_bubble(b);
    return b;
}

static lv_obj_t *label(lv_obj_t *par, const lv_font_t *f, const char *txt)
{
    lv_obj_t *l = ui_label(par, f, lv_color_white(), txt);
    return l;
}

static void create(lv_obj_t *scr)
{
    load();
    v_ms = 0;
    a_kind = A_IDLE;
    memset(s_part, 0, sizeof s_part);
    memset(s_so, 0, sizeof s_so);
    s_sh_x = -1000;

    /* tout l'écran, coins compris */
    v_root = lv_obj_create(scr);
    lv_obj_remove_style_all(v_root);
    lv_obj_set_size(v_root, SW, SH);
    lv_obj_set_pos(v_root, -UI_PAD_X, -UI_PAD_Y);
    lv_obj_remove_flag(v_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(v_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(v_root, root_cb, LV_EVENT_PRESSED, NULL);
    ui_bubble(v_root);

    if (!make_base()) {
        lv_obj_t *l = label(v_root, &font_text, _("Mémoire insuffisante"));
        lv_obj_center(l);
        return;
    }
    v_bg = lv_image_create(v_root);
    lv_image_set_src(v_bg, &s_bg_dsc);
    lv_obj_set_pos(v_bg, 0, 0);

    make_cloud(0, 150, 60, 1);
    make_cloud(1, 104, 44, 6);
    for (int i = 0; i < 2; i++) {
        if (!s_cloud_px[i]) continue;
        v_cloud[i] = lv_image_create(v_root);
        lv_image_set_src(v_cloud[i], &s_cloud_dsc[i]);
        lv_obj_set_pos(v_cloud[i], (int)s_cloud_x[i], i ? 214 : 160);
    }

    /* objets des dessins, dans l'ordre d'empilement */
    for (int i = 0; i < SL_COUNT; i++) {
        if (i == SL_EGG) {
            v_shadow = lv_obj_create(v_root);
            lv_obj_remove_style_all(v_shadow);
            lv_obj_set_style_radius(v_shadow, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(v_shadow, lv_color_hex(0x0a1a08), 0);
            lv_obj_set_style_bg_opa(v_shadow, LV_OPA_40, 0);
            hide(v_shadow);
        }
        lv_obj_t *o = lv_image_create(v_root);
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        s_so[i].o = o;
        s_so[i].x = s_so[i].y = -1;
        s_so[i].rop = 0xFF - 1;
    }

    /* barre du haut : retour, stade et âge, son */
    lv_obj_t *bk = round_btn(v_root, 56, back_cb, 0);
    lv_obj_set_pos(bk, TOP_X, TOP_CY - 28);
    lv_obj_set_ext_click_area(bk, 10);
    lv_obj_center(label(bk, &font_title, LV_SYMBOL_LEFT));
    v_stage = label(v_root, &font_text, "");
    lv_obj_set_style_bg_color(v_stage, c_btn(), 0);
    lv_obj_set_style_bg_opa(v_stage, LV_OPA_60, 0);
    lv_obj_set_style_radius(v_stage, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(v_stage, 18, 0);
    lv_obj_set_style_pad_ver(v_stage, 9, 0);
    lv_obj_align(v_stage, LV_ALIGN_TOP_MID, 0, TOP_CY - 22);
    v_spk = round_btn(v_root, 56, spk_cb, 0);
    lv_obj_set_pos(v_spk, SW - TOP_X - 56, TOP_CY - 28);
    lv_obj_set_ext_click_area(v_spk, 10);
    v_spk_lbl = label(v_spk, &font_text, LV_SYMBOL_VOLUME_MAX);
    lv_obj_center(v_spk_lbl);

    /* jauges : satiété, humeur, énergie */
    static const puca_spr_t *const gi[3] = { &puca_it_potato, &puca_it_heart, &puca_it_bolt };
    static const uint32_t gc[3] = { 0xf0a860, 0xff5a7a, 0xffd84a };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *p = pill(v_root, GAUGE_W, 44);
        lv_obj_set_pos(p, GAUGE_X + i * (GAUGE_W + 8), GAUGE_Y);
        lv_obj_t *im = lv_image_create(p);
        lv_image_set_src(im, icon(gi[i], 4));
        lv_obj_align(im, LV_ALIGN_LEFT_MID, 8, 0);
        v_bar[i] = lv_bar_create(p);
        lv_obj_set_size(v_bar[i], GAUGE_W - 56, 14);
        lv_obj_align(v_bar[i], LV_ALIGN_LEFT_MID, 46, 0);
        lv_bar_set_range(v_bar[i], 0, 100);
        lv_obj_set_style_bg_color(v_bar[i], lv_color_hex(0x40444f), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(v_bar[i], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(v_bar[i], lv_color_hex(gc[i]), LV_PART_INDICATOR);
        lv_obj_set_style_radius(v_bar[i], 7, LV_PART_MAIN);
        lv_obj_set_style_radius(v_bar[i], 7, LV_PART_INDICATOR);
        lv_obj_set_style_anim_duration(v_bar[i], 400, 0);
        ui_bubble(v_bar[i]);
    }

    /* trèfles */
    v_clov = pill(v_root, 86, 42);
    lv_obj_set_pos(v_clov, GAUGE_X, CLOV_Y);
    lv_obj_t *ci = lv_image_create(v_clov);
    lv_image_set_src(ci, icon(&puca_it_clover, 3));
    lv_obj_align(ci, LV_ALIGN_LEFT_MID, 10, 0);
    v_clov_lbl = label(v_clov, &font_text, "0");
    lv_obj_align(v_clov_lbl, LV_ALIGN_LEFT_MID, 44, 0);

    /* messages */
    v_msg = label(v_root, &font_text, "");
    lv_obj_set_style_bg_color(v_msg, lv_color_hex(0x0b0d14), 0);
    lv_obj_set_style_bg_opa(v_msg, LV_OPA_80, 0);
    lv_obj_set_style_radius(v_msg, 18, 0);
    lv_obj_set_style_pad_hor(v_msg, 16, 0);
    lv_obj_set_style_pad_ver(v_msg, 8, 0);
    lv_label_set_long_mode(v_msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(v_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(v_msg, LV_ALIGN_TOP_MID, 0, MSG_Y);
    hide(v_msg);

    v_hint = label(v_root, &font_text, "");
    lv_obj_set_style_bg_color(v_hint, lv_color_hex(0x0b0d14), 0);
    lv_obj_set_style_bg_opa(v_hint, LV_OPA_70, 0);
    lv_obj_set_style_radius(v_hint, 18, 0);
    lv_obj_set_style_pad_hor(v_hint, 16, 0);
    lv_obj_set_style_pad_ver(v_hint, 8, 0);
    lv_label_set_long_mode(v_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(v_hint, LV_TEXT_ALIGN_CENTER, 0);
    bubble_text(v_hint, _("Touche l'œuf pour le réchauffer"));
    lv_obj_align(v_hint, LV_ALIGN_TOP_MID, 0, MSG_Y);

    /* actions : quatre gros boutons ronds */
    v_btnrow = lv_obj_create(v_root);
    lv_obj_remove_style_all(v_btnrow);
    lv_obj_set_size(v_btnrow, SW, BTN_D + 8);
    lv_obj_set_pos(v_btnrow, 0, BTN_CY - BTN_D / 2 - 4);
    lv_obj_remove_flag(v_btnrow, LV_OBJ_FLAG_SCROLLABLE);
    ui_bubble(v_btnrow);
    static const puca_spr_t *const bi[4] = { &puca_it_b_feed, &puca_it_b_play, &puca_it_b_clean, &puca_it_b_moon };
    for (int i = 0; i < 4; i++) {
        v_btn[i] = round_btn(v_btnrow, BTN_D, act_cb, i);
        lv_obj_set_pos(v_btn[i], SW / 2 + (i * 2 - 3) * BTN_STEP / 2 - BTN_D / 2, 4);
        lv_obj_t *im = lv_image_create(v_btn[i]);
        lv_image_set_src(im, icon(bi[i], 4));
        lv_obj_center(im);
        if (i == B_CARE) v_care_img = im;
        if (i == B_LIGHT) v_light_img = im;
    }

    v_gamerow = lv_obj_create(v_root);
    lv_obj_remove_style_all(v_gamerow);
    lv_obj_set_size(v_gamerow, SW, BTN_D + 8);
    lv_obj_set_pos(v_gamerow, 0, BTN_CY - BTN_D / 2 - 4);
    lv_obj_remove_flag(v_gamerow, LV_OBJ_FLAG_SCROLLABLE);
    ui_bubble(v_gamerow);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *b = round_btn(v_gamerow, BTN_D, guess_cb, i ? 1 : -1);
        lv_obj_set_size(b, 158, BTN_D);
        lv_obj_set_style_transform_pivot_x(b, 79, 0);
        lv_obj_set_style_bg_color(b, C_ACC, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_90, 0);
        lv_obj_set_pos(b, i ? SW / 2 + 6 : SW / 2 - 6 - 158, 4);
        lv_obj_center(label(b, &font_title, i ? LV_SYMBOL_RIGHT : LV_SYMBOL_LEFT));
    }
    hide(v_gamerow);

    /* choix du repas */
    v_feed = lv_obj_create(v_root);
    lv_obj_remove_style_all(v_feed);
    lv_obj_set_size(v_feed, SW, SH);
    lv_obj_set_style_bg_color(v_feed, lv_color_hex(0x05060a), 0);
    lv_obj_set_style_bg_opa(v_feed, LV_OPA_70, 0);
    lv_obj_add_flag(v_feed, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(v_feed, feed_close_cb, LV_EVENT_CLICKED, NULL);
    ui_bubble(v_feed);
    for (int i = 0; i < 2; i++) {
        lv_obj_t *b = lv_button_create(v_feed);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, 160, 200);
        lv_obj_align(b, LV_ALIGN_CENTER, i ? 86 : -86, 10);
        lv_obj_set_style_radius(b, 28, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x1c2030), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x343a52), LV_STATE_PRESSED);
        lv_obj_set_style_border_width(b, 3, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(i ? 0xd89a55 : 0x8a5a30), 0);
        lv_obj_set_style_transform_scale(b, 240, LV_STATE_PRESSED);
        lv_obj_set_style_transform_pivot_x(b, 80, 0);
        lv_obj_set_style_transform_pivot_y(b, 100, 0);
        lv_obj_add_event_cb(b, feed_choice_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        ui_bubble(b);
        lv_obj_t *im = lv_image_create(b);
        lv_image_set_src(im, icon(i ? &puca_it_cake : &puca_it_potato, K));
        lv_obj_align(im, LV_ALIGN_TOP_MID, 0, 34);
        lv_obj_t *l = label(b, &font_title, i ? _("Gâteau") : _("Repas"));
        lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -22);
        if (i) {
            lv_obj_t *c = lv_image_create(b);
            lv_image_set_src(c, icon(&puca_it_clover, 3));
            lv_obj_align(c, LV_ALIGN_TOP_RIGHT, -12, 10);
        }
    }
    hide(v_feed);

    /* parti : nouvel œuf */
    v_gone = lv_obj_create(v_root);
    lv_obj_remove_style_all(v_gone);
    lv_obj_set_size(v_gone, SW, SH);
    lv_obj_remove_flag(v_gone, LV_OBJ_FLAG_SCROLLABLE);
    ui_bubble(v_gone);
    lv_obj_t *gl = label(v_gone, &font_text, _("Ton púca est retourné au tertre des fées."));
    lv_obj_set_width(gl, SW - 70);
    lv_label_set_long_mode(gl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(gl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(gl, lv_color_hex(0x0b0d14), 0);
    lv_obj_set_style_bg_opa(gl, LV_OPA_80, 0);
    lv_obj_set_style_radius(gl, 18, 0);
    lv_obj_set_style_pad_all(gl, 14, 0);
    lv_obj_align(gl, LV_ALIGN_TOP_MID, 0, MSG_Y - 40);
    lv_obj_t *nb = round_btn(v_gone, BTN_D, new_egg_cb, 0);
    lv_obj_set_size(nb, 260, BTN_D);
    lv_obj_set_style_transform_pivot_x(nb, 130, 0);
    lv_obj_set_style_bg_color(nb, C_ACC, 0);
    lv_obj_set_style_bg_opa(nb, LV_OPA_COVER, 0);
    lv_obj_align(nb, LV_ALIGN_TOP_MID, 0, BTN_CY - BTN_D / 2);
    lv_obj_center(label(nb, &font_title, _("Nouvel œuf")));
    hide(v_gone);

    v_noclock = label(v_root, &font_text, _("Heure inconnue : connecte l'iPhone"));
    lv_obj_set_style_bg_color(v_noclock, lv_color_hex(0x0b0d14), 0);
    lv_obj_set_style_bg_opa(v_noclock, LV_OPA_80, 0);
    lv_obj_set_style_radius(v_noclock, 18, 0);
    lv_obj_set_style_pad_all(v_noclock, 12, 0);
    lv_obj_set_width(v_noclock, SW - 70);
    lv_label_set_long_mode(v_noclock, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(v_noclock, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(v_noclock, LV_ALIGN_CENTER, 0, -20);
    hide(v_noclock);

    v_flash = lv_obj_create(v_root);
    lv_obj_remove_style_all(v_flash);
    lv_obj_set_size(v_flash, SW, SH);
    lv_obj_set_style_bg_color(v_flash, lv_color_white(), 0);
    hide(v_flash);

    p_x = p_tx = (LW - (alive() ? body_w() : 14)) / 2; p_dir = 1; p_jump = 0;
    relight(false);                            /* décor gardé : recalculé seulement si l'heure l'exige */
    for (int i = 0; i < 2; i++) if (v_cloud[i]) {    /* nuages : teinte et visibilité du moment */
        lv_obj_set_style_image_recolor(v_cloud[i], s_tint, 0);
        lv_obj_set_style_image_recolor_opa(v_cloud[i], s_tint_opa, 0);
        show(v_cloud[i], !s_li.night);
    }
    refresh_ui();
    render();
}

static void destroy(void)
{
    if (v_tmr) { lv_timer_delete(v_tmr); v_tmr = NULL; }
    wide_inval(false);
    save();
    /* décor, nuages et dessins lissés restent en PSRAM pour la prochaine
     * ouverture (calculés une seule fois par démarrage) */
    for (int i = 0; i < 2; i++) v_cloud[i] = NULL;
    memset(s_so, 0, sizeof s_so);
    v_root = v_bg = v_msg = v_hint = v_stage = v_spk = v_spk_lbl = v_clov = v_clov_lbl = NULL;
    v_btnrow = v_gamerow = v_feed = v_gone = v_noclock = v_light_img = v_care_img = NULL;
    v_shadow = v_flash = NULL;
    for (int i = 0; i < 3; i++) v_bar[i] = NULL;
    for (int i = 0; i < 4; i++) v_btn[i] = NULL;
}

static void on_show(void)
{
    load();
    model_update();
    handle_events();
    if (!v_tmr) v_tmr = lv_timer_create(tick_cb, TICK_MS, NULL);
    else lv_timer_resume(v_tmr);
    wide_inval(true);
    relight(false);
    refresh_ui();
}

static void on_hide(void)
{
    if (v_tmr) lv_timer_pause(v_tmr);
    wide_inval(false);
    if (a_kind == A_GAME) {                   /* partie interrompue : comptée telle quelle */
        if (g_phase < 2) { puca_play_done(&m, g_wins, G_ROUNDS); }
        hide(v_gamerow);
        show(v_btnrow, m.stage != PS_GONE);
    }
    if (a_kind == A_EVOLVE) m.prev_stage = m.stage;
    a_kind = A_IDLE;
    p_jump = 0;
    hide(v_feed);
    save();
}

const app_t app_puca = {
    .name = "Púca",
    .icon = SYM_GHOST,
    .color = 0x7C5CFF,
    .create = create,
    .destroy = destroy,
    .on_show = on_show,
    .on_hide = on_hide,
};
