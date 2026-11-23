/*
 * app_mesh.c — « MeshCore » : messages LoRa reçus, état de la radio.
 *
 *   ‹ MeshCore
 *   Nœud · 869,618 MHz · SF8         état (app iPhone connectée, en attente)
 *   ┌ #Public · Alice   14:02 ┐
 *   │ Salut 👋                 │     les plus récents en haut, émojis en couleur
 *   └──────────────────────────┘
 *
 * La montre lit sa propre copie des messages (mc_history_*) : les lire ici
 * ne les retire pas de la file que l'app MeshCore récupère à la connexion.
 * Réponse et UI complète : plus tard (l'app iPhone sert pour écrire).
 */
#include <stdio.h>
#include <time.h>
#include "app.h"
#include "ui_kit.h"
#include "ui_geom.h"
#include "i18n.h"
#include "emoji.h"
#include "settings.h"
#include "fonts/fonts.h"
#include "wclock.h"
#include "meshcore/mc_port.h"

#define SHOW_MAX 30             /* objets LVGL bornés : 30 bulles au plus */
#define EV_NEW   1

extern const app_t app_mesh;

static lv_obj_t *v_status, *v_list, *v_off;
static uint32_t v_seq;          /* dernier message affiché */
static lv_timer_t *v_tmr;

static void fmt_time(uint32_t ts, char *b, size_t n)
{
    time_t t = (time_t)ts, now = time(NULL);
    struct tm tm, tn;
    int32_t off = wclock_tz_offset_min() * 60;
    t += off; now += off;
    gmtime_r(&t, &tm);
    gmtime_r(&now, &tn);
    if (tm.tm_yday == tn.tm_yday && tm.tm_year == tn.tm_year) strftime(b, n, "%H:%M", &tm);
    else strftime(b, n, "%d/%m %H:%M", &tm);
}

static void refresh_status(void)
{
    if (!v_status) return;
    char b[96];
    if (!mc_running()) {
        lv_label_set_text(v_status, _("MeshCore coupé"));
        return;
    }
    mc_status_t s;
    mc_get_status(&s);
    char f[16];
    snprintf(f, sizeof f, "%.3f", s.freq_mhz);
    if (i18n_get() == LANG_FR) for (char *p = f; *p; p++) if (*p == '.') *p = ',';
    if (s.app_connected) snprintf(b, sizeof b, "%s · %s MHz · SF%u · %s", s.name, f, s.sf, _("app connectée"));
    else if (s.app_queue) snprintf(b, sizeof b, "%s · %s MHz · %d %s", s.name, f, s.app_queue, _("pour l'app"));
    else snprintf(b, sizeof b, "%s · %s MHz · SF%u", s.name, f, s.sf);
    emoji_label_set_text(v_status, b);
}

static void add_bubble(const mc_msg_t *m)
{
    lv_obj_t *c = ui_container(v_list);
    lv_obj_set_size(c, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(c, C_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, S(10), 0);
    lv_obj_set_style_pad_all(c, S(8), 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, S(2), 0);
    ui_bubble(c);

    char head[MC_NAME_MAX + 24], when[16];
    fmt_time(m->ts, when, sizeof when);
    snprintf(head, sizeof head, "%s%s · %s", m->channel ? "#" : "", m->from, when);
    lv_obj_t *h = ui_label(c, &font_small, m->channel ? lv_color_hex(0x5EEAD4) : C_ACC, "");
    lv_label_set_long_mode(h, LV_LABEL_LONG_DOT);
    lv_obj_set_width(h, lv_pct(100));
    emoji_label_set_text(h, head);

    lv_obj_t *t = ui_label(c, &font_text, C_FG, "");
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(t, lv_pct(100));
    emoji_label_set_text(t, m->text);
}

static void rebuild(void)
{
    if (!v_list) return;
    refresh_status();
    bool on = mc_running();
    if (on) lv_obj_add_flag(v_off, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(v_off, LV_OBJ_FLAG_HIDDEN);
    uint32_t last = mc_history_last_seq();
    if (last == v_seq && lv_obj_get_child_count(v_list)) return;
    v_seq = last;
    lv_obj_clean(v_list);
    int n = mc_history_count();
    if (n > SHOW_MAX) n = SHOW_MAX;
    static mc_msg_t m;
    for (int i = 0; i < n; i++)
        if (mc_history_get(i, &m)) add_bubble(&m);
    if (!n && on) {
        lv_obj_t *e = ui_label(v_list, &font_small, C_DIM, _("Aucun message reçu pour l'instant."));
        lv_label_set_long_mode(e, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(e, lv_pct(100));
    }
    lv_obj_scroll_to_y(v_list, 0, LV_ANIM_OFF);
}

static void enable_cb(lv_event_t *e)
{
    settings_get()->meshcore = true;
    settings_save();
    if (mc_start() != ESP_OK) ui_toast(_("Radio LoRa indisponible"));
    v_seq = UINT32_MAX;
    rebuild();
}

static void tick_cb(lv_timer_t *t) { refresh_status(); }

/* appelé par la tâche mesh : on ne touche pas à LVGL ici */
static void on_msg(const mc_msg_t *m) { app_post_event(&app_mesh, EV_NEW); }

static void create(lv_obj_t *scr)
{
    mc_set_msg_cb(on_msg);
    int y = ui_header(scr, _("MeshCore"));
    v_status = ui_label(scr, &font_small, C_DIM, "");
    lv_label_set_long_mode(v_status, LV_LABEL_LONG_DOT);
    lv_obj_set_width(v_status, UI_W - S(16));
    lv_obj_align(v_status, LV_ALIGN_TOP_MID, 0, y);
    y += S(24);

    v_list = ui_container(scr);
    lv_obj_set_size(v_list, UI_W, UI_H - y);
    lv_obj_set_pos(v_list, 0, y);
    lv_obj_set_style_pad_hor(v_list, S(6), 0);
    lv_obj_set_style_pad_row(v_list, S(6), 0);
    lv_obj_set_flex_flow(v_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(v_list, LV_DIR_VER);
    lv_obj_add_flag(v_list, LV_OBJ_FLAG_SCROLLABLE);

    v_off = ui_container(scr);
    lv_obj_set_size(v_off, S(220), S(120));
    lv_obj_align(v_off, LV_ALIGN_CENTER, 0, S(20));
    lv_obj_t *m = ui_label(v_off, &font_small, C_DIM,
                           _("Radio LoRa coupée. Allumée, elle reçoit les messages MeshCore et les garde pour l'app iPhone."));
    lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(m, S(210));
    lv_obj_set_style_text_align(m, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(m, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_t *bt = ui_button(v_off, _("Activer MeshCore"), C_ACC, enable_cb, NULL);
    lv_obj_set_size(bt, S(200), S(44));
    lv_obj_align(bt, LV_ALIGN_BOTTOM_MID, 0, 0);

    v_seq = UINT32_MAX;
    rebuild();
    v_tmr = lv_timer_create(tick_cb, 5000, NULL);
}

static void destroy(void)
{
    if (v_tmr) { lv_timer_delete(v_tmr); v_tmr = NULL; }
    v_status = v_list = v_off = NULL;
}

static void on_show(void) { if (v_tmr) lv_timer_resume(v_tmr); rebuild(); }
static void on_hide(void) { if (v_tmr) lv_timer_pause(v_tmr); }

static bool on_event(int code)
{
    if (code == EV_NEW) rebuild();
    return false;           /* un message n'ouvre pas l'app (bannière : plus tard) */
}

const app_t app_mesh = {
    .name = N_("MeshCore"),
    .icon = SYM_COMMENTS,
    .color = 0x14B8A6,
    .create = create,
    .destroy = destroy,
    .on_show = on_show,
    .on_hide = on_hide,
    .on_event = on_event,
};
