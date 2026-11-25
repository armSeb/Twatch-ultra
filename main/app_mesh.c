/*
 * app_mesh.c — « MeshCore » : conversations LoRa, état de la radio.
 *
 *   ‹ MeshCore
 *   [ Contacts · 2 | Canaux | Radio ]      onglets ; non lus comptés
 *   ┌ Alice 👋               14:02 ┐
 *   │ On se retrouve où ?       (2) │      non lus d'abord, puis A → Z
 *   └───────────────────────────────┘
 *
 * Toucher une ligne fait glisser la conversation par-dessus (bulles, les
 * plus anciennes en haut ; « Messages plus anciens » relit l'anneau puis la
 * carte SD). L'ouvrir marque ses messages lus. Retour : ‹, glissé →, bouton.
 *
 * La montre lit sa propre copie des messages (mc_hist.c) : les lire ici ne
 * les retire pas de la file que l'app MeshCore récupère à la connexion.
 *
 * Écrire : la barre « Écrire » en bas d'une conversation ouvre le clavier
 * (compose.c). Sous chaque message envoyé de la montre, son état, comme
 * l'app iPhone : privé « Envoi… 2/3 », « Livré · 1,8 s » ou « Échec » ;
 * canal « Envoyé », « Relayé ×3 » ou « Aucun relais entendu ». Toucher un
 * message en échec le renvoie. Les essais continuent app fermée (tâche mesh).
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "app.h"
#include "ui_kit.h"
#include "ui_geom.h"
#include "i18n.h"
#include "emoji.h"
#include "settings.h"
#include "fonts/fonts.h"
#include "wclock.h"
#include "sdcard.h"
#include "esp_attr.h"
#include "meshcore/mc_port.h"
#include "meshcore/mc_text.h"
#include "sx1262.h"
#include "compose.h"

#define EV_NEW     1
#define ROWS_STEP  40           /* lignes construites d'un coup (« Afficher plus ») */
#define ROWS_MAX   400
#define PAGE       20           /* bulles chargées d'un coup */
#define BUBBLES_MAX 200         /* objets LVGL bornés dans une conversation */
#define SLIDE_MS   200
#define TRACK_N    16           /* messages envoyés suivis dans la conversation ouverte */
#define WRITE_H    S(46)        /* barre « Écrire » */

enum { TAB_CONTACTS, TAB_CHANNELS, TAB_RADIO, TAB_N };

extern const app_t app_mesh;

/* modèle (survit à la vue) */
static int m_tab;
EXT_RAM_BSS_ATTR static mc_conv_t m_conv[ROWS_MAX];
EXT_RAM_BSS_ATTR static mc_msg_t m_buf[PAGE];

/* vue */
static lv_obj_t *v_scr, *v_tabs, *v_list, *v_off, *v_radio, *v_status, *v_diag, *v_sd;
static lv_timer_t *v_tmr;
static uint32_t v_ver = UINT32_MAX;     /* version de l'historique affichée */
static int v_limit = ROWS_STEP, v_nconv;
static char v_tab_txt[TAB_N][32];
static const char *v_tab_map[TAB_N + 1];

/* conversation ouverte */
static lv_obj_t *c_panel, *c_list, *c_more, *c_closing;
static mc_conv_t c_conv;
static bool c_open;
static uint32_t c_oldest, c_newest;
static int c_count;
/* état des messages envoyés depuis la montre : ligne à mettre à jour */
static struct { uint32_t seq; uint8_t flags, hops; int8_t rtt; lv_obj_t *meta; } c_track[TRACK_N];
static int c_ntrack;

/* ------------------------------------------------------------------ outils */
static void fmt_time(uint32_t ts, char *b, size_t n)
{
    time_t t = (time_t)ts, now = time(NULL);
    struct tm tm, tn;
    int32_t off = wclock_tz_offset_min() * 60;
    t += off; now += off;
    gmtime_r(&t, &tm);
    gmtime_r(&now, &tn);
    if (tm.tm_yday == tn.tm_yday && tm.tm_year == tn.tm_year) strftime(b, n, "%H:%M", &tm);
    else if (tm.tm_year == tn.tm_year) strftime(b, n, "%d/%m", &tm);
    else strftime(b, n, "%d/%m/%y", &tm);
}

static void fmt_stamp(uint32_t ts, char *b, size_t n)      /* bulle : jour et heure */
{
    time_t t = (time_t)ts + wclock_tz_offset_min() * 60, now = time(NULL) + wclock_tz_offset_min() * 60;
    struct tm tm, tn;
    gmtime_r(&t, &tm);
    gmtime_r(&now, &tn);
    if (tm.tm_yday == tn.tm_yday && tm.tm_year == tn.tm_year) strftime(b, n, "%H:%M", &tm);
    else strftime(b, n, "%d/%m %H:%M", &tm);
}

/* couleur stable par expéditeur (canaux) */
static lv_color_t name_color(const char *s)
{
    static const uint32_t pal[] = { 0x5EEAD4, 0xF9A8D4, 0xFCD34D, 0x93C5FD, 0xC4B5FD, 0x86EFAC, 0xFDBA74 };
    return lv_color_hex(pal[mc_fnv32(s, strlen(s)) % (sizeof pal / sizeof pal[0])]);
}

static void slide(lv_obj_t *o, int32_t from, int32_t to, lv_anim_completed_cb_t done)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_x);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, SLIDE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    if (done) lv_anim_set_completed_cb(&a, done);
    lv_anim_start(&a);
}

/* ------------------------------------------------------------------ onglet Radio */
static void refresh_radio(void)
{
    if (!v_status) return;
    char b[96], d[96];
    if (!mc_running()) {
        lv_label_set_text(v_status, _("MeshCore coupé"));
        lv_label_set_text(v_diag, "");
    } else {
        /* compteurs radio : de quoi voir d'un coup d'œil si la LoRa émet et reçoit */
        if (!sx_running()) {
            snprintf(d, sizeof d, "%s : %s", _("Radio LoRa arrêtée"), esp_err_to_name(sx_last_error()));
        } else {
            sx_stats_t x;
            sx_get_stats(&x);
            int16_t nf = sx_noise_floor();
            int k = snprintf(d, sizeof d, "TX %lu", (unsigned long)x.tx_ok);
            if (x.tx_fail) k += snprintf(d + k, sizeof d - k, " (%lu %s)", (unsigned long)x.tx_fail, _("ratés"));
            k += snprintf(d + k, sizeof d - k, " · RX %lu", (unsigned long)x.rx_ok);
            if (x.rx_crc) k += snprintf(d + k, sizeof d - k, " (%lu %s)", (unsigned long)x.rx_crc, _("abîmés"));
            if (nf != INT16_MIN) snprintf(d + k, sizeof d - k, " · %d dBm", nf);
        }
        lv_label_set_text(v_diag, d);
        mc_status_t s;
        mc_get_status(&s);
        char f[16];
        snprintf(f, sizeof f, "%.3f", s.freq_mhz);
        if (i18n_get() == LANG_FR) for (char *p = f; *p; p++) if (*p == '.') *p = ',';
        int k = snprintf(b, sizeof b, "%s\n%s MHz · SF%u · %d dBm\n", s.name, f, s.sf, s.tx_dbm);
        if (s.app_connected) snprintf(b + k, sizeof b - k, "%s", _("App MeshCore connectée"));
        else if (s.app_queue) snprintf(b + k, sizeof b - k, _("%d message(s) pour l'app"), s.app_queue);
        else snprintf(b + k, sizeof b - k, "%s", _("App MeshCore à jour"));
        emoji_label_set_text(v_status, b);
    }
    sd_info_t si;
    sd_get_info(&si);
    if (si.state == SD_MOUNTED) snprintf(b, sizeof b, "%s", _("Carte SD : historique archivé"));
    else snprintf(b, sizeof b, "%s", _("Sans carte SD : derniers messages seulement"));
    lv_label_set_text(v_sd, b);
}

/* ------------------------------------------------------------------ conversation */
/* ligne sous une bulle : heure, puis état de l'envoi (messages de la montre) */
static void meta_text(const mc_msg_t *m, lv_obj_t *mt)
{
    char meta[64], when[16];
    bool out = m->flags & MC_F_OUT;
    fmt_stamp(m->ts, when, sizeof when);
    lv_color_t col = out ? lv_color_hex(0x93C5FD) : C_DIM;
    if (out && (m->flags & MC_F_WATCH)) {
        bool chan = m->kind == MC_CONV_CHANNEL;
        if (m->flags & MC_F_ACK) {
            col = C_OK;
            if (chan) snprintf(meta, sizeof meta, _("%s · ✓ Relayé ×%u"), when, m->hops);
            else if (m->snr_q4 > 0) {
                char rt[12];
                snprintf(rt, sizeof rt, "%d.%d", m->snr_q4 / 10, m->snr_q4 % 10);
                if (i18n_get() == LANG_FR) for (char *q = rt; *q; q++) if (*q == '.') *q = ',';
                snprintf(meta, sizeof meta, _("%s · ✓ Livré · %s s"), when, rt);
            } else snprintf(meta, sizeof meta, _("%s · ✓ Livré"), when);
        } else if (m->flags & MC_F_FAIL) {
            col = chan ? C_WARN : C_BAD;
            snprintf(meta, sizeof meta, "%s · %s", when,
                     chan ? _("Aucun relais entendu · toucher pour renvoyer") : _("✗ Échec · toucher pour renvoyer"));
        } else if (m->flags & MC_F_PEND) {
            uint8_t tries = settings_get()->mesh_tries;
            if (chan) snprintf(meta, sizeof meta, _("%s · Envoyé, en attente de relais"), when);
            else if (tries > 1) snprintf(meta, sizeof meta, _("%s · Envoi… %u/%u"), when, m->hops + 1, tries);
            else snprintf(meta, sizeof meta, _("%s · Envoi…"), when);
        } else {
            snprintf(meta, sizeof meta, "%s", when);
        }
    } else if (out) snprintf(meta, sizeof meta, "%s", when);
    else if (m->hops == 0xFF || m->hops == 0) snprintf(meta, sizeof meta, "%s · %s", when, _("direct"));
    else snprintf(meta, sizeof meta, _("%s · %u saut(s)"), when, m->hops);
    lv_obj_set_style_text_color(mt, col, 0);
    lv_label_set_text(mt, meta);
}

static void track(const mc_msg_t *m, lv_obj_t *mt)
{
    if (!(m->flags & MC_F_WATCH)) return;
    int i = 0;
    while (i < c_ntrack && c_track[i].seq != m->seq) i++;
    if (i == c_ntrack) {
        if (c_ntrack < TRACK_N) c_ntrack++;
        else {                                   /* le plus ancien suivi cède sa place */
            int o = 0;
            for (int k = 1; k < TRACK_N; k++) if (c_track[k].seq < c_track[o].seq) o = k;
            if (c_track[o].seq > m->seq) return;
            i = o;
        }
    }
    c_track[i].seq = m->seq;
    c_track[i].flags = m->flags;
    c_track[i].hops = m->hops;
    c_track[i].rtt = m->snr_q4;
    c_track[i].meta = mt;
}

/* états changés depuis l'affichage (accusé, relais, nouvel essai) */
static void track_refresh(void)
{
    static mc_msg_t m;
    for (int i = 0; i < c_ntrack; i++) {
        if (!c_track[i].meta || !mc_msg_get(c_track[i].seq, &m)) continue;
        if (m.flags == c_track[i].flags && m.hops == c_track[i].hops && m.snr_q4 == c_track[i].rtt) continue;
        c_track[i].flags = m.flags;
        c_track[i].hops = m.hops;
        c_track[i].rtt = m.snr_q4;
        meta_text(&m, c_track[i].meta);
    }
}

static void bubble_cb(lv_event_t *e)
{
    uint32_t seq = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    static mc_msg_t m;
    if (!mc_msg_get(seq, &m) || !(m.flags & MC_F_FAIL) || !(m.flags & MC_F_WATCH)) return;
    if (mc_resend(seq)) ui_toast(_("Nouvel essai"));
    else ui_toast(_("Radio LoRa indisponible"));
}

static void add_bubble(const mc_msg_t *m, bool at_top)
{
    bool out = m->flags & MC_F_OUT;
    lv_obj_t *row = ui_container(c_list);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, out ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    ui_bubble(row);
    if (at_top) lv_obj_move_to_index(row, 1);          /* sous le bouton « plus anciens » */

    lv_obj_t *c = ui_container(row);
    lv_obj_set_size(c, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(c, UI_W * 82 / 100, 0);
    lv_obj_set_style_bg_color(c, out ? lv_color_hex(0x1E3A8A) : C_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, S(10), 0);
    lv_obj_set_style_pad_all(c, S(7), 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, S(2), 0);
    ui_bubble(c);
    if (m->flags & MC_F_WATCH) {                /* en échec : toucher pour renvoyer */
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(c, lv_color_hex(0x2B4BA8), LV_STATE_PRESSED);
        lv_obj_add_event_cb(c, bubble_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)m->seq);
    }

    const char *text = m->text;
    char who[MC_NAME_MAX];
    if (m->kind == MC_CONV_CHANNEL && !out) {
        text = mc_split_sender(m->text, who, sizeof who);
        if (who[0]) {
            lv_obj_t *h = ui_label(c, &font_small, name_color(who), "");
            lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);   /* DOT + largeur au contenu : « … » seul */
            lv_obj_set_style_max_width(h, UI_W * 82 / 100 - S(14), 0);
            emoji_label_set_text(h, who);
        }
    }
    lv_obj_t *t = ui_label(c, &font_text, C_FG, "");
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(t, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(t, UI_W * 82 / 100 - S(14), 0);
    emoji_label_set_text(t, text);

    lv_obj_t *mt = ui_label(c, &font_small, C_DIM, "");
    lv_label_set_long_mode(mt, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_max_width(mt, UI_W * 82 / 100 - S(14), 0);
    meta_text(m, mt);
    if (out) lv_obj_set_style_text_align(mt, LV_TEXT_ALIGN_RIGHT, 0);
    track(m, mt);
    c_count++;
}

static void unread_divider(bool at_top)
{
    lv_obj_t *l = ui_label(c_list, &font_small, C_ACC, _("Non lus"));
    lv_obj_set_width(l, lv_pct(100));
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    if (at_top) lv_obj_move_to_index(l, 1);
}

static void more_state(int got)
{
    /* une page incomplète : plus rien dans l'anneau ni sur la carte. Le
     * bouton est caché, pas détruit (on peut être dans son propre clic) */
    if (got < PAGE || c_count >= BUBBLES_MAX) lv_obj_add_flag(c_more, LV_OBJ_FLAG_HIDDEN);
}

static void more_cb(lv_event_t *e)
{
    if (!c_open || lv_obj_has_flag(c_more, LV_OBJ_FLAG_HIDDEN)) return;
    int32_t bottom = lv_obj_get_scroll_bottom(c_list);
    int n = mc_conv_messages(c_conv.kind, c_conv.peer, c_oldest, m_buf, PAGE);
    /* du plus récent au plus ancien : chacun passe en tête, juste sous le bouton */
    for (int i = 0; i < n; i++) {
        add_bubble(&m_buf[i], true);
        if (m_buf[i].seq < c_oldest) c_oldest = m_buf[i].seq;
    }
    more_state(n);
    lv_obj_update_layout(c_list);
    lv_obj_scroll_to_y(c_list, lv_obj_get_scroll_y(c_list) + lv_obj_get_scroll_bottom(c_list) - bottom, LV_ANIM_OFF);
}

/* messages arrivés pendant que la conversation est ouverte */
static void conv_append_new(void)
{
    int n = mc_conv_messages(c_conv.kind, c_conv.peer, UINT32_MAX, m_buf, PAGE);
    bool at_end = lv_obj_get_scroll_bottom(c_list) < S(40);
    int k = 0;
    while (k < n && m_buf[k].seq > c_newest) k++;
    for (int i = k - 1; i >= 0; i--) {
        add_bubble(&m_buf[i], false);
        if (m_buf[i].flags & MC_F_OUT) at_end = true;      /* son propre message : on le montre */
    }
    track_refresh();
    if (k) {
        c_newest = m_buf[0].seq;
        mc_conv_mark_read(c_conv.kind, c_conv.peer);
        if (at_end) {
            lv_obj_update_layout(c_list);
            lv_obj_scroll_to_y(c_list, LV_COORD_MAX, LV_ANIM_ON);
        }
    }
}

static void conv_deleted_cb(lv_anim_t *a)
{
    lv_obj_t *p = lv_anim_get_user_data(a);
    if (p == c_closing) c_closing = NULL;
    if (p) lv_obj_delete(p);                /* comme lv_obj_delete_anim_completed_cb */
}

static void rebuild(bool force);

static void close_conv(bool anim)
{
    if (!c_panel) return;
    lv_obj_t *p = c_panel;
    c_panel = c_list = c_more = NULL;
    c_open = false;
    c_ntrack = 0;
    compose_close(false);
    rebuild(true);                      /* liste : compteurs de non lus à jour */
    if (!anim) { lv_obj_delete(p); return; }
    if (c_closing) { lv_anim_delete(c_closing, NULL); lv_obj_delete(c_closing); }
    c_closing = p;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, p);
    lv_anim_set_user_data(&a, p);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_x);
    lv_anim_set_values(&a, 0, UI_W + UI_PAD_X);
    lv_anim_set_duration(&a, SLIDE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_completed_cb(&a, conv_deleted_cb);
    lv_anim_start(&a);
}

static void conv_back_cb(lv_event_t *e) { close_conv(true); }

static bool send_cb(const char *text, void *ud)
{
    if (!c_open) return false;
    if (!mc_running()) { ui_toast(_("MeshCore coupé")); return false; }
    if (!mc_send_text(c_conv.kind, c_conv.peer, text)) { ui_toast(_("Envoi impossible pour l'instant")); return false; }
    return true;
}

static void write_cb(lv_event_t *e)
{
    if (!c_open) return;
    if (!mc_running()) { ui_toast(_("MeshCore coupé")); return; }
    char title[MC_NAME_MAX + 8];
    snprintf(title, sizeof title, "%s%s", c_conv.kind == MC_CONV_CHANNEL && c_conv.name[0] != '#' ? "#" : "", c_conv.name);
    uint8_t key[MC_PEER_LEN + 1];
    key[0] = c_conv.kind;
    memcpy(key + 1, c_conv.peer, MC_PEER_LEN);
    compose_open(v_scr, title, mc_fnv32(key, sizeof key), mc_text_max(c_conv.kind), send_cb, NULL);
}

static void open_conv(const mc_conv_t *cv)
{
    if (c_panel) close_conv(false);
    c_conv = *cv;
    c_open = true;
    c_count = 0;
    c_ntrack = 0;

    c_panel = ui_container(v_scr);
    lv_obj_add_flag(c_panel, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(c_panel, UI_W, UI_H);
    lv_obj_set_style_bg_color(c_panel, C_BG, 0);
    lv_obj_set_style_bg_opa(c_panel, LV_OPA_COVER, 0);
    lv_obj_add_flag(c_panel, LV_OBJ_FLAG_CLICKABLE);          /* bloque les touches dessous */
    ui_bubble(c_panel);

    lv_obj_t *hdr = lv_button_create(c_panel);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, UI_W, S(40));
    lv_obj_set_style_bg_color(hdr, C_CARD, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_event_cb(hdr, conv_back_cb, LV_EVENT_CLICKED, NULL);
    ui_bubble(hdr);
    lv_obj_t *ar = ui_label(hdr, &font_title, C_ACC, LV_SYMBOL_LEFT);
    lv_obj_align(ar, LV_ALIGN_LEFT_MID, S(12), 0);
    char title[MC_NAME_MAX + 2];
    snprintf(title, sizeof title, "%s%s", cv->kind == MC_CONV_CHANNEL && cv->name[0] != '#' ? "#" : "", cv->name);
    lv_obj_t *t = ui_label(hdr, &font_title, C_FG, "");
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_width(t, UI_W - S(44));
    lv_obj_align(t, LV_ALIGN_LEFT_MID, S(36), 0);
    emoji_label_set_text(t, title);

    c_list = ui_container(c_panel);
    lv_obj_set_size(c_list, UI_W, UI_H - S(42) - WRITE_H - S(18));
    lv_obj_set_pos(c_list, 0, S(42));
    lv_obj_set_style_pad_hor(c_list, S(4), 0);
    lv_obj_set_style_pad_row(c_list, S(6), 0);
    lv_obj_set_style_pad_bottom(c_list, S(10), 0);
    lv_obj_set_flex_flow(c_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(c_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(c_list, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_add_flag(c_list, LV_OBJ_FLAG_SCROLLABLE);
    ui_bubble(c_list);

    /* barre « Écrire » : en bas, rétrécie pour les coins arrondis */
    lv_obj_t *wb = ui_button(c_panel, "", C_ACC, write_cb, NULL);
    lv_obj_set_size(wb, UI_W - S(60), WRITE_H);
    lv_obj_set_style_radius(wb, WRITE_H / 2, 0);
    lv_obj_align(wb, LV_ALIGN_BOTTOM_MID, 0, -S(4));
    /* le tactile prenait mal le bas de la barre : zone sensible élargie
     * tout autour (la liste s'arrête au-dessus) */
    lv_obj_set_ext_click_area(wb, S(14));
    char wt[48];
    snprintf(wt, sizeof wt, "%s  %s", SYM_PEN, _("Écrire"));
    lv_label_set_text(lv_obj_get_child(wb, 0), wt);
    if (!mc_running()) lv_obj_add_state(wb, LV_STATE_DISABLED);

    c_more = ui_button(c_list, _("Messages plus anciens"), C_CARD, more_cb, NULL);
    lv_obj_set_size(c_more, lv_pct(100), S(36));

    int n = mc_conv_messages(cv->kind, cv->peer, UINT32_MAX, m_buf, PAGE);
    c_oldest = UINT32_MAX;
    c_newest = 0;
    int first_unread = -1;              /* le plus ancien non lu (indices décroissants) */
    for (int i = 0; i < n; i++) if (!(m_buf[i].flags & (MC_F_READ | MC_F_OUT))) first_unread = i;
    for (int i = n - 1; i >= 0; i--) {
        if (i == first_unread) unread_divider(false);
        add_bubble(&m_buf[i], false);
        if (m_buf[i].seq < c_oldest) c_oldest = m_buf[i].seq;
        if (m_buf[i].seq > c_newest) c_newest = m_buf[i].seq;
    }
    if (!n) {
        lv_obj_t *e = ui_label(c_list, &font_small, C_DIM, _("Aucun message sur la montre. L'app MeshCore garde tout l'historique."));
        lv_label_set_long_mode(e, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(e, lv_pct(100));
    }
    more_state(n);
    mc_conv_mark_read(cv->kind, cv->peer);

    lv_obj_update_layout(c_panel);
    lv_obj_scroll_to_y(c_list, LV_COORD_MAX, LV_ANIM_OFF);
    lv_obj_set_x(c_panel, UI_W + UI_PAD_X);
    slide(c_panel, UI_W + UI_PAD_X, 0, NULL);
}

/* ------------------------------------------------------------------ listes */
static void row_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i >= 0 && i < v_nconv && i < ROWS_MAX) open_conv(&m_conv[i]);
}

static void add_row(int i)
{
    const mc_conv_t *cv = &m_conv[i];
    lv_obj_t *r = lv_button_create(v_list);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, lv_pct(100), S(48));
    lv_obj_set_style_bg_color(r, C_CARD, 0);
    lv_obj_set_style_bg_color(r, C_OFF, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(r, S(10), 0);
    lv_obj_set_style_pad_hor(r, S(8), 0);
    lv_obj_set_style_pad_ver(r, S(4), 0);
    lv_obj_add_event_cb(r, row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    ui_bubble(r);

    int right = cv->unread ? S(44) : S(40);
    char name[MC_NAME_MAX + 2];
    snprintf(name, sizeof name, "%s%s", cv->kind == MC_CONV_CHANNEL && cv->name[0] != '#' ? "#" : "", cv->name);
    lv_obj_t *n = ui_label(r, &font_text, C_FG, "");
    lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
    lv_obj_set_size(n, UI_W - S(16) - right, lv_font_get_line_height(&font_text));   /* une ligne */
    lv_obj_align(n, LV_ALIGN_TOP_LEFT, 0, 0);
    emoji_label_set_text(n, name);

    char prev[80];
    if (!cv->last_ts) snprintf(prev, sizeof prev, "%s", "");
    else snprintf(prev, sizeof prev, "%s%s", cv->last_out ? _("Moi : ") : "", cv->last);
    for (char *p = prev; *p; p++) if (*p == '\n') *p = ' ';
    lv_obj_t *pv = ui_label(r, &font_small, cv->unread ? C_FG : C_DIM, "");
    lv_label_set_long_mode(pv, LV_LABEL_LONG_DOT);
    lv_obj_set_size(pv, UI_W - S(16) - right, lv_font_get_line_height(&font_small));
    lv_obj_align(pv, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    emoji_label_set_text(pv, prev);

    if (cv->last_ts) {
        char w[16];
        fmt_time(cv->last_ts, w, sizeof w);
        lv_obj_t *tl = ui_label(r, &font_small, cv->unread ? C_ACC : C_DIM, w);
        lv_obj_align(tl, LV_ALIGN_TOP_RIGHT, 0, S(2));
    }
    if (cv->unread) {
        char u[8];
        snprintf(u, sizeof u, "%u", cv->unread > 99 ? 99 : cv->unread);
        lv_obj_t *b = ui_label(r, &font_small, lv_color_white(), u);
        lv_obj_set_style_bg_color(b, C_ACC, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_pad_hor(b, S(6), 0);
        lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_min_width(b, S(20), 0);
        lv_obj_align(b, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    }
}

static void more_rows_cb(lv_event_t *e);

static void update_tabs(void)
{
    static const char *const base[TAB_N] = { N_("Contacts"), N_("Canaux"), N_("Radio") };
    bool same = v_tab_map[0] == v_tab_txt[0];
    for (int t = 0; t < TAB_N; t++) {
        char b[sizeof v_tab_txt[0]];
        int u = t == TAB_CONTACTS ? mc_unread_total(MC_CONV_CONTACT) :
                t == TAB_CHANNELS ? mc_unread_total(MC_CONV_CHANNEL) : 0;
        if (u) snprintf(b, sizeof b, "%s · %d", _(base[t]), u > 99 ? 99 : u);
        else snprintf(b, sizeof b, "%s", _(base[t]));
        if (strcmp(b, v_tab_txt[t])) { same = false; memcpy(v_tab_txt[t], b, sizeof b); }
        v_tab_map[t] = v_tab_txt[t];
    }
    v_tab_map[TAB_N] = "";
    if (same) return;
    lv_buttonmatrix_set_map(v_tabs, v_tab_map);          /* recrée les boutons : état à remettre */
    lv_buttonmatrix_set_button_ctrl_all(v_tabs, LV_BUTTONMATRIX_CTRL_CHECKABLE);
    lv_buttonmatrix_set_button_ctrl(v_tabs, m_tab, LV_BUTTONMATRIX_CTRL_CHECKED);
}

static void rebuild(bool force)
{
    if (!v_list) return;
    uint32_t ver = mc_history_last_seq();
    if (!force && ver == v_ver) return;
    v_ver = ver;
    update_tabs();

    bool on = mc_running();
    bool radio = m_tab == TAB_RADIO;
    if (radio) {
        lv_obj_add_flag(v_list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(v_off, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(v_radio, LV_OBJ_FLAG_HIDDEN);
        refresh_radio();
        return;
    }
    lv_obj_add_flag(v_radio, LV_OBJ_FLAG_HIDDEN);
    if (!on) {
        lv_obj_add_flag(v_list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(v_off, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(v_off, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(v_list, LV_OBJ_FLAG_HIDDEN);

    int32_t sy = lv_obj_get_scroll_y(v_list);
    lv_obj_clean(v_list);
    uint8_t kind = m_tab == TAB_CHANNELS ? MC_CONV_CHANNEL : MC_CONV_CONTACT;
    int total = mc_conv_list(kind, m_conv, v_limit < ROWS_MAX ? v_limit : ROWS_MAX);
    v_nconv = total < v_limit ? total : v_limit;
    for (int i = 0; i < v_nconv; i++) add_row(i);
    if (total > v_nconv && v_limit < ROWS_MAX) {
        char b[48];
        snprintf(b, sizeof b, _("Afficher plus (%d)"), total - v_nconv);
        lv_obj_t *m = ui_button(v_list, b, C_CARD, more_rows_cb, NULL);
        lv_obj_set_size(m, lv_pct(100), S(36));
    }
    if (!total) {
        lv_obj_t *e = ui_label(v_list, &font_small, C_DIM, kind == MC_CONV_CHANNEL ?
                               _("Aucun canal. Ajoute-les depuis l'app MeshCore.") :
                               _("Aucun contact pour l'instant. Ils apparaissent quand les nœuds s'annoncent."));
        lv_label_set_long_mode(e, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(e, lv_pct(100));
    }
    lv_obj_update_layout(v_list);
    lv_obj_scroll_to_y(v_list, sy, LV_ANIM_OFF);
}

/* reconstruire la liste détruit le bouton qu'on vient de toucher : après
 * l'événement (la vue peut avoir été détruite entre-temps) */
static void more_rows_async(void *arg)
{
    if (!v_list) return;
    v_limit += ROWS_STEP;
    rebuild(true);
}

static void more_rows_cb(lv_event_t *e) { lv_async_call(more_rows_async, NULL); }

static void tab_switch_async(void *arg)
{
    if (!v_list) return;
    int dir = (int)(intptr_t)arg;
    v_limit = ROWS_STEP;
    lv_obj_scroll_to_y(v_list, 0, LV_ANIM_OFF);
    rebuild(true);
    if (m_tab == TAB_RADIO || mc_running()) {          /* v_off est centré : pas de glissé */
        lv_obj_t *o = m_tab == TAB_RADIO ? v_radio : v_list;
        slide(o, dir * S(60), 0, NULL);
    }
    if (m_tab != TAB_RADIO) mc_conv_refresh();
}

static void tabs_cb(lv_event_t *e)
{
    uint32_t t = lv_buttonmatrix_get_selected_button(v_tabs);
    if (t >= TAB_N || (int)t == m_tab) return;
    int dir = (int)t > m_tab ? 1 : -1;
    m_tab = (int)t;
    lv_async_call(tab_switch_async, (void *)(intptr_t)dir);   /* la barre se redessine hors de son clic */
}

static void enable_cb(lv_event_t *e)
{
    settings_get()->meshcore = true;
    settings_save();
    if (mc_start() != ESP_OK) ui_toast(_("Radio LoRa indisponible"));
    mc_conv_refresh();
    rebuild(true);
}

static void advert_cb(lv_event_t *e)
{
    bool flood = lv_event_get_user_data(e) != NULL;
    if (!mc_running()) { ui_toast(_("MeshCore coupé")); return; }
    if (mc_send_advert(flood)) ui_toast(flood ? _("Annonce flood envoyée") : _("Annonce locale envoyée"));
    else ui_toast(_("Envoi impossible pour l'instant"));
}

static void tick_cb(lv_timer_t *t)
{
    if (m_tab == TAB_RADIO && !c_open) refresh_radio();
}

/* appelé par la tâche mesh : on ne touche pas à LVGL ici */
static void on_msg(const mc_msg_t *m) { app_post_event(&app_mesh, EV_NEW); }

/* ------------------------------------------------------------------ cycle de vie */
static void create(lv_obj_t *scr)
{
    v_scr = scr;
    mc_set_msg_cb(on_msg);
    mc_conv_refresh();
    int y = ui_header(scr, _("MeshCore"));

    for (int t = 0; t <= TAB_N; t++) v_tab_map[t] = "";
    v_tab_map[0] = " ";
    v_tabs = ui_segmented(scr, v_tab_map, tabs_cb);
    lv_obj_set_size(v_tabs, UI_W - S(8), S(34));
    lv_obj_align(v_tabs, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_text_font(v_tabs, &font_small, LV_PART_ITEMS);
    y += S(40);

    v_list = ui_container(scr);
    lv_obj_set_size(v_list, UI_W, UI_H - y);
    lv_obj_set_pos(v_list, 0, y);
    lv_obj_set_style_pad_hor(v_list, S(4), 0);
    lv_obj_set_style_pad_row(v_list, S(5), 0);
    lv_obj_set_style_pad_bottom(v_list, S(10), 0);
    lv_obj_set_flex_flow(v_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(v_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(v_list, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_add_flag(v_list, LV_OBJ_FLAG_SCROLLABLE);
    ui_bubble(v_list);

    /* onglet Radio */
    v_radio = ui_container(scr);
    lv_obj_set_size(v_radio, UI_W, UI_H - y);
    lv_obj_set_pos(v_radio, 0, y);
    lv_obj_set_style_pad_hor(v_radio, S(8), 0);
    lv_obj_set_style_pad_row(v_radio, S(8), 0);
    lv_obj_set_flex_flow(v_radio, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_bottom(v_radio, S(20), 0);
    lv_obj_set_scroll_dir(v_radio, LV_DIR_VER);
    lv_obj_add_flag(v_radio, LV_OBJ_FLAG_SCROLLABLE);
    ui_bubble(v_radio);
    v_status = ui_label(v_radio, &font_text, C_FG, "");
    lv_label_set_long_mode(v_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(v_status, lv_pct(100));
    v_diag = ui_label(v_radio, &font_small, C_DIM, "");
    lv_label_set_long_mode(v_diag, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(v_diag, lv_pct(100));
    v_sd = ui_label(v_radio, &font_small, C_DIM, "");
    lv_label_set_long_mode(v_sd, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(v_sd, lv_pct(100));
    /* annonces : la montre se fait connaître (flood : relayée par tout le réseau) */
    lv_obj_t *af = ui_button(v_radio, "", C_ACC, advert_cb, (void *)1);
    lv_obj_set_size(af, lv_pct(100), S(44));
    char ab[64];
    snprintf(ab, sizeof ab, "%s  %s", SYM_TOWER, _("Annonce flood"));
    lv_label_set_text(lv_obj_get_child(af, 0), ab);
    lv_obj_t *az = ui_button(v_radio, _("Annonce locale (0 saut)"), C_CARD, advert_cb, (void *)0);
    lv_obj_set_size(az, lv_pct(100), S(40));
    lv_obj_t *hint = ui_label(v_radio, &font_small, C_DIM,
                              _("Fréquence, débit et puissance se règlent depuis l'app MeshCore de l'iPhone."));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, lv_pct(100));

    /* MeshCore coupé */
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

    v_limit = ROWS_STEP;
    rebuild(true);
    v_tmr = lv_timer_create(tick_cb, 2000, NULL);
}

static void destroy(void)
{
    if (v_tmr) { lv_timer_delete(v_tmr); v_tmr = NULL; }
    if (c_panel) lv_anim_delete(c_panel, NULL);       /* l'écran et ses enfants partent ensuite */
    if (c_closing) lv_anim_delete(c_closing, NULL);
    mc_set_msg_cb(NULL);
    compose_forget();
    c_ntrack = 0;
    v_scr = v_tabs = v_list = v_off = v_radio = v_status = v_diag = v_sd = NULL;
    c_panel = c_list = c_more = c_closing = NULL;
    c_open = false;
    v_ver = UINT32_MAX;
}

static void on_show(void)
{
    if (v_tmr) lv_timer_resume(v_tmr);
    mc_conv_refresh();
    if (c_open) conv_append_new();
    rebuild(false);
}

static void on_hide(void) { if (v_tmr) lv_timer_pause(v_tmr); }

static bool on_event(int code)
{
    if (code != EV_NEW || !v_list) return false;
    if (c_open) conv_append_new();
    else rebuild(false);
    return false;           /* un message n'ouvre pas l'app (bannière : plus tard) */
}

static bool on_back(void)
{
    if (compose_on_back()) return true;
    if (!c_open) return false;
    close_conv(true);
    return true;
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
    .on_back = on_back,
};
