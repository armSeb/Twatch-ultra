/* compose.c — voir compose.h */
#include "compose.h"
#include <stdio.h>
#include <string.h>
#include "ui_kit.h"
#include "ui_geom.h"
#include "i18n.h"
#include "emoji.h"
#include "settings.h"
#include "esp_attr.h"
#include "src/misc/lv_area_private.h"   /* lv_area_intersect */

#define TXT_CAP     200                 /* tampon brut (max_bytes <= MC_TEXT_MAX) */
#define DISP_CAP    (TXT_CAP * 2 + 8)
#define KEYS_MAX    48
#define GAP         4
#define T9_COMMIT_MS 1000
#define SLIDE_MS    200

/* géométrie (repère de l'écran entier, 410 × 502) */
#define SCR_W       BOARD_LCD_W
#define SCR_H       BOARD_LCD_H
#define HDR_Y       32
#define HDR_H       52
#define BOX_X       24
#define BOX_Y       90
#define BOX_W       292
#define BOX_H       106
#define SEND_D      64
#define KB_Y        204
#define KB_H        (SCR_H - KB_Y - 8)
#define ROW_H       68
#define ROW_X0      6                   /* rangées 1 à 3 : toute la largeur */
#define ROW_X1      (SCR_W - 6)
#define LAST_X0     44                  /* rangée du bas : hors des coins arrondis */
#define LAST_X1     (SCR_W - 44)

enum { PG_ALPHA, PG_NUM, PG_EMOJI, PG_T9 };
enum { K_CHAR, K_SHIFT, K_BKSP, K_SPACE, K_PAGE, K_QUICK, K_T9, K_ABC };

typedef struct { uint8_t type, arg, u10; const char *txt; } kdef_t;
typedef struct { lv_area_t a; uint8_t type, arg; const char *txt; } key_t;

/* ------------------------------------------------------------------ dispositions */
#define C1(s) { K_CHAR, 0, 10, s }
static const kdef_t AZ1[] = { C1("a"), C1("z"), C1("e"), C1("r"), C1("t"), C1("y"), C1("u"), C1("i"), C1("o"), C1("p") };
static const kdef_t AZ2[] = { C1("q"), C1("s"), C1("d"), C1("f"), C1("g"), C1("h"), C1("j"), C1("k"), C1("l"), C1("m") };
static const kdef_t AZ3[] = { { K_SHIFT, 0, 15, "⇧" }, C1("w"), C1("x"), C1("c"), C1("v"), C1("b"), C1("n"), C1("'"),
                              { K_BKSP, 0, 15, "⌫" } };
static const kdef_t QW1[] = { C1("q"), C1("w"), C1("e"), C1("r"), C1("t"), C1("y"), C1("u"), C1("i"), C1("o"), C1("p") };
static const kdef_t QW2[] = { C1("a"), C1("s"), C1("d"), C1("f"), C1("g"), C1("h"), C1("j"), C1("k"), C1("l"), C1("'") };
static const kdef_t QW3[] = { { K_SHIFT, 0, 15, "⇧" }, C1("z"), C1("x"), C1("c"), C1("v"), C1("b"), C1("n"), C1("m"),
                              { K_BKSP, 0, 15, "⌫" } };
static const kdef_t NU1[] = { C1("1"), C1("2"), C1("3"), C1("4"), C1("5"), C1("6"), C1("7"), C1("8"), C1("9"), C1("0") };
static const kdef_t NU2[] = { C1("@"), C1("#"), C1("€"), C1("%"), C1("&"), C1("-"), C1("+"), C1("("), C1(")"), C1("/") };
static const kdef_t NU3[] = { { K_ABC, 0, 15, "ABC" }, C1("?"), C1("!"), C1(":"), C1(";"), C1("\""), C1("*"), C1("="),
                              { K_BKSP, 0, 15, "⌫" } };
/* rangée du bas des pages lettres et chiffres */
static const kdef_t LASTA[] = { { K_PAGE, PG_NUM, 14, "123" }, { K_PAGE, PG_EMOJI, 10, "\xF0\x9F\x99\x82" },
                                { K_QUICK, 0, 10, SYM_COMMENTS }, { K_SPACE, 0, 36, "" }, C1(","), C1(".") };
static const kdef_t LASTN[] = { { K_ABC, 0, 14, "ABC" }, { K_PAGE, PG_EMOJI, 10, "\xF0\x9F\x99\x82" },
                                { K_QUICK, 0, 10, SYM_COMMENTS }, { K_SPACE, 0, 36, "" }, C1(","), C1(".") };

/* émojis courants (séquences complètes : emoji_text() les réduit à l'affichage) */
static const char *const EMOJIS[] = {
    "\xF0\x9F\x98\x80", "\xF0\x9F\x98\x82", "\xF0\x9F\x98\x8A", "\xF0\x9F\x98\x8D",   /* 😀 😂 😊 😍 */
    "\xF0\x9F\x98\x89", "\xF0\x9F\x98\x8E", "\xF0\x9F\xA4\x94", "\xF0\x9F\x98\xA2",   /* 😉 😎 🤔 😢 */
    "\xF0\x9F\x91\x8D", "\xF0\x9F\x91\x8E", "\xF0\x9F\x91\x8F", "\xF0\x9F\x99\x8F",   /* 👍 👎 👏 🙏 */
    "\xF0\x9F\x91\x8B", "\xF0\x9F\x92\xAA", "\xE2\x9D\xA4\xEF\xB8\x8F", "\xF0\x9F\x94\xA5", /* 👋 💪 ❤️ 🔥 */
    "\xF0\x9F\x8E\x89", "\xE2\x9C\x85", "\xE2\x9D\x8C", "\xF0\x9F\x93\x8D",           /* 🎉 ✅ ❌ 📍 */
    "\xF0\x9F\x9A\x97", "\xF0\x9F\x8F\xA0", "\xE2\x98\x95", "\xF0\x9F\x98\xB4",       /* 🚗 🏠 ☕ 😴 */
};
#define EMOJI_N   (int)(sizeof EMOJIS / sizeof EMOJIS[0])
#define EMOJI_COL 8
static const kdef_t LASTE[] = { { K_ABC, 0, 14, "ABC" }, { K_QUICK, 0, 10, SYM_COMMENTS },
                                { K_SPACE, 0, 36, "" }, { K_BKSP, 0, 14, "⌫" } };

/* T9 : lettres de chaque touche, dans l'ordre du défilement */
static const char *const T9G[10][10] = {
    { " ", "0", NULL },
    { ".", ",", "?", "!", "'", "-", ":", "1", NULL },
    { "a", "b", "c", "2", "à", "â", "ç", NULL },
    { "d", "e", "f", "3", "é", "è", "ê", "ë", NULL },
    { "g", "h", "i", "4", "î", "ï", NULL },
    { "j", "k", "l", "5", NULL },
    { "m", "n", "o", "6", "ô", "œ", "ñ", NULL },
    { "p", "q", "r", "s", "7", NULL },
    { "t", "u", "v", "8", "ù", "û", "ü", NULL },
    { "w", "x", "y", "z", "9", NULL },
};
static const char *const T9L[10] = { "", ".,?!", "abc", "def", "ghi", "jkl", "mno", "pqrs", "tuv", "wxyz" };
static const char *const T9D[10] = { "0", "1", "2", "3", "4", "5", "6", "7", "8", "9" };

/* appui long : accents */
static const char *const ACC[][6] = {
    { "a", "à", "â", "ä", "æ", NULL },
    { "c", "ç", NULL },
    { "e", "é", "è", "ê", "ë", NULL },
    { "i", "î", "ï", NULL },
    { "o", "ô", "ö", "œ", NULL },
    { "u", "ù", "û", "ü", NULL },
    { "y", "ÿ", NULL },
    { "n", "ñ", NULL },
    { "'", "’", "\"", NULL },
    { ".", "…", NULL },
};
#define ACC_N (int)(sizeof ACC / sizeof ACC[0])

static const char *const QUICK[] = {
    N_("OK 👍"), N_("Oui"), N_("Non"), N_("Bien reçu ✅"), N_("J'arrive"), N_("Je suis en route 🚗"),
    N_("J'arrive dans 10 min"), N_("Je te rappelle"), N_("Où es-tu ?"), N_("Tout va bien 🙂"),
    N_("Merci 🙏"), N_("Besoin d'aide !"),
};
#define QUICK_N (int)(sizeof QUICK / sizeof QUICK[0])

/* ------------------------------------------------------------------ état */
EXT_RAM_BSS_ATTR static char s_txt[TXT_CAP + 1];
EXT_RAM_BSS_ATTR static char s_disp[DISP_CAP];
static int s_len, s_max;
static uint32_t s_draft_id;
static bool s_draft_valid;

static lv_obj_t *o_root, *o_box, *o_lbl, *o_count, *o_send, *o_mode, *o_kb, *o_prev, *o_prev_l,
                *o_acc, *o_quick, *o_closing;
static lv_timer_t *s_t9_tmr;
static compose_send_cb_t s_cb;
static void *s_ud;

EXT_RAM_BSS_ATTR static key_t s_keys[KEYS_MAX];
static int s_nkeys, s_page, s_mode, s_shift;    /* shift : 0, 1 (une lettre), 2 (verrouillé) */
static int s_press = -1;                        /* touche sous le doigt */
static bool s_long;                             /* appui long déjà traité */
static bool s_t9_down;                          /* touche T9 déjà prise à l'appui */
static int s_t9_key = -1, s_t9_i, s_t9_bytes;   /* lettre T9 en cours (pas encore validée) */
static uint32_t s_t9_tick;
static int s_acc_row = -1, s_acc_hover = -1, s_acc_n;
static lv_area_t s_acc_area;                    /* bulle d'accents, repère écran */

/* ------------------------------------------------------------------ UTF-8 */
static int cp_len(uint8_t c) { return c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1; }

/* dernier point de code de s_txt : début et valeur */
static int last_cp(int end, uint32_t *cp)
{
    if (end <= 0) return -1;
    int i = end - 1;
    while (i > 0 && ((uint8_t)s_txt[i] & 0xC0) == 0x80) i--;
    const uint8_t *p = (const uint8_t *)s_txt + i;
    int n = cp_len(p[0]);
    uint32_t v = n == 1 ? p[0] : n == 2 ? (p[0] & 0x1F) : n == 3 ? (p[0] & 0x0F) : (p[0] & 0x07);
    for (int k = 1; k < n && i + k < end; k++) v = (v << 6) | (p[k] & 0x3F);
    if (cp) *cp = v;
    return i;
}

static bool is_mod(uint32_t c)      /* se colle au caractère d'avant */
{
    return c == 0xFE0F || c == 0x20E3 || (c >= 0x1F3FB && c <= 0x1F3FF) || (c >= 0xE0020 && c <= 0xE007F);
}

/* efface le dernier caractère visible (une séquence d'émoji entière) */
static void del_last(void)
{
    uint32_t c;
    int i = last_cp(s_len, &c);
    if (i < 0) return;
    s_len = i;
    bool ri = c >= 0x1F1E6 && c <= 0x1F1FF;
    for (;;) {
        uint32_t p;
        int j = last_cp(s_len, &p);
        if (j < 0) break;
        if (is_mod(c) || p == 0x200D || is_mod(p)) {     /* modificateur, ou joint par ZWJ */
            s_len = j;
            if (p == 0x200D) {                           /* ZWJ : et ce qui le précède */
                int k = last_cp(s_len, &c);
                if (k >= 0) s_len = k;
                continue;
            }
            c = p;
            continue;
        }
        if (ri && p >= 0x1F1E6 && p <= 0x1F1FF) s_len = j;   /* drapeau : deux lettres régionales */
        break;
    }
    s_txt[s_len] = 0;
}

/* majuscule d'une lettre (ASCII, Latin-1, œ, ÿ) */
static void upcase(const char *in, char *out, size_t cap)
{
    const uint8_t *p = (const uint8_t *)in;
    if (p[0] >= 'a' && p[0] <= 'z' && !p[1]) { out[0] = (char)(p[0] - 32); out[1] = 0; return; }
    if (p[0] == 0xC3 && p[1] >= 0xA0 && p[1] <= 0xBE && p[1] != 0xB7 && !p[2]) {   /* à..þ sauf ÷ */
        out[0] = (char)0xC3; out[1] = (char)(p[1] - 0x20); out[2] = 0; return;
    }
    if (!strcmp(in, "œ")) { snprintf(out, cap, "Œ"); return; }
    if (!strcmp(in, "ÿ")) { snprintf(out, cap, "Ÿ"); return; }
    snprintf(out, cap, "%s", in);
}

static bool is_letter(const char *s)
{
    const uint8_t *p = (const uint8_t *)s;
    return (p[0] >= 'a' && p[0] <= 'z') || p[0] == 0xC3 || p[0] == 0xC5;
}

/* ------------------------------------------------------------------ texte */
static void refresh_text(void)
{
    if (!o_lbl) return;
    int n = 0;
    if (!s_len) {
        lv_obj_set_style_text_color(o_lbl, C_DIM, 0);
        lv_label_set_text(o_lbl, _("Message…"));
        lv_label_set_text_selection_start(o_lbl, LV_LABEL_TEXT_SELECTION_OFF);
        lv_label_set_text_selection_end(o_lbl, LV_LABEL_TEXT_SELECTION_OFF);
    } else {
        lv_obj_set_style_text_color(o_lbl, C_FG, 0);
        n = (int)emoji_text(s_txt, s_disp, DISP_CAP - 4);
        bool pend = s_t9_key >= 0;
        if (!pend) { s_disp[n++] = '|'; }                /* curseur */
        s_disp[n] = 0;
        lv_label_set_text(o_lbl, s_disp);
        uint32_t chars = 0;
        for (const char *q = s_disp; *q; q++) chars += ((uint8_t)*q & 0xC0) != 0x80;
        if (pend && chars) {                             /* lettre T9 en cours : surlignée */
            lv_label_set_text_selection_start(o_lbl, chars - 1);
            lv_label_set_text_selection_end(o_lbl, chars);
        } else {
            lv_label_set_text_selection_start(o_lbl, LV_LABEL_TEXT_SELECTION_OFF);
            lv_label_set_text_selection_end(o_lbl, LV_LABEL_TEXT_SELECTION_OFF);
        }
    }
    char b[24];
    snprintf(b, sizeof b, "%d/%d", s_len, s_max);
    lv_label_set_text(o_count, b);
    lv_obj_set_style_text_color(o_count, s_len >= s_max ? C_BAD : s_len * 10 >= s_max * 9 ? C_WARN : C_DIM, 0);
    lv_obj_set_style_bg_color(o_send, s_len ? C_ACC : C_OFF, 0);
    lv_obj_update_layout(o_box);
    lv_obj_scroll_to_y(o_box, LV_COORD_MAX, LV_ANIM_OFF);
}

static void kb_invalidate(void) { if (o_kb) lv_obj_invalidate(o_kb); }

/* majuscule automatique : début du message, après « . », « ! » ou « ? » */
static bool s_no_caps;

static void auto_shift(void)
{
    if (s_no_caps) return;
    if (s_shift == 2 || (s_page != PG_ALPHA && s_page != PG_T9)) return;
    int i = s_len;
    while (i > 0 && s_txt[i - 1] == ' ') i--;
    bool want = i == 0 || ((s_txt[i - 1] == '.' || s_txt[i - 1] == '!' || s_txt[i - 1] == '?') && i < s_len);
    int ns = want ? 1 : 0;
    if (ns != s_shift) { s_shift = ns; kb_invalidate(); }
}

static bool insert(const char *s)
{
    char up[8];
    if (s_shift && is_letter(s)) { upcase(s, up, sizeof up); s = up; }
    int n = (int)strlen(s);
    if (s_len + n > s_max || s_len + n > TXT_CAP) {
        lv_obj_set_style_text_color(o_count, C_BAD, 0);
        return false;
    }
    memcpy(s_txt + s_len, s, (size_t)n);
    s_len += n;
    s_txt[s_len] = 0;
    if (s_shift == 1 && is_letter(s)) { s_shift = 0; kb_invalidate(); }
    return true;
}

/* ------------------------------------------------------------------ T9 */
static void invalidate_key(int i);

/* seule la touche T9 concernée est redessinée : tout le clavier prenait
 * assez de temps pour qu'une seconde tape rapide passe inaperçue */
static void invalidate_t9(int d)
{
    for (int i = 0; i < s_nkeys; i++)
        if (s_keys[i].type == K_T9 && s_keys[i].arg == d) { invalidate_key(i); return; }
}

static void t9_commit(void)
{
    if (s_t9_key < 0) return;
    int old = s_t9_key;
    s_t9_key = -1;
    if (s_t9_tmr) lv_timer_pause(s_t9_tmr);
    invalidate_t9(old);
    auto_shift();
    refresh_text();
}

static void t9_tmr_cb(lv_timer_t *t)
{
    if (s_t9_key >= 0 && lv_tick_elaps(s_t9_tick) >= T9_COMMIT_MS) t9_commit();
}

static int s_t9_shift;                          /* majuscule au début de la lettre en cours */

static void t9_tap(int k)
{
    int n = 0;
    while (n < 10 && T9G[k][n]) n++;
    if (s_t9_key == k && lv_tick_elaps(s_t9_tick) < T9_COMMIT_MS) {
        s_len -= s_t9_bytes;                    /* même touche : lettre suivante à la place */
        s_txt[s_len] = 0;
        s_t9_i = (s_t9_i + 1) % n;
    } else {
        t9_commit();
        s_t9_key = k;
        s_t9_i = 0;
        s_t9_shift = s_shift;
    }
    if (s_shift != 2) s_shift = s_t9_shift;     /* la majuscule vaut pour la lettre retenue */
    int before = s_len;
    if (!insert(T9G[k][s_t9_i])) s_t9_key = -1;
    s_t9_bytes = s_len - before;
    s_t9_tick = lv_tick_get();
    if (s_t9_tmr && s_t9_key >= 0) lv_timer_resume(s_t9_tmr);
    invalidate_t9(k);
    refresh_text();
}

/* ------------------------------------------------------------------ disposition */
static void add_row(const kdef_t *d, int n, int x0, int x1, int y)
{
    int units = 0;
    for (int i = 0; i < n; i++) units += d[i].u10;
    int avail = x1 - x0 - GAP * (n - 1), acc = 0, x = x0;
    for (int i = 0; i < n && s_nkeys < KEYS_MAX; i++) {
        acc += d[i].u10;
        int xe = x0 + avail * acc / units + GAP * i;
        key_t *k = &s_keys[s_nkeys++];
        lv_area_set(&k->a, x, y, xe - 1, y + ROW_H - 1);
        k->type = d[i].type;
        k->arg = d[i].arg;
        k->txt = d[i].txt;
        x = xe + GAP;
    }
}

static void add_key(int x, int y, int w, int h, uint8_t type, uint8_t arg, const char *txt)
{
    if (s_nkeys >= KEYS_MAX) return;
    key_t *k = &s_keys[s_nkeys++];
    lv_area_set(&k->a, x, y, x + w - 1, y + h - 1);
    k->type = type;
    k->arg = arg;
    k->txt = txt;
}

static void build_page(int pg)
{
    s_page = pg;
    s_nkeys = 0;
    s_press = -1;
    const int y1 = 0, y2 = ROW_H + GAP, y3 = 2 * (ROW_H + GAP), y4 = 3 * (ROW_H + GAP);
    if (pg == PG_ALPHA) {
        bool qw = settings_get()->kb_layout == KB_LAYOUT_QWERTY;
        add_row(qw ? QW1 : AZ1, 10, ROW_X0, ROW_X1, y1);
        add_row(qw ? QW2 : AZ2, 10, ROW_X0, ROW_X1, y2);
        add_row(qw ? QW3 : AZ3, 9, ROW_X0, ROW_X1, y3);
        add_row(LASTA, 6, LAST_X0, LAST_X1, y4);
    } else if (pg == PG_NUM) {
        add_row(NU1, 10, ROW_X0, ROW_X1, y1);
        add_row(NU2, 10, ROW_X0, ROW_X1, y2);
        add_row(NU3, 9, ROW_X0, ROW_X1, y3);
        add_row(LASTN, 6, LAST_X0, LAST_X1, y4);
    } else if (pg == PG_EMOJI) {
        int w = (ROW_X1 - ROW_X0 - GAP * (EMOJI_COL - 1)) / EMOJI_COL;
        for (int i = 0; i < EMOJI_N && i < 3 * EMOJI_COL; i++)
            add_key(ROW_X0 + (i % EMOJI_COL) * (w + GAP), (i / EMOJI_COL) * (ROW_H + GAP), w, ROW_H, K_CHAR, 1, EMOJIS[i]);
        add_row(LASTE, 4, LAST_X0, LAST_X1, y4);
    } else {                                       /* T9 : 3 grandes colonnes + une étroite */
        int wr = 84, wb = (ROW_X1 - ROW_X0 - wr - 3 * GAP) / 3;
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++)
                add_key(ROW_X0 + c * (wb + GAP), r * (ROW_H + GAP), wb, ROW_H, K_T9, (uint8_t)(1 + r * 3 + c), NULL);
        int xr = ROW_X1 - wr;
        add_key(xr, y1, wr, ROW_H, K_BKSP, 0, "⌫");
        add_key(xr, y2, wr, ROW_H, K_SHIFT, 0, "⇧");
        add_key(xr, y3, wr, ROW_H, K_PAGE, PG_NUM, "123");
        static const kdef_t L9[] = { { K_PAGE, PG_EMOJI, 10, "\xF0\x9F\x99\x82" }, { K_QUICK, 0, 10, SYM_COMMENTS },
                                     { K_T9, 0, 28, "" } };
        add_row(L9, 3, LAST_X0, LAST_X1, y4);
    }
    auto_shift();
    kb_invalidate();
}

static int home_page(void) { return s_mode == KB_MODE_T9 ? PG_T9 : PG_ALPHA; }

/* ------------------------------------------------------------------ dessin du clavier */
static const char *key_label(const key_t *k, char *buf, size_t cap)
{
    if (k->type == K_CHAR) {
        if (k->arg == 1) { emoji_text(k->txt, buf, cap); return buf; }
        if (s_shift && s_page == PG_ALPHA && is_letter(k->txt)) { upcase(k->txt, buf, cap); return buf; }
        return k->txt;
    }
    if (k->type == K_PAGE && k->arg == PG_EMOJI) { emoji_text(k->txt, buf, cap); return buf; }
    if (k->type == K_SPACE) return _("espace");
    return k->txt;
}

static void draw_text(lv_layer_t *layer, const lv_area_t *a, const lv_font_t *f, lv_color_t c, const char *t, int dy)
{
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.font = f;
    d.color = c;
    d.text = t;
    d.align = LV_TEXT_ALIGN_CENTER;
    int32_t lh = lv_font_get_line_height(f);
    lv_area_t r = *a;
    r.y1 = a->y1 + (lv_area_get_height(a) - lh) / 2 + dy;
    r.y2 = r.y1 + lh - 1;
    lv_draw_label(layer, &d, &r);
}

static void kb_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t co, clip = layer->_clip_area;
    lv_obj_get_coords(lv_event_get_target(e), &co);     /* o_kb est NULL pendant la fermeture animée */
    char buf[24];
    for (int i = 0; i < s_nkeys; i++) {
        const key_t *k = &s_keys[i];
        lv_area_t a = k->a, tmp;
        lv_area_move(&a, co.x1, co.y1);
        if (!lv_area_intersect(&tmp, &a, &clip)) continue;       /* une bande à la fois */
        bool pr = i == s_press;
        lv_color_t bg = C_CARD, fg = C_FG;
        if (k->type != K_CHAR && k->type != K_T9 && k->type != K_SPACE) bg = lv_color_hex(0x2A2E36);
        if (k->type == K_SHIFT && s_shift == 1) { bg = C_ACC; }
        if (k->type == K_SHIFT && s_shift == 2) { bg = C_FG; fg = C_BG; }
        if (k->type == K_T9 && k->arg == s_t9_key && s_t9_key >= 0) bg = lv_color_hex(0x1E3A8A);
        if (pr) bg = C_OFF;
        lv_draw_rect_dsc_t r;
        lv_draw_rect_dsc_init(&r);
        r.bg_color = bg;
        r.bg_opa = LV_OPA_COVER;
        r.radius = 10;
        lv_draw_rect(layer, &r, &a);
        if (k->type == K_T9) {
            int d = k->arg;
            if (d == 0) {                                     /* espace (appui long : 0) */
                draw_text(layer, &a, &font_text, C_DIM, _("espace"), 0);
                continue;
            }
            const char *l = T9L[d];
            if (s_shift && d >= 2) { int n = 0; for (; l[n] && n < 7; n++) buf[n] = (char)(l[n] - 32); buf[n] = 0; l = buf; }
            draw_text(layer, &a, &font_title, fg, l, -8);
            draw_text(layer, &a, &font_small, C_DIM, T9D[d], 20);
            continue;
        }
        const char *t = key_label(k, buf, sizeof buf);
        /* ⇧ ⌫ : dans font_text seulement */
        bool small = k->type == K_PAGE || k->type == K_ABC || k->type == K_SPACE || k->type == K_SHIFT ||
                     k->type == K_BKSP;
        if (k->type == K_PAGE && k->arg == PG_EMOJI) small = false;
        draw_text(layer, &a, small ? &font_text : &font_title, fg, t, 0);
    }
}

/* ------------------------------------------------------------------ bulle d'aperçu, accents */
static void preview_show(int i)
{
    if (!o_prev) return;
    const key_t *k = i >= 0 ? &s_keys[i] : NULL;
    if (!k || k->type != K_CHAR) { lv_obj_add_flag(o_prev, LV_OBJ_FLAG_HIDDEN); return; }
    char buf[24];
    lv_label_set_text(o_prev_l, key_label(k, buf, sizeof buf));
    int w = 70, h = 84;
    int cx = (k->a.x1 + k->a.x2) / 2;
    int x = cx - w / 2;
    if (x < 4) x = 4;
    if (x + w > SCR_W - 4) x = SCR_W - 4 - w;
    lv_obj_set_pos(o_prev, x, KB_Y + k->a.y1 - h + 8);
    lv_obj_remove_flag(o_prev, LV_OBJ_FLAG_HIDDEN);
}

static int acc_row_of(const key_t *k)
{
    if (k->type != K_CHAR || k->arg == 1 || s_page != PG_ALPHA) return -1;
    for (int r = 0; r < ACC_N; r++) if (!strcmp(ACC[r][0], k->txt)) return r;
    return -1;
}

static void acc_close(void)
{
    if (o_acc) { lv_obj_delete(o_acc); o_acc = NULL; }
    s_acc_row = -1;
    s_acc_hover = -1;
}

static void acc_hover(int h)
{
    if (h == s_acc_hover || !o_acc) return;
    s_acc_hover = h;
    for (int i = 0; i < s_acc_n; i++) {
        lv_obj_t *c = lv_obj_get_child(o_acc, i);
        lv_obj_set_style_bg_color(c, i == h ? C_ACC : C_OFF, 0);
    }
}

static void acc_open(int ki)
{
    const key_t *k = &s_keys[ki];
    int r = acc_row_of(k);
    if (r < 0) return;
    s_acc_row = r;
    s_acc_n = 0;
    while (s_acc_n < 5 && ACC[r][s_acc_n + 1]) s_acc_n++;
    int w = 58, h = 76, tw = s_acc_n * w + 8;
    /* la première variante juste au-dessus de la touche : le doigt glisse vers la droite */
    int cx = (k->a.x1 + k->a.x2) / 2, x = cx - w / 2 - 4;
    if (x < 4) x = 4;
    if (x + tw > SCR_W - 4) x = SCR_W - 4 - tw;
    int y = KB_Y + k->a.y1 - h - 4;
    o_acc = ui_container(o_root);
    lv_obj_add_flag(o_acc, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_pos(o_acc, x, y);
    lv_obj_set_size(o_acc, tw, h);
    lv_obj_set_style_bg_color(o_acc, C_OFF, 0);
    lv_obj_set_style_bg_opa(o_acc, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o_acc, 12, 0);
    lv_obj_set_style_pad_all(o_acc, 4, 0);
    lv_obj_remove_flag(o_acc, LV_OBJ_FLAG_CLICKABLE);
    char buf[8];
    for (int i = 0; i < s_acc_n; i++) {
        lv_obj_t *c = ui_label(o_acc, &font_title, C_FG, "");
        const char *t = ACC[r][i + 1];
        if (s_shift && is_letter(t)) { upcase(t, buf, sizeof buf); t = buf; }
        lv_label_set_text(c, t);
        lv_obj_set_size(c, w, h - 8);
        lv_obj_set_pos(c, i * w, 0);
        lv_obj_set_style_text_align(c, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(c, (h - 8 - lv_font_get_line_height(&font_title)) / 2, 0);
        lv_obj_set_style_radius(c, 10, 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(c, C_OFF, 0);
    }
    s_acc_area.x1 = x + 4;
    s_acc_area.y1 = y;
    s_acc_area.x2 = x + 4 + s_acc_n * w - 1;
    s_acc_area.y2 = y + h - 1;
    s_acc_hover = -1;
    acc_hover(0);
    lv_obj_add_flag(o_prev, LV_OBJ_FLAG_HIDDEN);
}

/* ------------------------------------------------------------------ réponses rapides */
static void quick_close(void)
{
    if (o_quick) { lv_obj_delete(o_quick); o_quick = NULL; }
}

static void quick_close_async(void *arg) { quick_close(); }

static void quick_pick_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= QUICK_N) return;
    t9_commit();
    const char *q = _(QUICK[i]);
    if (s_len && s_txt[s_len - 1] != ' ') insert(" ");
    int sh = s_shift;
    s_shift = 0;                                /* le texte garde sa casse */
    insert(q);
    s_shift = sh;
    auto_shift();
    refresh_text();
    lv_async_call(quick_close_async, NULL);    /* hors de son propre clic */
}

static void quick_back_cb(lv_event_t *e) { lv_async_call(quick_close_async, NULL); }

static void quick_open(void)
{
    if (o_quick) return;
    o_quick = ui_container(o_root);
    lv_obj_add_flag(o_quick, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_pos(o_quick, 0, KB_Y);
    lv_obj_set_size(o_quick, SCR_W, SCR_H - KB_Y);
    lv_obj_set_style_bg_color(o_quick, C_BG, 0);
    lv_obj_set_style_bg_opa(o_quick, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(o_quick, 30, 0);
    lv_obj_set_style_pad_bottom(o_quick, 60, 0);          /* la dernière ligne remonte hors des coins */
    lv_obj_set_style_pad_row(o_quick, 6, 0);
    lv_obj_set_flex_flow(o_quick, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(o_quick, LV_DIR_VER);
    lv_obj_add_flag(o_quick, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o_quick, LV_OBJ_FLAG_GESTURE_BUBBLE);
    char b[64];
    snprintf(b, sizeof b, "%s  %s", LV_SYMBOL_LEFT, _("Clavier"));
    lv_obj_t *bk = ui_button(o_quick, b, lv_color_hex(0x2A2E36), quick_back_cb, NULL);
    lv_obj_set_size(bk, lv_pct(100), 56);
    for (int i = 0; i < QUICK_N; i++) {
        lv_obj_t *bt = ui_button(o_quick, "", C_CARD, quick_pick_cb, (void *)(intptr_t)i);
        lv_obj_set_size(bt, lv_pct(100), 60);
        lv_obj_t *l = lv_obj_get_child(bt, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_set_width(l, lv_pct(100));
        emoji_label_set_text(l, _(QUICK[i]));
    }
}

/* ------------------------------------------------------------------ touches */
static void activate(int i)
{
    const key_t *k = &s_keys[i];
    if (k->type != K_T9) t9_commit();
    switch (k->type) {
    case K_CHAR:
        insert(k->txt);
        if (s_page == PG_EMOJI || s_page == PG_NUM) {
            /* une ponctuation de fin de phrase ramène aux lettres */
            if (s_page == PG_NUM && (!strcmp(k->txt, "?") || !strcmp(k->txt, "!"))) build_page(home_page());
        }
        auto_shift();
        refresh_text();
        break;
    case K_SPACE:
        insert(" ");
        auto_shift();
        refresh_text();
        break;
    case K_SHIFT:
        s_shift = (s_shift + 1) % 3;
        kb_invalidate();
        break;
    case K_PAGE:
        build_page(k->arg);
        break;
    case K_ABC:
        build_page(home_page());
        break;
    case K_QUICK:
        quick_open();
        break;
    case K_T9:
        if (k->arg == 0) {                      /* espace */
            t9_commit();
            insert(" ");
            auto_shift();
            refresh_text();
        } else {
            t9_tap(k->arg);
        }
        break;
    default:
        break;
    }
}

static int hit(const lv_point_t *p)
{
    lv_area_t co;
    lv_obj_get_coords(o_kb, &co);
    int x = p->x - co.x1, y = p->y - co.y1, best = -1, bd = 1 << 30;
    for (int i = 0; i < s_nkeys; i++) {
        const lv_area_t *a = &s_keys[i].a;
        /* la plus proche (les interstices comptent pour la voisine) */
        int dx = x < a->x1 ? a->x1 - x : x > a->x2 ? x - a->x2 : 0;
        int dy = y < a->y1 ? a->y1 - y : y > a->y2 ? y - a->y2 : 0;
        int d = dx * dx + dy * dy;
        if (d < bd) { bd = d; best = i; }
    }
    return bd <= 12 * 12 ? best : -1;
}

static void invalidate_key(int i)
{
    if (i < 0 || i >= s_nkeys || !o_kb) return;
    lv_area_t co, a = s_keys[i].a;
    lv_obj_get_coords(o_kb, &co);
    lv_area_move(&a, co.x1, co.y1);
    lv_obj_invalidate_area(o_kb, &a);
}

static void set_press(int i)
{
    if (i == s_press) return;
    int old = s_press;
    s_press = i;
    invalidate_key(old);
    invalidate_key(i);
    preview_show(i);
}

static void kb_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *in = lv_indev_active();
    lv_point_t p = { 0, 0 };
    if (in) lv_indev_get_point(in, &p);
    switch (code) {
    case LV_EVENT_PRESSED:
        s_long = false;
        set_press(hit(&p));
        if (s_press >= 0 && s_keys[s_press].type == K_T9 && s_keys[s_press].arg > 0) {
            /* T9 : la lettre change dès que le doigt touche, pour suivre
             * des tapes rapides ; pas de glissé sur ces grandes touches */
            s_t9_down = true;
            t9_tap(s_keys[s_press].arg);
        } else if (s_press >= 0 && s_keys[s_press].type == K_BKSP) {
            t9_commit();
            del_last();
            auto_shift();
            refresh_text();
        }
        break;
    case LV_EVENT_PRESSING:
        if (o_acc) {
            int h = -1;
            if (p.y >= s_acc_area.y1 - 60 && p.y <= s_acc_area.y2 + 90) {
                h = (p.x - s_acc_area.x1) / (lv_area_get_width(&s_acc_area) / s_acc_n);
                if (h < 0) h = 0;
                if (h >= s_acc_n) h = s_acc_n - 1;
            }
            acc_hover(h);
        } else if (!s_long && !s_t9_down) {
            set_press(hit(&p));
        }
        break;
    case LV_EVENT_LONG_PRESSED:
        if (s_press < 0) break;
        if (s_keys[s_press].type == K_BKSP) break;
        if (acc_row_of(&s_keys[s_press]) >= 0) {
            s_long = true;
            acc_open(s_press);
        } else if (s_keys[s_press].type == K_T9) {
            s_long = true;                                /* le chiffre, à la place de la lettre */
            int d = s_keys[s_press].arg;
            if (s_t9_down && s_t9_key == d) {
                s_len -= s_t9_bytes;
                s_txt[s_len] = 0;
                s_t9_key = -1;
                if (s_t9_tmr) lv_timer_pause(s_t9_tmr);
                if (s_t9_shift == 1 && s_shift == 0) s_shift = 1;
                invalidate_t9(d);
            } else {
                t9_commit();
            }
            insert(T9D[d]);
            auto_shift();
            refresh_text();
            preview_show(-1);
        }
        break;
    case LV_EVENT_LONG_PRESSED_REPEAT:
        if (s_press >= 0 && s_keys[s_press].type == K_BKSP) {
            del_last();
            auto_shift();
            refresh_text();
        }
        break;
    case LV_EVENT_RELEASED:
        if (o_acc) {
            if (s_acc_hover >= 0 && s_acc_row >= 0) {
                t9_commit();
                insert(ACC[s_acc_row][s_acc_hover + 1]);
                auto_shift();
                refresh_text();
            }
            acc_close();
        } else if (!s_long && !s_t9_down && s_press >= 0 && s_keys[s_press].type != K_BKSP) {
            int k = s_press;
            set_press(-1);
            activate(k);
        }
        set_press(-1);
        preview_show(-1);
        s_t9_down = false;
        break;
    case LV_EVENT_PRESS_LOST:
        s_t9_down = false;
        acc_close();
        set_press(-1);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ en-tête */
static void update_mode_btn(void)
{
    lv_label_set_text(lv_obj_get_child(o_mode, 0), s_mode == KB_MODE_T9 ? "ABC" : "T9");
}

static void mode_cb(lv_event_t *e)
{
    t9_commit();
    s_mode = s_mode == KB_MODE_T9 ? KB_MODE_FULL : KB_MODE_T9;
    settings_get()->kb_mode = (uint8_t)s_mode;      /* le dernier mode choisi revient */
    settings_save();
    update_mode_btn();
    quick_close();
    build_page(home_page());
}

static void close_async(void *arg) { compose_close(true); }
static void close_cb(lv_event_t *e) { lv_async_call(close_async, NULL); }

static void send_cb(lv_event_t *e)
{
    t9_commit();
    /* espaces de fin retirés */
    while (s_len && s_txt[s_len - 1] == ' ') s_txt[--s_len] = 0;
    refresh_text();
    if (!s_len || !s_cb) return;
    if (s_cb(s_txt, s_ud)) {
        s_len = 0;
        s_txt[0] = 0;
        s_draft_valid = false;
        lv_async_call(close_async, NULL);
    }
}

static lv_obj_t *round_btn(lv_obj_t *p, const char *sym, lv_color_t bg, lv_event_cb_t cb, int d)
{
    lv_obj_t *b = ui_button(p, sym, bg, cb, NULL);
    lv_obj_set_size(b, d, d);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_text_font(lv_obj_get_child(b, 0), &font_title, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_FLOATING);
    return b;
}

/* ------------------------------------------------------------------ ouverture */
static void closed_cb(lv_anim_t *a)
{
    lv_obj_t *p = lv_anim_get_user_data(a);
    if (p == o_closing) o_closing = NULL;
    if (p) lv_obj_delete(p);
}

void compose_close(bool anim)
{
    if (!o_root) return;
    if (s_t9_tmr) { lv_timer_delete(s_t9_tmr); s_t9_tmr = NULL; }
    s_t9_key = -1;
    s_draft_valid = s_len > 0;                       /* brouillon gardé */
    lv_obj_t *p = o_root;
    o_root = o_box = o_lbl = o_count = o_send = o_mode = o_kb = o_prev = o_prev_l = o_acc = o_quick = NULL;
    s_press = -1;
    if (!anim) { lv_obj_delete(p); return; }
    if (o_closing) { lv_anim_delete(o_closing, NULL); lv_obj_delete(o_closing); }
    o_closing = p;
    lv_obj_remove_flag(p, LV_OBJ_FLAG_CLICKABLE);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, p);
    lv_anim_set_user_data(&a, p);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_values(&a, lv_obj_get_y(p), lv_obj_get_y(p) + SCR_H);
    lv_anim_set_duration(&a, SLIDE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_completed_cb(&a, closed_cb);
    lv_anim_start(&a);
}

bool compose_is_open(void) { return o_root != NULL; }

bool compose_on_back(void)
{
    if (!o_root) return false;
    if (o_quick) { quick_close(); return true; }
    if (s_page != home_page()) { build_page(home_page()); return true; }
    compose_close(true);
    return true;
}

void compose_forget(void)
{
    /* les objets partent avec l'écran */
    if (o_closing) lv_anim_delete(o_closing, NULL);
    if (o_root) lv_anim_delete(o_root, NULL);
    if (s_t9_tmr) { lv_timer_delete(s_t9_tmr); s_t9_tmr = NULL; }
    if (o_root) s_draft_valid = s_len > 0;
    o_root = o_box = o_lbl = o_count = o_send = o_mode = o_kb = o_prev = o_prev_l = o_acc = o_quick = o_closing = NULL;
    s_t9_key = -1;
    s_press = -1;
}

void compose_open(lv_obj_t *scr, const char *title, uint32_t draft_id, int max_bytes,
                  unsigned flags, compose_send_cb_t cb, void *ud)
{
    s_no_caps = flags & COMPOSE_NO_CAPS;
    if (o_root) compose_close(false);
    s_cb = cb;
    s_ud = ud;
    s_max = max_bytes < 1 ? 1 : max_bytes > TXT_CAP ? TXT_CAP : max_bytes;
    if (!s_draft_valid || draft_id != s_draft_id) { s_len = 0; s_txt[0] = 0; }
    while (s_len > s_max) del_last();
    s_draft_id = draft_id;
    s_draft_valid = false;
    s_mode = settings_get()->kb_mode == KB_MODE_T9 ? KB_MODE_T9 : KB_MODE_FULL;
    s_shift = 0;
    s_t9_key = -1;

    /* tout l'écran, par-dessus la marge de l'app */
    o_root = ui_container(scr);
    lv_obj_add_flag(o_root, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(o_root, SCR_W, SCR_H);
    lv_obj_set_pos(o_root, -UI_PAD_X, -UI_PAD_Y);
    lv_obj_set_style_bg_color(o_root, C_BG, 0);
    lv_obj_set_style_bg_opa(o_root, LV_OPA_COVER, 0);
    lv_obj_add_flag(o_root, LV_OBJ_FLAG_CLICKABLE);          /* bloque les touches dessous */
    lv_obj_remove_flag(o_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o_root, LV_OBJ_FLAG_GESTURE_BUBBLE);  /* pas de « retour » en tapant */

    lv_obj_t *x = round_btn(o_root, LV_SYMBOL_CLOSE, lv_color_hex(0x2A2E36), close_cb, HDR_H);
    lv_obj_set_pos(x, 46, HDR_Y);
    lv_obj_t *t = ui_label(o_root, &font_text, C_FG, "");
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_size(t, 196, lv_font_get_line_height(&font_text));     /* une ligne */
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(t, (SCR_W - 196) / 2, HDR_Y + (HDR_H - lv_font_get_line_height(&font_text)) / 2);
    emoji_label_set_text(t, title);
    o_mode = ui_button(o_root, "", lv_color_hex(0x2A2E36), mode_cb, NULL);
    lv_obj_add_flag(o_mode, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(o_mode, 70, HDR_H);
    lv_obj_set_style_radius(o_mode, HDR_H / 2, 0);
    lv_obj_set_pos(o_mode, SCR_W - 46 - 70, HDR_Y);
    update_mode_btn();

    /* champ de texte */
    o_box = ui_container(o_root);
    lv_obj_add_flag(o_box, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_pos(o_box, BOX_X, BOX_Y);
    lv_obj_set_size(o_box, BOX_W, BOX_H);
    lv_obj_set_style_bg_color(o_box, C_CARD, 0);
    lv_obj_set_style_bg_opa(o_box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o_box, 14, 0);
    lv_obj_set_style_pad_hor(o_box, 10, 0);
    lv_obj_set_style_pad_ver(o_box, 6, 0);
    lv_obj_set_scroll_dir(o_box, LV_DIR_VER);
    lv_obj_add_flag(o_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o_box, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_scrollbar_mode(o_box, LV_SCROLLBAR_MODE_ACTIVE);
    o_lbl = ui_label(o_box, &font_text, C_FG, "");
    lv_label_set_long_mode(o_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(o_lbl, lv_pct(100));
    lv_obj_set_style_bg_color(o_lbl, C_ACC, LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(o_lbl, LV_OPA_COVER, LV_PART_SELECTED);
    lv_obj_set_style_text_color(o_lbl, lv_color_white(), LV_PART_SELECTED);

    o_send = round_btn(o_root, LV_SYMBOL_RIGHT, C_OFF, send_cb, SEND_D);
    lv_obj_set_pos(o_send, BOX_X + BOX_W + 12, BOX_Y);
    o_count = ui_label(o_root, &font_small, C_DIM, "");
    lv_obj_add_flag(o_count, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_width(o_count, 84);
    lv_obj_set_style_text_align(o_count, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(o_count, BOX_X + BOX_W + 12 + SEND_D / 2 - 42, BOX_Y + SEND_D + 10);

    /* clavier : un seul objet, dessiné touche par touche */
    o_kb = lv_obj_create(o_root);
    lv_obj_remove_style_all(o_kb);
    lv_obj_add_flag(o_kb, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_pos(o_kb, 0, KB_Y);
    lv_obj_set_size(o_kb, SCR_W, KB_H);
    lv_obj_add_flag(o_kb, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o_kb, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_SCROLL_CHAIN |
                             LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_event_cb(o_kb, kb_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(o_kb, kb_event_cb, LV_EVENT_ALL, NULL);

    /* aperçu de la touche, au-dessus du doigt */
    o_prev = ui_container(o_root);
    lv_obj_add_flag(o_prev, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(o_prev, 70, 84);
    lv_obj_set_style_bg_color(o_prev, C_OFF, 0);
    lv_obj_set_style_bg_opa(o_prev, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o_prev, 14, 0);
    lv_obj_remove_flag(o_prev, LV_OBJ_FLAG_CLICKABLE);
    o_prev_l = ui_label(o_prev, &font_title, C_FG, "");
    lv_obj_center(o_prev_l);
    lv_obj_add_flag(o_prev, LV_OBJ_FLAG_HIDDEN);

    s_t9_tmr = lv_timer_create(t9_tmr_cb, 200, NULL);
    lv_timer_pause(s_t9_tmr);
    build_page(home_page());
    refresh_text();

    /* glisse depuis le bas */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o_root);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_values(&a, -UI_PAD_Y + SCR_H, -UI_PAD_Y);
    lv_anim_set_duration(&a, SLIDE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_obj_set_y(o_root, -UI_PAD_Y + SCR_H);
    lv_anim_start(&a);
}
