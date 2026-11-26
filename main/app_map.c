/*
 * app_map.c — Cartes : tuiles OpenStreetMap de la carte SD, position du GPS,
 * nœuds MeshCore qui annoncent la leur.
 *
 *   ‹                ▔▔▔ 200 m              (échelle)
 *
 *            ● Alice
 *                       ▲   (moi : flèche dans le sens de la marche,
 *                             rond sans cap ; gris si la position est vieille)
 *        ◆ Relais-Nord
 *
 *          (−)    (➤)    (+)
 *            © OpenStreetMap
 *
 * Gestes : glisser déplace (avec élan), pincer ou toucher deux fois zoome,
 * toucher un nœud donne son nom, sa distance et l'âge de son annonce. Le
 * bouton ➤ recentre sur soi et suit la position (allume le GPS s'il est
 * coupé). Retour : flèche ‹ ou bouton de la montre (glisser → déplace la
 * carte).
 *
 * La carte occupe tout l'écran, coins arrondis compris. Elle est dessinée
 * par un seul objet LVGL (évènement DRAW_MAIN) : tuiles du cache PSRAM
 * (maptiles.h), puis nœuds et position. Une tuile pas encore lue est
 * remplacée par sa voisine d'un autre niveau déjà en cache, agrandie ou
 * réduite par LVGL : le zoom ne montre jamais de trou.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "app.h"
#include "ui_kit.h"
#include "src/misc/lv_area_private.h"
#include "i18n.h"
#include "gnss.h"
#include "maptiles.h"
#include "sdcard.h"
#include "display.h"
#include "power.h"
#include "meshcore/mc_port.h"
#include "esp_attr.h"
#include "esp_timer.h"


#define MAP_W       BOARD_LCD_W
#define MAP_H       BOARD_LCD_H
#define NODE_MAX    160
#define LBL_MAX     40                 /* étiquettes posées sans chevauchement */
#define BTN         S(50)
#define DEF_ZOOM    15
#define OVERZOOM    2                  /* niveaux au-delà du plus détaillé présent */
#define C_MAPBG     lv_color_hex(0x15171B)
#define C_HOLE      lv_color_hex(0x24272D)
#define C_ME        lv_color_hex(0x2F7BF6)
#define C_ME_OLD    lv_color_hex(0x8A8F98)

typedef struct {
    char     name[MC_NAME_MAX];
    double   mx, my;                   /* Mercator 0..1 */
    double   lat, lon;
    uint32_t heard;
    uint8_t  type;
    int16_t  lw, lh;                   /* taille de l'étiquette */
    /* vue courante */
    bool     vis, lbl, left;
    int16_t  sx, sy;
} node_t;

/* ---- modèle (survit à la fermeture de l'app) ---- */
static double s_mx = 0.5, s_my = 0.5; /* centre de la vue */
static int s_z = -1;                  /* -1 : vue pas encore placée */
static bool s_follow = true;

/* ---- vue ---- */
static lv_obj_t *v_map, *v_msg, *v_loc, *v_zin, *v_zout, *v_attr;
static lv_timer_t *v_tick, *v_poll, *v_fling;
EXT_RAM_BSS_ATTR static node_t s_node[NODE_MAX];
EXT_RAM_BSS_ATTR static mc_node_pos_t s_npos[NODE_MAX];
EXT_RAM_BSS_ATTR static gnss_info_t s_gi;
static int s_nn;
static mt_state_t s_mts = MT_SCANNING;
static int s_tick_n;

/* moi */
static bool s_me_ok, s_me_fresh;
static double s_me_mx, s_me_my, s_me_lat, s_me_lon;
static float s_me_acc_m, s_me_course;

/* toucher */
static bool s_down, s_moved, s_pinch;
static int s_pinch_d0;
static int32_t s_drag;
static float s_vx, s_vy;
static int64_t s_t_down, s_t_tap;
static lv_point_t s_p_tap;

/* ------------------------------------------------------------------ géométrie */
static double merc_x(double lon) { return (lon + 180.0) / 360.0; }

static double merc_y(double lat)
{
    if (lat > 85.0511) lat = 85.0511;
    if (lat < -85.0511) lat = -85.0511;
    double r = lat * M_PI / 180.0;
    return (1.0 - log(tan(r) + 1.0 / cos(r)) / M_PI) / 2.0;
}

static double merc_lat(double my) { return atan(sinh(M_PI * (1.0 - 2.0 * my))) * 180.0 / M_PI; }

static double world(int z) { return (double)MT_TILE * (double)(1u << z); }

static double dist_m(double la1, double lo1, double la2, double lo2)
{
    double p1 = la1 * M_PI / 180, p2 = la2 * M_PI / 180, dp = p2 - p1, dl = (lo2 - lo1) * M_PI / 180;
    double a = sin(dp / 2) * sin(dp / 2) + cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
    return 6371000.0 * 2 * atan2(sqrt(a), sqrt(1 - a));
}

static int zmin(void)
{
    uint32_t m = mt_zoom_mask();
    return m ? __builtin_ctz(m) : 0;
}

static int zmax(void)
{
    uint32_t m = mt_zoom_mask();
    int z = m ? 31 - __builtin_clz(m) + OVERZOOM : MT_ZMAX;
    return z > MT_ZMAX ? MT_ZMAX : z;
}

/* point Mercator → écran, relatif au coin de la carte */
static void to_screen(double mx, double my, int *sx, int *sy)
{
    double ws = world(s_z);
    double dx = mx - s_mx;
    if (dx > 0.5) dx -= 1.0;           /* au plus court autour du 180e méridien */
    if (dx < -0.5) dx += 1.0;
    *sx = (int)lround(dx * ws) + MAP_W / 2;
    *sy = (int)lround((my - s_my) * ws) + MAP_H / 2;
}

static void clamp_view(void)
{
    s_mx -= floor(s_mx);
    if (s_my < 0) s_my = 0;
    if (s_my > 1) s_my = 1;
}

/* ------------------------------------------------------------------ tuiles voulues */
static void request_tiles(void)
{
    if (s_z < 0 || s_mts != MT_READY) return;
    double ws = world(s_z);
    int n = 1 << s_z;
    double ox = s_mx * ws - MAP_W / 2.0, oy = s_my * ws - MAP_H / 2.0;
    int tx0 = (int)floor(ox / MT_TILE), tx1 = (int)floor((ox + MAP_W - 1) / MT_TILE);
    int ty0 = (int)floor(oy / MT_TILE), ty1 = (int)floor((oy + MAP_H - 1) / MT_TILE);
    double cx = s_mx * ws / MT_TILE, cy = s_my * ws / MT_TILE;
    /* visibles du centre vers les bords, puis une couronne d'avance */
    int want[64][2], nw = 0;
    for (int ring = 0; ring <= 1; ring++) {
        int a0 = tx0 - ring, a1 = tx1 + ring, b0 = ty0 - ring, b1 = ty1 + ring;
        int base = nw;
        for (int ty = b0; ty <= b1; ty++) {
            for (int tx = a0; tx <= a1; tx++) {
                if (ty < 0 || ty >= n || nw >= 64) continue;
                bool inner = tx >= tx0 && tx <= tx1 && ty >= ty0 && ty <= ty1;
                if (ring == 1 && inner) continue;
                want[nw][0] = tx;
                want[nw][1] = ty;
                nw++;
            }
        }
        /* tri par distance au centre (insertion, une vingtaine au plus) */
        for (int i = base + 1; i < nw; i++) {
            int t0 = want[i][0], t1 = want[i][1];
            double d = (t0 + 0.5 - cx) * (t0 + 0.5 - cx) + (t1 + 0.5 - cy) * (t1 + 0.5 - cy);
            int j = i - 1;
            while (j >= base) {
                double e = (want[j][0] + 0.5 - cx) * (want[j][0] + 0.5 - cx) + (want[j][1] + 0.5 - cy) * (want[j][1] + 0.5 - cy);
                if (e <= d) break;
                want[j + 1][0] = want[j][0];
                want[j + 1][1] = want[j][1];
                j--;
            }
            want[j + 1][0] = t0;
            want[j + 1][1] = t1;
        }
    }
    mt_begin();
    for (int i = 0; i < nw; i++) mt_want(s_z, ((want[i][0] % n) + n) % n, want[i][1]);
}

/* ------------------------------------------------------------------ nœuds */
static void load_nodes(void)
{
    int n = mc_node_positions(s_npos, NODE_MAX);
    for (int i = 0; i < n; i++) {
        node_t *d = &s_node[i];
        const mc_node_pos_t *p = &s_npos[i];
        memcpy(d->name, p->name, sizeof d->name);
        d->name[sizeof d->name - 1] = 0;
        d->lat = p->lat_e6 / 1e6;
        d->lon = p->lon_e6 / 1e6;
        d->mx = merc_x(d->lon);
        d->my = merc_y(d->lat);
        d->heard = p->heard;
        d->type = p->type;
        lv_point_t sz;
        lv_text_get_size(&sz, d->name, &font_text, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        d->lw = (int16_t)(sz.x + S(8));
        d->lh = (int16_t)(sz.y + S(2));
    }
    s_nn = n;
}

static bool overlap(const lv_area_t *a, const lv_area_t *b)
{
    return a->x1 <= b->x2 && b->x1 <= a->x2 && a->y1 <= b->y2 && b->y1 <= a->y2;
}

/* positions à l'écran et étiquettes qui ne se chevauchent pas */
static void layout_nodes(void)
{
    lv_area_t placed[LBL_MAX + 2];
    int np = 0;
    /* pas d'étiquette sous les boutons ni sous l'échelle */
    int bw = 3 * BTN / 2 + S(30);
    placed[np++] = (lv_area_t){ MAP_W / 2 - bw, MAP_H - UI_PAD_Y - S(18) - BTN, MAP_W / 2 + bw, MAP_H };
    placed[np++] = (lv_area_t){ 0, 0, MAP_W, UI_PAD_Y + BTN + S(2) };
    int me_x = -1000, me_y = -1000;
    if (s_me_ok) to_screen(s_me_mx, s_me_my, &me_x, &me_y);
    for (int i = 0; i < s_nn; i++) {
        node_t *d = &s_node[i];
        int x, y;
        to_screen(d->mx, d->my, &x, &y);
        d->sx = (int16_t)x;
        d->sy = (int16_t)y;
        d->vis = x > -S(200) && x < MAP_W + S(200) && y > -S(20) && y < MAP_H + S(20);
        d->lbl = false;
        /* étiquette seulement si le point est à l'écran */
        if (!d->vis || np >= LBL_MAX + 2 || x < 0 || x >= MAP_W || y < 0 || y >= MAP_H) continue;
        bool left_first = x + S(12) + d->lw > MAP_W - UI_PAD_X;
        for (int k = 0; k < 2 && !d->lbl; k++) {
            int side = left_first ? !k : k;
            lv_area_t a;
            a.x1 = side ? x - S(12) - d->lw : x + S(12);
            a.x2 = a.x1 + d->lw - 1;
            a.y1 = y - d->lh / 2;
            a.y2 = a.y1 + d->lh - 1;
            if (a.x2 < 0 || a.x1 >= MAP_W) continue;
            bool clash = abs(me_x - (a.x1 + a.x2) / 2) < d->lw / 2 + S(10) && abs(me_y - y) < d->lh / 2 + S(10);
            for (int k = 0; k < np && !clash; k++) clash = overlap(&a, &placed[k]);
            if (clash) continue;
            placed[np++] = a;
            d->lbl = true;
            d->left = side;
        }
    }
}

static void view_changed(void)
{
    clamp_view();
    layout_nodes();
    request_tiles();
    if (v_map) lv_obj_invalidate(v_map);
}

/* ------------------------------------------------------------------ dessin */
static lv_color_t node_color(uint8_t type)
{
    switch (type) {
    case 2: return lv_color_hex(0xF59E0B);     /* répéteur */
    case 3: return lv_color_hex(0xA855F7);     /* salon */
    case 4: return lv_color_hex(0x14B8A6);     /* capteur */
    default: return lv_color_hex(0x22C55E);    /* discussion */
    }
}

/* tuile absente : un niveau voisin déjà en cache, mis à l'échelle et
 * découpé sur la case ; false s'il n'y en a pas */
static bool draw_stand_in(lv_layer_t *layer, const lv_area_t *cell, int z, int tx, int ty)
{
    int n = 1 << z;
    lv_area_t clip;
    if (!lv_area_intersect(&clip, cell, &layer->_clip_area)) return true;
    lv_draw_image_dsc_t d;
    /* plus large : le quart de la tuile parente, agrandi */
    for (int k = 1; k <= 2 && z - k >= 0; k++) {
        const lv_image_dsc_t *p = mt_peek(z - k, ((tx % n) + n) % n >> k, ty >> k, NULL);
        if (!p) continue;
        int m = (1 << k) - 1, sz = MT_TILE << k;
        lv_area_t a;
        a.x1 = cell->x1 - (tx & m) * MT_TILE;
        a.y1 = cell->y1 - (ty & m) * MT_TILE;
        a.x2 = a.x1 + MT_TILE - 1;
        a.y2 = a.y1 + MT_TILE - 1;
        lv_draw_image_dsc_init(&d);
        d.src = p;
        d.scale_x = d.scale_y = LV_SCALE_NONE << k;
        d.pivot.x = 0;
        d.pivot.y = 0;
        (void)sz;
        lv_area_t saved = layer->_clip_area;
        layer->_clip_area = clip;
        lv_draw_image(layer, &d, &a);
        layer->_clip_area = saved;
        return true;
    }
    /* plus détaillé : les quatre tuiles filles, réduites de moitié */
    if (z + 1 <= MT_ZMAX) {
        bool any = false;
        int n2 = n * 2;
        for (int q = 0; q < 4; q++) {
            int cx = 2 * tx + (q & 1), cy = 2 * ty + (q >> 1);
            const lv_image_dsc_t *c = mt_peek(z + 1, ((cx % n2) + n2) % n2, cy, NULL);
            if (!c) continue;
            lv_area_t a;
            a.x1 = cell->x1 + (q & 1) * (MT_TILE / 2);
            a.y1 = cell->y1 + (q >> 1) * (MT_TILE / 2);
            a.x2 = a.x1 + MT_TILE - 1;
            a.y2 = a.y1 + MT_TILE - 1;
            lv_draw_image_dsc_init(&d);
            d.src = c;
            d.scale_x = d.scale_y = LV_SCALE_NONE / 2;
            d.pivot.x = 0;
            d.pivot.y = 0;
            lv_area_t saved = layer->_clip_area;
            layer->_clip_area = clip;
            lv_draw_image(layer, &d, &a);
            layer->_clip_area = saved;
            any = true;
        }
        if (any) return true;
    }
    return false;
}

static void draw_tiles(lv_layer_t *layer, const lv_area_t *co)
{
    const lv_area_t *clip = &layer->_clip_area;
    double ws = world(s_z);
    int n = 1 << s_z;
    double fox = floor(s_mx * ws - MAP_W / 2.0), foy = floor(s_my * ws - MAP_H / 2.0);
    /* coordonnées monde du coin : jusqu'à 2^28, tiennent dans un int */
    int ox = (int)fox, oy = (int)foy;
    int tx0 = (int)floor((fox + (clip->x1 - co->x1)) / MT_TILE);
    int tx1 = (int)floor((fox + (clip->x2 - co->x1)) / MT_TILE);
    int ty0 = (int)floor((foy + (clip->y1 - co->y1)) / MT_TILE);
    int ty1 = (int)floor((foy + (clip->y2 - co->y1)) / MT_TILE);
    lv_draw_rect_dsc_t hole;
    lv_draw_rect_dsc_init(&hole);
    hole.bg_color = C_HOLE;
    hole.border_color = C_OFF;
    hole.border_width = 1;
    for (int ty = ty0; ty <= ty1; ty++) {
        if (ty < 0 || ty >= n) continue;
        for (int tx = tx0; tx <= tx1; tx++) {
            lv_area_t a;
            a.x1 = co->x1 + tx * MT_TILE - ox;
            a.y1 = co->y1 + ty * MT_TILE - oy;
            a.x2 = a.x1 + MT_TILE - 1;
            a.y2 = a.y1 + MT_TILE - 1;
            const lv_image_dsc_t *img = mt_peek(s_z, ((tx % n) + n) % n, ty, NULL);
            if (img) {
                lv_draw_image_dsc_t d;
                lv_draw_image_dsc_init(&d);
                d.src = img;
                lv_draw_image(layer, &d, &a);
            } else if (!draw_stand_in(layer, &a, s_z, tx, ty)) {
                lv_draw_rect(layer, &hole, &a);
            }
        }
    }
}

static void draw_dot(lv_layer_t *layer, int x, int y, int r, lv_color_t c, int border)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = LV_RADIUS_CIRCLE;
    d.bg_color = c;
    d.border_color = lv_color_white();
    d.border_width = border;
    d.shadow_width = 0;
    lv_area_t a = { x - r, y - r, x + r, y + r };
    lv_draw_rect(layer, &d, &a);
}

static void draw_nodes(lv_layer_t *layer, const lv_area_t *co)
{
    lv_draw_rect_dsc_t bg;
    lv_draw_rect_dsc_init(&bg);
    bg.bg_color = lv_color_black();
    bg.bg_opa = LV_OPA_60;
    bg.radius = S(6);
    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    ld.font = &font_text;
    ld.color = C_FG;
    for (int i = 0; i < s_nn; i++) {
        const node_t *d = &s_node[i];
        if (!d->vis) continue;
        int x = co->x1 + d->sx, y = co->y1 + d->sy;
        if (d->lbl) {
            lv_area_t a;
            a.x1 = d->left ? x - S(12) - d->lw : x + S(12);
            a.x2 = a.x1 + d->lw - 1;
            a.y1 = y - d->lh / 2;
            a.y2 = a.y1 + d->lh - 1;
            lv_area_t tmp;
            if (lv_area_intersect(&tmp, &a, &layer->_clip_area)) {
                lv_draw_rect(layer, &bg, &a);
                lv_area_t t = a;
                t.x1 += S(4);
                t.y1 += S(1);
                ld.text = d->name;
                lv_draw_label(layer, &ld, &t);
            }
        }
        draw_dot(layer, x, y, S(7), node_color(d->type), 3);
    }
}

static void draw_me(lv_layer_t *layer, const lv_area_t *co)
{
    if (!s_me_ok) return;
    int x, y;
    to_screen(s_me_mx, s_me_my, &x, &y);
    x += co->x1;
    y += co->y1;
    if (x < -S(60) || x > co->x2 + S(60) || y < -S(60) || y > co->y2 + S(60)) return;
    lv_color_t c = s_me_fresh ? C_ME : C_ME_OLD;
    /* précision : disque pâle, s'il dépasse le repère */
    if (s_me_fresh && s_me_acc_m > 0) {
        double mpp = 40075016.686 * cos(s_me_lat * M_PI / 180) / world(s_z);
        int r = (int)(s_me_acc_m / mpp);
        if (r > S(14) && r < MAP_W) {
            lv_draw_rect_dsc_t d;
            lv_draw_rect_dsc_init(&d);
            d.radius = LV_RADIUS_CIRCLE;
            d.bg_color = C_ME;
            d.bg_opa = LV_OPA_20;
            d.border_color = C_ME;
            d.border_opa = LV_OPA_50;
            d.border_width = 2;
            lv_area_t a = { x - r, y - r, x + r, y + r };
            lv_draw_rect(layer, &d, &a);
        }
    }
    if (s_me_fresh && s_me_course >= 0) {
        /* flèche dans le sens de la marche : ombre, bord blanc, couleur */
        float a = s_me_course * (float)M_PI / 180.0f;
        float sa = sinf(a), ca = cosf(a);
        static const float K[3] = { 1.75f, 1.45f, 1.0f };
        for (int pass = 0; pass < 3; pass++) {
            float k = K[pass];
            /* repère local, pointe vers le haut : pointe, gauche, droite */
            float px[3] = { 0, -S(8) * k, S(8) * k };
            float py[3] = { -S(17) * k, S(9) * k, S(9) * k };
            lv_draw_triangle_dsc_t t;
            lv_draw_triangle_dsc_init(&t);
            t.bg_color = pass == 2 ? c : pass ? lv_color_white() : lv_color_black();
            t.bg_opa = pass ? LV_OPA_COVER : LV_OPA_30;
            for (int i = 0; i < 3; i++) {
                t.p[i].x = (lv_value_precise_t)lroundf(x + px[i] * ca - py[i] * sa);
                t.p[i].y = (lv_value_precise_t)lroundf(y + px[i] * sa + py[i] * ca);
            }
            lv_draw_triangle(layer, &t);
        }
    } else {
        draw_dot(layer, x, y, S(10), c, 4);
    }
}

/* échelle en haut, au centre : la plus grande longueur ronde sous 80 px */
static void draw_scale(lv_layer_t *layer, const lv_area_t *co)
{
    static const int32_t STEP[] = { 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000, 50000,
                                    100000, 200000, 500000, 1000000, 2000000 };
    double mpp = 40075016.686 * cos(merc_lat(s_my) * M_PI / 180) / world(s_z);
    int32_t m = STEP[0];
    for (size_t i = 0; i < sizeof STEP / sizeof STEP[0]; i++)
        if (STEP[i] / mpp <= S(80)) m = STEP[i];
    int len = (int)(m / mpp);
    if (len < 4) return;
    char t[16];
    if (m >= 1000) snprintf(t, sizeof t, "%ld km", (long)(m / 1000));
    else snprintf(t, sizeof t, "%ld m", (long)m);
    lv_point_t ts;
    lv_text_get_size(&ts, t, &font_text, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int w = len + S(8) + ts.x + S(16);
    int x0 = co->x1 + (MAP_W - w) / 2, y0 = co->y1 + UI_PAD_Y + S(6);
    lv_area_t pill = { x0, y0, x0 + w - 1, y0 + ts.y + S(6) - 1 };
    lv_area_t tmp;
    if (!lv_area_intersect(&tmp, &pill, &layer->_clip_area)) return;
    lv_draw_rect_dsc_t bg;
    lv_draw_rect_dsc_init(&bg);
    bg.bg_color = lv_color_black();
    bg.bg_opa = LV_OPA_60;
    bg.radius = LV_RADIUS_CIRCLE;
    lv_draw_rect(layer, &bg, &pill);
    int ym = (pill.y1 + pill.y2) / 2;
    lv_draw_rect_dsc_t bar;
    lv_draw_rect_dsc_init(&bar);
    bar.bg_color = C_FG;
    lv_area_t b = { pill.x1 + S(8), ym + S(4), pill.x1 + S(8) + len - 1, ym + S(6) };
    lv_draw_rect(layer, &bar, &b);
    lv_area_t l = { b.x1, ym - S(4), b.x1 + 2, b.y2 };
    lv_draw_rect(layer, &bar, &l);
    l.x1 = b.x2 - 2;
    l.x2 = b.x2;
    lv_draw_rect(layer, &bar, &l);
    lv_draw_label_dsc_t ld;
    lv_draw_label_dsc_init(&ld);
    ld.font = &font_text;
    ld.color = C_FG;
    ld.text = t;
    lv_area_t ta = { b.x2 + S(8), pill.y1 + S(3), pill.x2, pill.y2 };
    lv_draw_label(layer, &ld, &ta);
}

static void draw_cb(lv_event_t *e)
{
    if (s_z < 0 || s_mts != MT_READY) return;
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t co;
    lv_obj_get_coords(v_map, &co);
    draw_tiles(layer, &co);
    draw_nodes(layer, &co);
    draw_me(layer, &co);
    draw_scale(layer, &co);
}

/* ------------------------------------------------------------------ zoom */
static void update_zoom_btns(void)
{
    if (!v_zin) return;
    bool in = s_z < zmax(), out = s_z > zmin();
    lv_obj_set_state(v_zin, LV_STATE_DISABLED, !in);
    lv_obj_set_state(v_zout, LV_STATE_DISABLED, !out);
}

/* zoom autour du point (ax, ay) de la carte, qui reste en place */
static void zoom_at(int dz, int ax, int ay)
{
    int nz = s_z + dz;
    if (nz < zmin()) nz = zmin();
    if (nz > zmax()) nz = zmax();
    if (nz == s_z) return;
    double ox = (ax - MAP_W / 2.0) / world(s_z), oy = (ay - MAP_H / 2.0) / world(s_z);
    double px = s_mx + ox, py = s_my + oy;
    s_z = nz;
    s_mx = px - (ax - MAP_W / 2.0) / world(s_z);
    s_my = py - (ay - MAP_H / 2.0) / world(s_z);
    if (s_follow && s_me_ok) { s_mx = s_me_mx; s_my = s_me_my; }
    update_zoom_btns();
    view_changed();
}

static void zin_cb(lv_event_t *e) { zoom_at(1, MAP_W / 2, MAP_H / 2); }
static void zout_cb(lv_event_t *e) { zoom_at(-1, MAP_W / 2, MAP_H / 2); }

/* ------------------------------------------------------------------ moi */
static void update_loc_btn(void)
{
    if (!v_loc) return;
    lv_obj_t *l = lv_obj_get_child(v_loc, 0);
    lv_obj_set_style_text_color(l, s_follow ? C_ACC : C_FG, 0);
}

static bool update_me(void)
{
    gnss_get(&s_gi);
    double lat, lon;
    float alt;
    bool alt_ok;
    bool was = s_me_ok;
    double omx = s_me_mx, omy = s_me_my;
    s_me_fresh = s_gi.on && s_gi.st.fix >= 2 && s_gi.fix_age_s < 10;
    if (s_me_fresh) {
        lat = s_gi.st.lat;
        lon = s_gi.st.lon;
        s_me_acc_m = s_gi.st.hdop > 0 ? s_gi.st.hdop * 2.5f : 0;
        s_me_course = s_gi.st.speed_kmh >= 2.5f ? s_gi.st.course_deg : -1;
        s_me_ok = true;
    } else if (gnss_position(&lat, &lon, &alt, &alt_ok, UINT32_MAX)) {
        s_me_ok = true;
    } else {
        s_me_ok = false;
    }
    if (s_me_ok) {
        s_me_lat = lat;
        s_me_lon = lon;
        s_me_mx = merc_x(lon);
        s_me_my = merc_y(lat);
    }
    return s_me_ok != was || s_me_mx != omx || s_me_my != omy;
}

static void loc_cb(lv_event_t *e)
{
    s_follow = true;
    update_loc_btn();
    if (!gnss_is_on()) {
        gnss_set(true);
        gnss_live(true);
        ui_toast(_("GPS allumé : recherche de la position"));
    } else if (!s_me_fresh) {
        ui_toast(_("Recherche de la position…"));
    }
    if (s_me_ok) {
        s_mx = s_me_mx;
        s_my = s_me_my;
        view_changed();
    }
}

/* ------------------------------------------------------------------ toucher */
static void fmt_dist(char *b, size_t n, double m)
{
    if (m < 1000) snprintf(b, n, "%.0f m", m);
    else if (m < 10000) snprintf(b, n, "%.1f km", m / 1000);
    else snprintf(b, n, "%.0f km", m / 1000);
    if (i18n_get() == LANG_FR) for (char *p = b; *p; p++) if (*p == '.') *p = ',';
}

static void fmt_age(char *b, size_t n, uint32_t heard)
{
    time_t now = time(NULL);
    b[0] = 0;
    if (!heard || now < 1600000000 || (time_t)heard > now + 300) return;
    uint32_t s = (uint32_t)(now - (time_t)heard);
    if (s < 3600) snprintf(b, n, _("il y a %lu min"), (unsigned long)(s / 60));
    else if (s < 2 * 86400) snprintf(b, n, _("il y a %lu h"), (unsigned long)(s / 3600));
    else snprintf(b, n, _("il y a %lu j"), (unsigned long)(s / 86400));
}

static void tap(int x, int y)
{
    int best = -1, bd = S(26) * S(26);
    for (int i = 0; i < s_nn; i++) {
        const node_t *d = &s_node[i];
        if (!d->vis) continue;
        int dx = d->sx - x, dy = d->sy - y, dd = dx * dx + dy * dy;
        if (dd < bd) { bd = dd; best = i; }
    }
    if (best < 0) return;
    const node_t *d = &s_node[best];
    char b[MC_NAME_MAX + 64], dist[24] = "", age[32];
    if (s_me_ok) fmt_dist(dist, sizeof dist, dist_m(s_me_lat, s_me_lon, d->lat, d->lon));
    fmt_age(age, sizeof age, d->heard);
    snprintf(b, sizeof b, "%s%s%s%s%s", d->name, dist[0] ? " · " : "", dist, age[0] ? " · " : "", age);
    ui_toast(b);
}

static void fling_cb(lv_timer_t *t)
{
    if (fabsf(s_vx) < 0.6f && fabsf(s_vy) < 0.6f) {
        lv_timer_pause(t);
        return;
    }
    double ws = world(s_z);
    s_mx -= s_vx / ws;
    s_my -= s_vy / ws;
    s_vx *= 0.9f;
    s_vy *= 0.9f;
    view_changed();
}

static void map_ev(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    lv_indev_t *in = lv_indev_active();
    if (!in || s_z < 0) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);
    lv_area_t co;
    lv_obj_get_coords(v_map, &co);
    p.x -= co.x1;
    p.y -= co.y1;
    int64_t now = esp_timer_get_time();
    if (c == LV_EVENT_PRESSED) {
        s_down = true;
        s_moved = s_pinch = false;
        s_drag = 0;
        s_vx = s_vy = 0;
        s_t_down = now;
        if (v_fling) lv_timer_pause(v_fling);
    } else if (c == LV_EVENT_PRESSING) {
        int x2, y2;
        if (display_touch_second(&x2, &y2)) {
            x2 -= co.x1;
            y2 -= co.y1;
            int dx = x2 - p.x, dy = y2 - p.y;
            int d = (int)sqrtf((float)(dx * dx + dy * dy));
            if (!s_pinch) {
                s_pinch = true;
                s_pinch_d0 = d;
            } else if (s_pinch_d0 > 0 && (d * 100 > s_pinch_d0 * 145 || d * 100 < s_pinch_d0 * 69)) {
                bool in_ = d > s_pinch_d0;
                s_follow = false;
                update_loc_btn();
                zoom_at(in_ ? 1 : -1, (p.x + x2) / 2, (p.y + y2) / 2);
                s_pinch_d0 = d;
            }
            return;
        }
        if (s_pinch) return;                   /* un doigt levé : on attend le relâcher */
        lv_point_t v;
        lv_indev_get_vect(in, &v);
        if (!v.x && !v.y) return;
        s_drag += abs(v.x) + abs(v.y);
        if (s_drag > S(8)) {
            s_moved = true;
            if (s_follow) { s_follow = false; update_loc_btn(); }
        }
        if (!s_moved) return;
        double ws = world(s_z);
        s_mx -= v.x / ws;
        s_my -= v.y / ws;
        s_vx = 0.5f * s_vx + 0.5f * v.x;
        s_vy = 0.5f * s_vy + 0.5f * v.y;
        view_changed();
    } else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) {
        if (!s_down) return;
        s_down = false;
        if (s_pinch) return;
        if (s_moved) {
            if (v_fling && (fabsf(s_vx) > 3 || fabsf(s_vy) > 3)) lv_timer_resume(v_fling);
            return;
        }
        if (now - s_t_down > 500000 || c == LV_EVENT_PRESS_LOST) return;
        if (now - s_t_tap < 350000 && abs(p.x - s_p_tap.x) < S(30) && abs(p.y - s_p_tap.y) < S(30)) {
            s_t_tap = 0;                       /* double toucher : zoom sur le point */
            s_follow = false;
            update_loc_btn();
            zoom_at(1, p.x, p.y);
            return;
        }
        s_t_tap = now;
        s_p_tap = p;
        tap(p.x, p.y);
    }
}

/* ------------------------------------------------------------------ état */
static void place_initial_view(void)
{
    int lo = zmin(), hi = zmax() - OVERZOOM;
    if (hi < lo) hi = lo;
    update_me();
    mc_status_t ms;
    double cx, cy;
    if (s_me_ok) {
        s_mx = s_me_mx;
        s_my = s_me_my;
        s_z = DEF_ZOOM;
    } else if (mc_get_status(&ms), ms.lat != 0 || ms.lon != 0) {
        s_mx = merc_x(ms.lon);
        s_my = merc_y(ms.lat);
        s_z = DEF_ZOOM;
        s_follow = false;
    } else if (mt_center(&cx, &cy)) {
        s_mx = cx;
        s_my = cy;
        s_z = lo;
        s_follow = false;
    } else {
        s_z = lo;
    }
    if (s_z > hi) s_z = hi;
    if (s_z < lo) s_z = lo;
}

static void show_state(void)
{
    const char *m = NULL;
    switch (s_mts) {
    case MT_SCANNING: m = _("Lecture de la carte SD…"); break;
    case MT_NO_SD: {
        sd_info_t si;
        sd_get_info(&si);
        m = si.state == SD_SHARED ? _("Carte SD prêtée au PC.") : _("Pas de carte SD.\nLes cartes s'y rangent dans le dossier tiles.");
        break;
    }
    case MT_NO_TILES: m = _("Aucune carte sur la carte SD.\nCopier les tuiles dans tiles/z/x/y.png"); break;
    case MT_NO_MEM: m = _("Mémoire insuffisante pour les cartes."); break;
    default: break;
    }
    if (m) {
        lv_label_set_text(v_msg, m);
        lv_obj_remove_flag(v_msg, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(v_msg, LV_OBJ_FLAG_HIDDEN);
    }
    bool ready = s_mts == MT_READY;
    lv_obj_t *b[] = { v_zin, v_zout, v_loc, v_attr };
    for (int i = 0; i < 4; i++) {
        if (ready) lv_obj_remove_flag(b[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(b[i], LV_OBJ_FLAG_HIDDEN);
    }
}

/* toutes les 50 ms : tuiles arrivées, état du cache */
static void poll_cb(lv_timer_t *t)
{
    mt_state_t st = mt_state();
    if (st == MT_NO_SD && sd_mounted()) {
        mt_rescan();
        st = MT_SCANNING;
    }
    if (st != s_mts) {
        s_mts = st;
        if (st == MT_READY) {
            if (s_z < 0) place_initial_view();
            else {
                if (s_z > zmax()) s_z = zmax();
                if (s_z < zmin()) s_z = zmin();
            }
            update_zoom_btns();
            update_loc_btn();
            view_changed();
        }
        show_state();
        lv_obj_invalidate(v_map);
    }
    if (mt_take_dirty()) lv_obj_invalidate(v_map);
}

/* chaque seconde : position ; toutes les 15 s : nœuds MeshCore */
static void tick_cb(lv_timer_t *t)
{
    if (s_tick_n++ % 15 == 0) mc_conv_refresh();
    if (s_tick_n % 15 == 2) {
        load_nodes();
        if (s_z >= 0) { layout_nodes(); lv_obj_invalidate(v_map); }
    }
    if (!update_me() || s_z < 0) return;
    if (s_follow && s_me_ok && !s_down) {
        s_mx = s_me_mx;
        s_my = s_me_my;
        view_changed();
    } else {
        layout_nodes();
        lv_obj_invalidate(v_map);
    }
}

/* ------------------------------------------------------------------ vue */
static lv_obj_t *round_btn(lv_obj_t *scr, const char *sym, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(scr);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, BTN, BTN);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x1A1D23), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_80, 0);
    lv_obj_set_style_bg_color(b, C_OFF, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(b, C_OFF, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_text_opa(b, LV_OPA_30, LV_STATE_DISABLED);
    lv_obj_set_ext_click_area(b, S(6));
    lv_obj_remove_flag(b, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = ui_label(b, &font_title, C_FG, sym);
    lv_obj_center(l);
    return b;
}

static void back_cb(lv_event_t *e) { app_close(); }

static void create(lv_obj_t *scr)
{
    s_mts = MT_SCANNING;
    s_tick_n = 0;
    s_down = false;
    mt_open();
    mc_conv_refresh();
    load_nodes();

    /* carte plein écran, coins compris (l'écran a une marge intérieure) */
    v_map = lv_obj_create(scr);
    lv_obj_remove_style_all(v_map);
    lv_obj_set_size(v_map, MAP_W, MAP_H);
    lv_obj_set_pos(v_map, -UI_PAD_X, -UI_PAD_Y);
    lv_obj_set_style_bg_color(v_map, C_MAPBG, 0);
    lv_obj_set_style_bg_opa(v_map, LV_OPA_COVER, 0);
    lv_obj_add_flag(v_map, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(v_map, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_SCROLL_CHAIN |
                                  LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_event_cb(v_map, draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(v_map, map_ev, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(v_map, map_ev, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(v_map, map_ev, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(v_map, map_ev, LV_EVENT_PRESS_LOST, NULL);

    v_msg = ui_label(scr, &font_text, C_FG, "");
    lv_label_set_long_mode(v_msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(v_msg, UI_W - S(20));
    lv_obj_set_style_text_align(v_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(v_msg, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *bk = round_btn(scr, LV_SYMBOL_LEFT, back_cb);
    lv_obj_set_style_text_color(lv_obj_get_child(bk, 0), C_ACC, 0);
    lv_obj_align(bk, LV_ALIGN_TOP_LEFT, S(4), 0);

    v_loc = round_btn(scr, SYM_LOCATE, loc_cb);
    lv_obj_align(v_loc, LV_ALIGN_BOTTOM_MID, 0, -S(14));
    v_zout = round_btn(scr, SYM_MINUS, zout_cb);
    lv_obj_align_to(v_zout, v_loc, LV_ALIGN_OUT_LEFT_MID, -S(22), 0);
    v_zin = round_btn(scr, SYM_PLUS, zin_cb);
    lv_obj_align_to(v_zin, v_loc, LV_ALIGN_OUT_RIGHT_MID, S(22), 0);

    v_attr = ui_label(scr, &font_small, lv_color_hex(0x5A5F68), "© OpenStreetMap");
    lv_obj_align(v_attr, LV_ALIGN_BOTTOM_MID, 0, S(10));

    show_state();
    update_loc_btn();
    v_poll = lv_timer_create(poll_cb, 50, NULL);
    v_tick = lv_timer_create(tick_cb, 1000, NULL);
    v_fling = lv_timer_create(fling_cb, 16, NULL);
    lv_timer_pause(v_fling);
    gnss_live(true);
    poll_cb(v_poll);
}

static void destroy(void)
{
    if (v_poll) lv_timer_delete(v_poll);
    if (v_tick) lv_timer_delete(v_tick);
    if (v_fling) lv_timer_delete(v_fling);
    v_poll = v_tick = v_fling = NULL;
    gnss_live(false);
    mt_close();
    v_map = v_msg = v_loc = v_zin = v_zout = v_attr = NULL;
    s_nn = 0;
}

static void on_show(void)
{
    gnss_live(true);
    if (v_poll) lv_timer_resume(v_poll);
    if (v_tick) lv_timer_resume(v_tick);
    if (v_map) {
        update_me();
        if (s_follow && s_me_ok && s_z >= 0) { s_mx = s_me_mx; s_my = s_me_my; }
        view_changed();
    }
}

static void on_hide(void)
{
    gnss_live(false);
    if (v_poll) lv_timer_pause(v_poll);
    if (v_tick) lv_timer_pause(v_tick);
    if (v_fling) lv_timer_pause(v_fling);
}

const app_t app_map = {
    .name = N_("Cartes"),
    .icon = SYM_MAP,
    .color = 0x22C55E,
    .create = create,
    .destroy = destroy,
    .on_show = on_show,
    .on_hide = on_hide,
    .screen_timeout_ms = 60 * 1000,
};
