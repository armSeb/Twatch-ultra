/*
 * app_gps.c — GPS : allumer / couper le GNSS, état du fix, position,
 * satellites par constellation et leur signal, position dans les annonces
 * MeshCore.
 *
 *   ‹ GPS                          [■]
 *   Position 3D                         gros, en couleur
 *   9 satellites · ±2 m
 *   48,85206° N
 *   2,35905° E
 *   Altitude 35 m · 3 km/h
 *   CONSTELLATIONS
 *   ● GPS            5 / 7              utilisés / vus
 *   ● Galileo        3 / 4
 *   SIGNAL           ▆ ▇ ▅ ▃ …          les 12 plus forts, couleur du système
 *   MESHCORE
 *   Position dans les annonces    [■]
 *
 * Ouverte, l'app lit le module en continu (gnss_live) ; fermée, le GNSS
 * reste allumé et lu par fenêtres (gnss.h).
 */
#include <stdio.h>
#include <string.h>
#include "app.h"
#include "ui_kit.h"
#include "i18n.h"
#include "gnss.h"
#include "settings.h"
#include "meshcore/mc_port.h"
#include "esp_attr.h"
#include "esp_timer.h"

#define NBAR   12
#define BAR_W  S(13)
#define BAR_H  S(54)

static const uint32_t SYS_COLOR[GNSS_SYS_N] = {
    0x3B82F6,   /* GPS */
    0xF59E0B,   /* Galileo */
    0xEF4444,   /* GLONASS */
    0x22C55E,   /* BeiDou */
    0xA855F7,   /* QZSS */
    0x14B8A6,   /* NavIC */
    0x8A8F98,   /* SBAS */
};

static lv_obj_t *v_sw, *v_col, *v_off, *v_status, *v_sub, *v_lat, *v_lon, *v_alt;
static lv_obj_t *v_sys_box, *v_sys_row[GNSS_SYS_N], *v_sys_n[GNSS_SYS_N], *v_none;
static lv_obj_t *v_bars, *v_bar[NBAR], *v_bar_l[NBAR], *v_ttff;
static lv_obj_t *v_sw_loc, *v_loc_note;
static lv_timer_t *v_tmr;
static int64_t s_loc_hold_us;              /* interrupteur touché : l'état de MeshCore suit */
EXT_RAM_BSS_ATTR static gnss_info_t s_gi;

/* virgule décimale en français */
static void fr_dec(char *b)
{
    if (i18n_get() != LANG_FR) return;
    for (char *p = b; *p; p++) if (*p == '.' && p[1] >= '0' && p[1] <= '9') *p = ',';
}

static void show(lv_obj_t *o, bool on)
{
    if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void fmt_dur(char *b, size_t n, uint32_t s)
{
    if (s < 60) snprintf(b, n, "%lu s", (unsigned long)s);
    else if (s < 3600) snprintf(b, n, "%lu min %02lu s", (unsigned long)(s / 60), (unsigned long)(s % 60));
    else snprintf(b, n, "%lu h %02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60));
}

static void refresh_loc(void)
{
    mc_status_t ms;
    mc_get_status(&ms);
    bool run = mc_running();
    if (run && esp_timer_get_time() > s_loc_hold_us)
        lv_obj_set_state(v_sw_loc, LV_STATE_CHECKED, ms.loc_share);
    if (run) lv_obj_remove_state(v_sw_loc, LV_STATE_DISABLED);
    else lv_obj_add_state(v_sw_loc, LV_STATE_DISABLED);
    char b[96];
    if (!run) snprintf(b, sizeof b, "%s", _("MeshCore est coupé."));
    else if (!ms.loc_share) snprintf(b, sizeof b, "%s", _("Les annonces ne donnent pas de position."));
    else if (gnss_is_on()) snprintf(b, sizeof b, "%s", _("Position du GPS, ou la dernière connue sans fix."));
    else snprintf(b, sizeof b, "%s", _("GPS coupé : dernière position connue."));
    lv_label_set_text(v_loc_note, b);
}

static void refresh(void)
{
    if (!v_col) return;
    gnss_get(&s_gi);
    const gnss_state_t *st = &s_gi.st;
    if (!lv_obj_has_state(v_sw, LV_STATE_PRESSED)) lv_obj_set_state(v_sw, LV_STATE_CHECKED, s_gi.on);
    show(v_off, !s_gi.on);
    show(v_status, s_gi.on);
    show(v_sub, s_gi.on);
    bool fix = s_gi.on && st->fix >= 2 && s_gi.fix_age_s < 10;
    show(v_lat, fix);
    show(v_lon, fix);
    show(v_alt, fix);
    show(v_sys_box, s_gi.on && s_gi.detected);
    show(v_bars, s_gi.on && st->nsat > 0);
    show(v_ttff, s_gi.on && s_gi.detected);
    refresh_loc();
    if (!s_gi.on) return;

    char b[96], d[32];
    if (!s_gi.detected) {
        lv_label_set_text(v_status, s_gi.on_s < 20 ? _("Démarrage…") : _("Module muet"));
        lv_obj_set_style_text_color(v_status, s_gi.on_s < 20 ? C_DIM : C_BAD, 0);
        lv_label_set_text(v_sub, s_gi.on_s < 20 ? "" : _("Aucune trame du GNSS"));
        return;
    }
    if (fix) {
        lv_label_set_text(v_status, st->fix == 3 ? _("Position 3D") : _("Position 2D"));
        lv_obj_set_style_text_color(v_status, C_OK, 0);
        int prec = st->hdop > 0 ? (int)(st->hdop * 2.5f + 0.5f) : 0;
        if (prec < 1) prec = 1;
        if (st->hdop > 0) snprintf(b, sizeof b, _("%u satellites · ±%d m"), st->sats_used, prec);
        else snprintf(b, sizeof b, _("%u satellites"), st->sats_used);
        lv_label_set_text(v_sub, b);
        snprintf(b, sizeof b, "%.5f° %s", st->lat < 0 ? -st->lat : st->lat, st->lat < 0 ? "S" : "N");
        fr_dec(b);
        lv_label_set_text(v_lat, b);
        snprintf(b, sizeof b, "%.5f° %s", st->lon < 0 ? -st->lon : st->lon, st->lon < 0 ? _("O") : "E");
        fr_dec(b);
        lv_label_set_text(v_lon, b);
        if (st->alt_ok) snprintf(b, sizeof b, _("Altitude %.0f m · %.0f km/h"), st->alt_m, st->speed_kmh);
        else snprintf(b, sizeof b, _("Vitesse %.0f km/h"), st->speed_kmh);
        lv_label_set_text(v_alt, b);
    } else {
        lv_label_set_text(v_status, _("Recherche…"));
        lv_obj_set_style_text_color(v_status, C_WARN, 0);
        int tracked = 0;
        for (int i = 0; i < st->nsat; i++) if (st->sat[i].snr) tracked++;
        snprintf(b, sizeof b, _("%d satellites captés sur %u"), tracked, st->nsat);
        lv_label_set_text(v_sub, b);
    }

    /* constellations : utilisés / vus */
    int nsys = 0;
    for (int s = 0; s < GNSS_SYS_N; s++) {
        bool any = st->view[s] > 0;
        show(v_sys_row[s], any);
        if (!any) continue;
        nsys++;
        snprintf(b, sizeof b, "%u / %u", st->used[s], st->view[s]);
        lv_label_set_text(v_sys_n[s], b);
        lv_obj_set_style_text_color(v_sys_n[s], st->used[s] ? C_FG : C_DIM, 0);
    }
    show(v_none, nsys == 0);

    /* signal : les NBAR plus forts (tri par sélection, 64 au plus) */
    uint8_t idx[GNSS_SAT_MAX];
    int n = 0;
    for (int i = 0; i < st->nsat; i++) idx[n++] = (uint8_t)i;
    for (int i = 0; i < n && i < NBAR; i++) {
        int best = i;
        for (int k = i + 1; k < n; k++) if (st->sat[idx[k]].snr > st->sat[idx[best]].snr) best = k;
        uint8_t t = idx[i]; idx[i] = idx[best]; idx[best] = t;
    }
    for (int i = 0; i < NBAR; i++) {
        bool has = i < n;
        show(v_bar[i], has);
        show(v_bar_l[i], has);
        if (!has) continue;
        const gnss_sat_t *s = &st->sat[idx[i]];
        int h = s->snr * BAR_H / 50;
        if (h > BAR_H) h = BAR_H;
        if (h < 3) h = 3;
        lv_obj_set_height(v_bar[i], h);
        lv_obj_set_style_bg_color(v_bar[i], lv_color_hex(SYS_COLOR[s->sys]), 0);
        lv_obj_set_style_bg_opa(v_bar[i], s->used ? LV_OPA_COVER : LV_OPA_40, 0);
        snprintf(b, sizeof b, "%u", s->prn);
        lv_label_set_text(v_bar_l[i], b);
    }

    fmt_dur(d, sizeof d, s_gi.on_s);
    if (s_gi.ttff_s >= 0) {
        char t[24];
        fmt_dur(t, sizeof t, (uint32_t)s_gi.ttff_s);
        snprintf(b, sizeof b, _("Premier fix en %s · allumé depuis %s"), t, d);
    } else {
        snprintf(b, sizeof b, _("Allumé depuis %s"), d);
    }
    lv_label_set_text(v_ttff, b);
}

static void tick_cb(lv_timer_t *t) { refresh(); }

static void sw_cb(lv_event_t *e)
{
    bool on = lv_obj_has_state(v_sw, LV_STATE_CHECKED);
    gnss_set(on);
    if (on) gnss_live(true);
    refresh();
}

static void loc_cb(lv_event_t *e)
{
    bool on = lv_obj_has_state(v_sw_loc, LV_STATE_CHECKED);
    if (!mc_set_loc_share(on)) {
        ui_toast(_("MeshCore occupé, réessayer"));
        lv_obj_set_state(v_sw_loc, LV_STATE_CHECKED, !on);
        return;
    }
    s_loc_hold_us = esp_timer_get_time() + 2000000;
    ui_toast(on ? _("Position dans les annonces") : _("Annonces sans position"));
}

static lv_obj_t *mk_switch(lv_obj_t *parent, lv_event_cb_t cb)
{
    lv_obj_t *sw = lv_switch_create(parent);
    lv_obj_set_style_bg_color(sw, C_OFF, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, C_ACC, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, C_FG, LV_PART_KNOB);
    lv_obj_set_style_opa(sw, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_set_ext_click_area(sw, S(8));
    ui_bubble(sw);
    return sw;
}

static lv_obj_t *wrap_label(lv_obj_t *col, const lv_font_t *f, lv_color_t c, const char *t)
{
    lv_obj_t *l = ui_label(col, f, c, t);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, lv_pct(100));
    return l;
}

static lv_obj_t *section(lv_obj_t *col, const char *t)
{
    lv_obj_t *l = ui_label(col, &font_small, C_DIM, t);
    lv_obj_set_style_pad_top(l, S(8), 0);
    return l;
}

static void create(lv_obj_t *scr)
{
    int y = ui_header(scr, "GPS");
    v_sw = mk_switch(scr, sw_cb);
    lv_obj_update_layout(v_sw);
    lv_obj_align(v_sw, LV_ALIGN_TOP_RIGHT, -S(10), (y - lv_obj_get_height(v_sw)) / 2);

    v_col = ui_container(scr);
    lv_obj_set_size(v_col, UI_W, UI_H - y);
    lv_obj_set_pos(v_col, 0, y);
    lv_obj_set_style_pad_hor(v_col, S(12), 0);
    lv_obj_set_style_pad_bottom(v_col, S(16), 0);
    lv_obj_set_style_pad_row(v_col, S(4), 0);
    lv_obj_set_flex_flow(v_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(v_col, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(v_col, LV_SCROLLBAR_MODE_ACTIVE);
    ui_bubble(v_col);

    v_off = wrap_label(v_col, &font_text, C_DIM,
                       _("GPS coupé : il ne consomme rien. L'almanach reste en mémoire pour un fix rapide."));
    v_status = ui_label(v_col, &font_title, C_FG, "");
    v_sub = wrap_label(v_col, &font_text, C_FG, "");
    v_lat = ui_label(v_col, &font_title, C_FG, "");
    v_lon = ui_label(v_col, &font_title, C_FG, "");
    v_alt = wrap_label(v_col, &font_text, C_FG, "");

    v_sys_box = ui_container(v_col);
    lv_obj_set_size(v_sys_box, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(v_sys_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(v_sys_box, S(2), 0);
    ui_bubble(v_sys_box);
    section(v_sys_box, _("SATELLITES : UTILISÉS / VUS"));
    for (int s = 0; s < GNSS_SYS_N; s++) {
        lv_obj_t *r = ui_container(v_sys_box);
        lv_obj_set_size(r, lv_pct(100), S(24));
        ui_bubble(r);
        lv_obj_t *dot = lv_obj_create(r);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, S(10), S(10));
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(SYS_COLOR[s]), 0);
        lv_obj_align(dot, LV_ALIGN_LEFT_MID, 0, 0);
        ui_bubble(dot);
        lv_obj_t *nm = ui_label(r, &font_text, C_FG, gnss_sys_name(s));
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, S(18), 0);
        v_sys_n[s] = ui_label(r, &font_text, C_FG, "");
        lv_obj_align(v_sys_n[s], LV_ALIGN_RIGHT_MID, 0, 0);
        v_sys_row[s] = r;
    }
    v_none = ui_label(v_sys_box, &font_text, C_DIM, _("Aucun satellite en vue"));

    v_bars = ui_container(v_col);
    lv_obj_set_size(v_bars, lv_pct(100), BAR_H + S(48));
    ui_bubble(v_bars);
    lv_obj_t *sl = ui_label(v_bars, &font_small, C_DIM, _("SIGNAL (PLEINE COULEUR : UTILISÉ)"));
    lv_obj_align(sl, LV_ALIGN_TOP_LEFT, 0, S(8));
    for (int i = 0; i < NBAR; i++) {
        int x = i * (UI_W - S(24)) / NBAR;
        v_bar[i] = lv_obj_create(v_bars);
        lv_obj_remove_style_all(v_bar[i]);
        lv_obj_set_style_radius(v_bar[i], S(3), 0);
        lv_obj_set_width(v_bar[i], BAR_W);
        lv_obj_align(v_bar[i], LV_ALIGN_BOTTOM_LEFT, x + S(2), -S(18));
        ui_bubble(v_bar[i]);
        v_bar_l[i] = ui_label(v_bars, &font_small, C_DIM, "");
        lv_obj_set_width(v_bar_l[i], BAR_W + S(8));
        lv_obj_set_style_text_align(v_bar_l[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(v_bar_l[i], LV_ALIGN_BOTTOM_LEFT, x - S(2), 0);
    }
    v_ttff = wrap_label(v_col, &font_small, C_DIM, "");

    section(v_col, "MESHCORE");
    lv_obj_t *r = ui_container(v_col);
    lv_obj_set_size(r, lv_pct(100), S(46));
    ui_bubble(r);
    lv_obj_t *t = ui_label(r, &font_text, C_FG, _("Position"));
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, S(4));
    lv_obj_t *dsc = ui_label(r, &font_small, C_DIM, _("Dans les annonces"));
    lv_obj_align(dsc, LV_ALIGN_TOP_LEFT, 0, S(26));
    v_sw_loc = mk_switch(r, loc_cb);
    lv_obj_align(v_sw_loc, LV_ALIGN_RIGHT_MID, 0, 0);
    v_loc_note = wrap_label(v_col, &font_small, C_DIM, "");

    refresh();
    v_tmr = lv_timer_create(tick_cb, 1000, NULL);
}

static void destroy(void)
{
    if (v_tmr) { lv_timer_delete(v_tmr); v_tmr = NULL; }
    gnss_live(false);
    v_sw = v_col = v_off = v_status = v_sub = v_lat = v_lon = v_alt = NULL;
    v_sys_box = v_none = v_bars = v_ttff = v_sw_loc = v_loc_note = NULL;
    for (int s = 0; s < GNSS_SYS_N; s++) v_sys_row[s] = v_sys_n[s] = NULL;
    for (int i = 0; i < NBAR; i++) v_bar[i] = v_bar_l[i] = NULL;
}

static void on_show(void)
{
    gnss_live(true);
    if (v_tmr) lv_timer_resume(v_tmr);
    refresh();
}

static void on_hide(void)
{
    gnss_live(false);
    if (v_tmr) lv_timer_pause(v_tmr);
}

const app_t app_gps = {
    .name = "GPS",
    .icon = SYM_SATELLITE,
    .color = 0x14B8A6,
    .create = create,
    .destroy = destroy,
    .on_show = on_show,
    .on_hide = on_hide,
    .screen_timeout_ms = 60 * 1000,
};
