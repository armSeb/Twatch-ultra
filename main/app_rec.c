/*
 * app_rec.c — Magnétophone : enregistre le micro sur la carte SD (WAV),
 * liste et relit les enregistrements.
 *
 *   ‹ Magnétophone
 *        ● Enregistrement               état
 *            12:34                      durée
 *        ▮▮▮▮▮▮▮▯▯▯▯                    niveau
 *     Reste 7 h 40 · 32 kHz             place sur la carte
 *    [ ≡ ]     ( ● / ■ )    [32 kHz]    liste, enregistrer/arrêter, qualité
 *
 * L'enregistrement tourne écran éteint (recorder.c) ; le bouton éteint
 * l'écran sans quitter. Quitter l'app ne l'arrête pas : seul ■ l'arrête
 * (on le retrouve en rouvrant l'app).
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "app.h"
#include "ui_kit.h"
#include "i18n.h"
#include "recorder.h"
#include "sdcard.h"
#include "settings.h"
#include "esp_heap_caps.h"

extern const app_t app_rec;

#define C_REC   lv_color_hex(0xDC2626)
#define MAX_FILES 80

/* modèle */
static rec_err_t m_err_seen = REC_OK;
static rec_file_t *m_files;
static int m_nfiles;
static char m_sel[40];                       /* enregistrement ouvert dans le lecteur */
static bool m_del_armed;

/* vue */
static lv_obj_t *v_state, *v_time, *v_level, *v_info, *v_rec, *v_rec_lbl, *v_list_btn, *v_q_btn;
static lv_obj_t *v_list, *v_list_col;                 /* liste (panneau par-dessus) */
static lv_obj_t *v_play, *v_p_title, *v_p_pos, *v_p_len, *v_p_bar, *v_p_btn, *v_p_del;
static lv_timer_t *v_tick;

static void set_text(lv_obj_t *l, const char *t)
{
    if (l && strcmp(lv_label_get_text(l), t)) lv_label_set_text(l, t);
}

static void fmt_dur(char *b, size_t n, uint32_t s)
{
    if (s >= 3600) snprintf(b, n, "%lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60),
                            (unsigned long)(s % 60));
    else snprintf(b, n, "%02lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

static void fmt_left(char *b, size_t n, uint32_t s)
{
    if (s >= 3600) snprintf(b, n, _("Reste %lu h %02lu"), (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60));
    else snprintf(b, n, _("Reste %lu min"), (unsigned long)(s / 60));
}

/* « 20261126-143205.WAV » → « 26/11 14:32 » ; autre nom : tel quel */
static void pretty_name(char *b, size_t n, const char *f)
{
    int y, mo, d, h, mi, s;
    if (sscanf(f, "%4d%2d%2d-%2d%2d%2d", &y, &mo, &d, &h, &mi, &s) == 6)
        snprintf(b, n, "%02d/%02d  %02d:%02d", d, mo, h, mi);
    else {
        strlcpy(b, f, n);
        char *dot = strrchr(b, '.');
        if (dot) *dot = 0;
    }
}

static const char *err_text(rec_err_t e)
{
    switch (e) {
    case REC_ERR_NO_SD: return _("Pas de carte SD");
    case REC_ERR_FULL:  return _("Carte SD pleine : enregistrement arrêté");
    case REC_ERR_SD:    return _("Carte SD retirée ou en erreur");
    case REC_ERR_MIC:   return _("Micro indisponible");
    case REC_ERR_MEM:   return _("Mémoire insuffisante");
    case REC_ERR_BUSY:  return _("Son en cours : réessayer");
    default:            return "";
    }
}

/* ------------------------------------------------------------------ */
static void refresh_main(void)
{
    if (!v_time) return;
    rec_status_t st;
    rec_get(&st);
    bool hq = settings_get()->rec_hq;
    char b[48];

    if (st.err != m_err_seen) {              /* erreur survenue pendant l'enregistrement */
        m_err_seen = st.err;
        if (st.err != REC_OK) ui_toast(err_text(st.err));
    }

    bool sd = sd_mounted();
    if (st.state == REC_RECORDING) {
        snprintf(b, sizeof b, "\xE2\x97\x8F %s", _("Enregistrement"));   /* ● */
        set_text(v_state, b);
        lv_obj_set_style_text_color(v_state, C_REC, 0);
    } else if (st.state == REC_STOPPING) {
        set_text(v_state, _("Écriture sur la carte…"));
        lv_obj_set_style_text_color(v_state, C_DIM, 0);
    } else if (!sd) {
        set_text(v_state, _("Pas de carte SD"));
        lv_obj_set_style_text_color(v_state, C_WARN, 0);
    } else {
        set_text(v_state, _("Prêt"));
        lv_obj_set_style_text_color(v_state, C_DIM, 0);
    }

    bool busy = st.state != REC_IDLE;
    fmt_dur(b, sizeof b, busy ? st.seconds : 0);
    set_text(v_time, b);
    lv_obj_set_style_text_color(v_time, st.state == REC_RECORDING ? C_FG : C_DIM, 0);
    lv_bar_set_value(v_level, st.state == REC_RECORDING ? st.level : 0, LV_ANIM_OFF);

    uint32_t left = busy ? st.left_s : rec_capacity_s(hq);
    int khz = busy ? (int)(st.rate / 1000) : hq ? 32 : 16;
    if (!sd && !busy) b[0] = 0;
    else if (!left && !busy) snprintf(b, sizeof b, "%d kHz", khz);   /* place pas encore mesurée */
    else {
        fmt_left(b, sizeof b, left);
        size_t l = strlen(b);
        snprintf(b + l, sizeof b - l, " · %d kHz", khz);
    }
    set_text(v_info, b);

    /* bouton central : ● pour enregistrer, ■ pour arrêter */
    set_text(v_rec_lbl, busy ? LV_SYMBOL_STOP : SYM_MIC);
    lv_obj_set_style_bg_color(v_rec, busy ? C_FG : C_REC, 0);
    lv_obj_set_style_text_color(v_rec_lbl, busy ? C_REC : C_FG, 0);
    if (st.state == REC_STOPPING || (!busy && !sd)) lv_obj_add_state(v_rec, LV_STATE_DISABLED);
    else lv_obj_remove_state(v_rec, LV_STATE_DISABLED);

    if (busy) {
        lv_obj_add_flag(v_list_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(v_q_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(v_list_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(v_q_btn, LV_OBJ_FLAG_HIDDEN);
        set_text(lv_obj_get_child(v_q_btn, 0), hq ? "32 kHz" : "16 kHz");
    }
}

static void rec_cb(lv_event_t *e)
{
    if (rec_active()) { rec_stop(); refresh_main(); return; }
    m_err_seen = REC_OK;
    if (rec_start(settings_get()->rec_hq) != ESP_OK) {
        rec_status_t st;
        rec_get(&st);
        m_err_seen = st.err;
        ui_toast(st.err != REC_OK ? err_text(st.err) : _("Enregistrement impossible"));
    }
    refresh_main();
}

static void q_cb(lv_event_t *e)
{
    settings_t *s = settings_get();
    s->rec_hq = !s->rec_hq;
    settings_save();
    ui_toast(s->rec_hq ? _("32 kHz : haute qualité") : _("16 kHz : voix, deux fois plus long"));
    refresh_main();
}

/* ------------------------------------------------------------------ */
/* Lecteur                                                              */
/* ------------------------------------------------------------------ */
static void refresh_player(void)
{
    if (!v_play || lv_obj_has_flag(v_play, LV_OBJ_FLAG_HIDDEN)) return;
    uint32_t pos = 0, len = 0;
    bool on = rec_playing();
    if (on) rec_play_pos(&pos, &len);
    else {
        for (int i = 0; i < m_nfiles; i++)
            if (!strcmp(m_files[i].name, m_sel)) len = m_files[i].seconds;
    }
    char b[32];
    fmt_dur(b, sizeof b, pos);
    set_text(v_p_pos, b);
    char l[24];
    fmt_dur(l, sizeof l, len);
    snprintf(b, sizeof b, "/ %s", l);
    set_text(v_p_len, b);
    lv_bar_set_value(v_p_bar, len ? (int)((uint64_t)pos * 1000 / len) : 0, LV_ANIM_OFF);
    set_text(lv_obj_get_child(v_p_btn, 0), on && !rec_paused() ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

static void player_close(void)
{
    rec_play_stop();
    if (v_play) lv_obj_add_flag(v_play, LV_OBJ_FLAG_HIDDEN);
}

static void p_play_cb(lv_event_t *e)
{
    if (rec_playing()) rec_play_pause(!rec_paused());
    else {
        esp_err_t r = rec_play(m_sel);
        if (r == ESP_ERR_NOT_SUPPORTED) ui_toast(_("Format non lu (WAV 16 bits mono seulement)"));
        else if (r != ESP_OK) ui_toast(_("Lecture impossible"));
    }
    refresh_player();
}

static void p_seek_cb(lv_event_t *e)
{
    rec_play_seek((int)(intptr_t)lv_event_get_user_data(e));
}

static void list_fill(void);

static void p_del_cb(lv_event_t *e)
{
    lv_obj_t *lbl = lv_obj_get_child(v_p_del, 0);
    if (!m_del_armed) {
        m_del_armed = true;
        lv_label_set_text_fmt(lbl, LV_SYMBOL_TRASH "  %s", _("Confirmer"));
        lv_obj_set_style_bg_color(v_p_del, C_BAD, 0);
        return;
    }
    m_del_armed = false;
    if (rec_delete(m_sel) != ESP_OK) ui_toast(_("Suppression impossible"));
    else ui_toast(_("Supprimé"));
    player_close();
    list_fill();
}

static void p_back_cb(lv_event_t *e) { player_close(); }

static lv_obj_t *panel(lv_obj_t *scr, const char *title, lv_event_cb_t back, lv_obj_t **title_lbl)
{
    lv_obj_t *p = ui_container(scr);
    lv_obj_set_size(p, UI_W, UI_H);
    lv_obj_set_pos(p, 0, 0);
    lv_obj_set_style_bg_color(p, C_BG, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);              /* n'atteint pas l'écran dessous */
    lv_obj_t *hdr = lv_button_create(p);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, UI_W, S(40));
    lv_obj_set_style_bg_color(hdr, C_CARD, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_event_cb(hdr, back, LV_EVENT_CLICKED, NULL);
    ui_bubble(hdr);
    lv_obj_t *ar = ui_label(hdr, &font_title, C_ACC, LV_SYMBOL_LEFT);
    lv_obj_align(ar, LV_ALIGN_LEFT_MID, S(12), 0);
    lv_obj_t *t = ui_label(hdr, &font_title, C_FG, title);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, S(36), 0);
    if (title_lbl) *title_lbl = t;
    ui_bubble(p);
    return p;
}

static lv_obj_t *round_btn(lv_obj_t *parent, const char *txt, lv_event_cb_t cb, void *ud, lv_color_t bg, int x)
{
    lv_obj_t *b = ui_button(parent, txt, bg, cb, ud);
    lv_obj_set_size(b, S(68), S(52));
    lv_obj_align(b, LV_ALIGN_BOTTOM_MID, x, -S(8));
    lv_obj_set_style_radius(b, S(16), 0);
    lv_obj_set_style_text_font(lv_obj_get_child(b, 0), &font_title, 0);
    return b;
}

static void build_player(lv_obj_t *scr)
{
    v_play = panel(scr, "", p_back_cb, &v_p_title);
    v_p_pos = ui_label(v_play, &font_num, C_FG, "00:00");
    lv_obj_align(v_p_pos, LV_ALIGN_TOP_MID, 0, S(58));
    v_p_len = ui_label(v_play, &font_text, C_DIM, "");
    lv_obj_align(v_p_len, LV_ALIGN_TOP_MID, 0, S(108));
    v_p_bar = lv_bar_create(v_play);
    lv_obj_set_size(v_p_bar, S(200), S(8));
    lv_obj_align(v_p_bar, LV_ALIGN_TOP_MID, 0, S(138));
    lv_bar_set_range(v_p_bar, 0, 1000);
    lv_obj_set_style_bg_color(v_p_bar, C_OFF, LV_PART_MAIN);
    lv_obj_set_style_bg_color(v_p_bar, C_ACC, LV_PART_INDICATOR);
    ui_bubble(v_p_bar);

    v_p_del = ui_button(v_play, "", C_CARD, p_del_cb, NULL);
    lv_obj_set_size(v_p_del, S(160), S(40));
    lv_obj_align(v_p_del, LV_ALIGN_TOP_MID, 0, S(160));
    lv_obj_set_style_text_color(lv_obj_get_child(v_p_del, 0), C_FG, 0);

    round_btn(v_play, LV_SYMBOL_PREV, p_seek_cb, (void *)(intptr_t)-10, C_OFF, -S(78));
    v_p_btn = round_btn(v_play, LV_SYMBOL_PLAY, p_play_cb, NULL, lv_color_hex(0x16A34A), 0);
    round_btn(v_play, LV_SYMBOL_NEXT, p_seek_cb, (void *)(intptr_t)10, C_OFF, S(78));
    lv_obj_add_flag(v_play, LV_OBJ_FLAG_HIDDEN);
}

static void player_open(const char *name)
{
    strlcpy(m_sel, name, sizeof m_sel);
    char b[40];
    pretty_name(b, sizeof b, name);
    lv_label_set_text(v_p_title, b);
    m_del_armed = false;
    lv_label_set_text_fmt(lv_obj_get_child(v_p_del, 0), LV_SYMBOL_TRASH "  %s", _("Supprimer"));
    lv_obj_set_style_bg_color(v_p_del, C_CARD, 0);
    lv_obj_remove_flag(v_play, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(v_play);
    esp_err_t r = rec_play(name);                       /* lecture immédiate */
    if (r == ESP_ERR_NOT_SUPPORTED) ui_toast(_("Format non lu (WAV 16 bits mono seulement)"));
    else if (r != ESP_OK) ui_toast(_("Lecture impossible"));
    refresh_player();
}

/* ------------------------------------------------------------------ */
/* Liste                                                                */
/* ------------------------------------------------------------------ */
static void row_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i >= 0 && i < m_nfiles) player_open(m_files[i].name);
}

static void list_fill(void)
{
    if (!v_list_col) return;
    lv_obj_clean(v_list_col);
    if (!m_files) m_files = heap_caps_malloc(MAX_FILES * sizeof *m_files, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    m_nfiles = m_files ? rec_list(m_files, MAX_FILES) : 0;
    if (m_nfiles <= 0) {
        lv_obj_t *l = ui_label(v_list_col, &font_text, C_DIM,
                               m_nfiles < 0 ? _("Pas de carte SD") : _("Aucun enregistrement"));
        lv_obj_set_width(l, LV_PCT(100));
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        ui_bubble(l);
        if (m_nfiles < 0) m_nfiles = 0;
        return;
    }
    for (int i = 0; i < m_nfiles; i++) {
        const rec_file_t *f = &m_files[i];
        lv_obj_t *r = lv_button_create(v_list_col);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, LV_PCT(100), S(58));
        lv_obj_set_style_radius(r, S(12), 0);
        lv_obj_set_style_bg_color(r, C_CARD, 0);
        lv_obj_set_style_bg_color(r, C_OFF, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(r, S(12), 0);
        lv_obj_add_event_cb(r, row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        ui_bubble(r);
        char b[48];
        pretty_name(b, sizeof b, f->name);
        lv_obj_t *t = ui_label(r, &font_text, C_FG, b);
        lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, S(5));
        char d[16];
        fmt_dur(d, sizeof d, f->seconds);
        if (f->bytes >= 10u << 20) snprintf(b, sizeof b, "%s · %lu Mo", d, (unsigned long)(f->bytes >> 20));
        else {
            unsigned t = (unsigned)((f->bytes * 10ull + (1 << 19)) >> 20);   /* dixièmes de Mo */
            snprintf(b, sizeof b, "%s · %u,%u Mo", d, t / 10, t % 10);
        }
        lv_obj_t *s = ui_label(r, &font_text, C_DIM, b);
        lv_obj_align(s, LV_ALIGN_BOTTOM_LEFT, 0, -S(5));
    }
}

static void list_close(void)
{
    player_close();
    if (v_list) lv_obj_add_flag(v_list, LV_OBJ_FLAG_HIDDEN);
    refresh_main();
}

static void l_back_cb(lv_event_t *e) { list_close(); }

static void list_btn_cb(lv_event_t *e)
{
    if (rec_active()) return;
    lv_obj_remove_flag(v_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(v_list);
    list_fill();
    lv_obj_scroll_to_y(v_list_col, 0, LV_ANIM_OFF);
}

static void build_list(lv_obj_t *scr)
{
    v_list = panel(scr, _("Enregistrements"), l_back_cb, NULL);
    v_list_col = ui_container(v_list);
    lv_obj_set_size(v_list_col, UI_W, UI_H - S(40));
    lv_obj_set_pos(v_list_col, 0, S(40));
    lv_obj_set_style_pad_hor(v_list_col, S(10), 0);
    lv_obj_set_style_pad_bottom(v_list_col, S(12), 0);
    lv_obj_set_style_pad_row(v_list_col, S(6), 0);
    lv_obj_set_flex_flow(v_list_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(v_list_col, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(v_list_col, LV_SCROLLBAR_MODE_ACTIVE);
    ui_bubble(v_list_col);
    lv_obj_add_flag(v_list, LV_OBJ_FLAG_HIDDEN);
}

/* ------------------------------------------------------------------ */
static void tick_cb(lv_timer_t *t)
{
    refresh_main();
    refresh_player();
}

static void create(lv_obj_t *scr)
{
    ui_header(scr, _("Magnétophone"));

    v_state = ui_label(scr, &font_text, C_DIM, "");
    lv_obj_align(v_state, LV_ALIGN_TOP_MID, 0, S(48));
    v_time = ui_label(scr, &font_num, C_DIM, "00:00");
    lv_obj_align(v_time, LV_ALIGN_TOP_MID, 0, S(70));

    v_level = lv_bar_create(scr);
    lv_obj_set_size(v_level, S(200), S(10));
    lv_obj_align(v_level, LV_ALIGN_TOP_MID, 0, S(130));
    lv_bar_set_range(v_level, 0, 100);
    lv_obj_set_style_bg_color(v_level, C_OFF, LV_PART_MAIN);
    lv_obj_set_style_bg_color(v_level, C_OK, LV_PART_INDICATOR);
    ui_bubble(v_level);

    v_info = ui_label(scr, &font_text, C_DIM, "");
    lv_obj_align(v_info, LV_ALIGN_TOP_MID, 0, S(150));

    v_list_btn = round_btn(scr, LV_SYMBOL_LIST, list_btn_cb, NULL, C_OFF, -S(86));
    v_q_btn = round_btn(scr, "", q_cb, NULL, C_OFF, S(86));
    lv_obj_set_style_text_font(lv_obj_get_child(v_q_btn, 0), &font_text, 0);

    v_rec = ui_button(scr, "", C_REC, rec_cb, NULL);
    lv_obj_set_size(v_rec, S(84), S(84));
    lv_obj_set_style_radius(v_rec, LV_RADIUS_CIRCLE, 0);
    lv_obj_align(v_rec, LV_ALIGN_BOTTOM_MID, 0, -S(4));
    lv_obj_set_style_bg_color(v_rec, C_OFF, LV_STATE_DISABLED);
    v_rec_lbl = lv_obj_get_child(v_rec, 0);
    lv_obj_set_style_text_font(v_rec_lbl, &font_title, 0);

    build_list(scr);
    build_player(scr);
    sd_refresh_info();                       /* place libre à jour */
    v_tick = lv_timer_create(tick_cb, 250, NULL);
    m_err_seen = REC_OK;
    {
        rec_status_t st;
        rec_get(&st);
        m_err_seen = st.err;                 /* déjà signalée */
    }
    refresh_main();
}

static void destroy(void)
{
    rec_play_stop();
    if (v_tick) lv_timer_delete(v_tick);
    v_tick = NULL;
    v_state = v_time = v_level = v_info = v_rec = v_rec_lbl = v_list_btn = v_q_btn = NULL;
    v_list = v_list_col = NULL;
    v_play = v_p_title = v_p_pos = v_p_len = v_p_bar = v_p_btn = v_p_del = NULL;
    free(m_files);
    m_files = NULL;
    m_nfiles = 0;
}

static void on_show(void)
{
    if (v_tick) lv_timer_resume(v_tick);
    refresh_main();
    refresh_player();
}

static void on_leave(void)
{
    rec_play_stop();
    if (rec_active()) ui_toast(_("L'enregistrement continue"));
}

static void on_hide(void)
{
    if (v_tick) lv_timer_pause(v_tick);
    if (app_leaving_for_good()) on_leave();
}

/* retour : lecteur → liste → écran principal → sortie (l'enregistrement
 * continue, l'app le montre quand on la rouvre) */
static bool on_back(void)
{
    if (v_play && !lv_obj_has_flag(v_play, LV_OBJ_FLAG_HIDDEN)) { player_close(); return true; }
    if (v_list && !lv_obj_has_flag(v_list, LV_OBJ_FLAG_HIDDEN)) { list_close(); return true; }
    return false;
}

static bool busy(void) { return rec_active() || rec_playing(); }

const app_t app_rec = {
    .name = "Magnéto",
    .icon = SYM_MIC,
    .color = 0xDC2626,
    .create = create,
    .destroy = destroy,
    .on_show = on_show,
    .on_hide = on_hide,
    .on_leave = on_leave,
    .on_back = on_back,
    .busy = busy,
    .background = busy,
};
