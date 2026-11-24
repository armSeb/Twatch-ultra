/*
 * face_bake.c — voir face_bake.h.
 */
#include "face_bake.h"
#include "src/misc/lv_area_private.h"   /* lv_area_intersect */

#define TILE 16                       /* carreau : 16 × 16 pixels */
#define TILE_DETAIL 0xFFFFFFFFu       /* carreau à lire dans l'image */

typedef struct {
    lv_obj_t *off;                    /* écran hors affichage qui porte base */
    lv_obj_t *base;                   /* contenu figé */
    lv_obj_t *view;                   /* objet affiché, dessine l'image */
    lv_draw_buf_t *buf;               /* image RGB565 (PSRAM) */
    uint32_t *tiles;                  /* couleur RGB565 d'un carreau uni, ou TILE_DETAIL */
    int tw, th;                       /* carreaux en largeur, en hauteur */
    lv_timer_t *timer;
    bool ready;
} bake_t;

static void scan_tiles(bake_t *b)
{
    const uint8_t *data = b->buf->data;
    uint32_t stride = b->buf->header.stride;
    int w = b->buf->header.w, h = b->buf->header.h;
    for (int ty = 0; ty < b->th; ty++) {
        for (int tx = 0; tx < b->tw; tx++) {
            int x0 = tx * TILE, y0 = ty * TILE;
            int x1 = LV_MIN(x0 + TILE, w), y1 = LV_MIN(y0 + TILE, h);
            const uint16_t *row = (const uint16_t *)(data + (size_t)y0 * stride) + x0;
            uint16_t c = row[0];
            bool solid = true;
            for (int y = y0; y < y1 && solid; y++, row = (const uint16_t *)((const uint8_t *)row + stride))
                for (int x = 0; x < x1 - x0; x++) if (row[x] != c) { solid = false; break; }
            b->tiles[ty * b->tw + tx] = solid ? c : TILE_DETAIL;
        }
    }
}

static void bake_timer_cb(lv_timer_t *t)
{
    bake_t *b = lv_timer_get_user_data(t);
    /* pas pendant une animation : le rendu complet (~50 ms) la saccaderait */
    if (lv_anim_count_running() > 0) { lv_timer_set_period(t, 50); return; }
    lv_timer_pause(t);
    lv_timer_set_period(t, 0);
    if (!b->buf) {
        b->buf = lv_snapshot_take(b->base, LV_COLOR_FORMAT_RGB565);
        if (!b->buf) return;          /* mémoire : rien d'affiché, nouvel essai au prochain changement */
        b->tw = (b->buf->header.w + TILE - 1) / TILE;
        b->th = (b->buf->header.h + TILE - 1) / TILE;
        b->tiles = lv_malloc(sizeof(uint32_t) * b->tw * b->th);
        if (!b->tiles) { lv_draw_buf_destroy(b->buf); b->buf = NULL; return; }
    } else if (lv_snapshot_take_to_draw_buf(b->base, LV_COLOR_FORMAT_RGB565, b->buf) != LV_RESULT_OK) {
        return;
    }
    lv_image_cache_drop(b->buf);
    scan_tiles(b);
    b->ready = true;
    lv_obj_invalidate(b->view);
}

/* Dessin de l'image, bande par bande : carreaux unis en aplats (aucune
 * lecture PSRAM), carreaux détaillés lus dans l'image par plages. */
static void view_draw_cb(lv_event_t *e)
{
    bake_t *b = lv_event_get_user_data(e);
    if (!b->ready) return;
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t co;
    lv_obj_get_coords(b->view, &co);
    lv_area_t clip;
    if (!lv_area_intersect(&clip, &layer->_clip_area, &co)) return;
    const lv_area_t clip0 = layer->_clip_area;

    lv_draw_image_dsc_t img;
    lv_draw_image_dsc_init(&img);
    img.src = b->buf;
    lv_draw_rect_dsc_t fill;
    lv_draw_rect_dsc_init(&fill);
    fill.bg_opa = LV_OPA_COVER;

    int ty0 = (clip.y1 - co.y1) / TILE, ty1 = (clip.y2 - co.y1) / TILE;
    int tx0 = (clip.x1 - co.x1) / TILE, tx1 = (clip.x2 - co.x1) / TILE;
    for (int ty = ty0; ty <= ty1; ty++) {
        int32_t y1 = LV_MAX(co.y1 + ty * TILE, clip.y1), y2 = LV_MIN(co.y1 + ty * TILE + TILE - 1, clip.y2);
        const uint32_t *row = b->tiles + ty * b->tw;
        int tx = tx0;
        while (tx <= tx1) {
            uint32_t k = row[tx];
            int end = tx;
            while (end + 1 <= tx1 && row[end + 1] == k) end++;
            lv_area_t a = { LV_MAX(co.x1 + tx * TILE, clip.x1), y1,
                            LV_MIN(co.x1 + end * TILE + TILE - 1, clip.x2), y2 };
            if (k == TILE_DETAIL) {
                layer->_clip_area = a;
                lv_draw_image(layer, &img, &co);
                layer->_clip_area = clip0;
            } else {
                uint16_t c = (uint16_t)k;
                fill.bg_color = lv_color_make((uint8_t)((c >> 8) & 0xF8), (uint8_t)((c >> 3) & 0xFC),
                                              (uint8_t)(c << 3));
                lv_draw_rect(layer, &fill, &a);
            }
            tx = end + 1;
        }
    }
}

static void holder_delete_cb(lv_event_t *e)
{
    bake_t *b = lv_event_get_user_data(e);
    if (b->timer) lv_timer_delete(b->timer);
    if (b->off) lv_obj_delete(b->off);
    if (b->buf) { lv_image_cache_drop(b->buf); lv_draw_buf_destroy(b->buf); }
    lv_free(b->tiles);
    lv_free(b);
}

lv_obj_t *face_bake_create(lv_obj_t *holder)
{
    bake_t *b = lv_malloc_zeroed(sizeof(bake_t));
    LV_ASSERT_MALLOC(b);
    lv_obj_update_layout(holder);
    int32_t w = lv_obj_get_width(holder), h = lv_obj_get_height(holder);

    b->off = lv_obj_create(NULL);
    lv_obj_remove_style_all(b->off);
    b->base = lv_obj_create(b->off);
    lv_obj_remove_style_all(b->base);
    lv_obj_set_size(b->base, w, h);
    lv_obj_set_pos(b->base, 0, 0);

    b->view = lv_obj_create(holder);
    lv_obj_remove_style_all(b->view);
    lv_obj_set_size(b->view, w, h);
    lv_obj_set_pos(b->view, 0, 0);
    lv_obj_add_flag(b->view, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_remove_flag(b->view, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(b->view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(b->view, view_draw_cb, LV_EVENT_DRAW_MAIN, b);

    lv_obj_set_user_data(holder, b);
    lv_obj_add_event_cb(holder, holder_delete_cb, LV_EVENT_DELETE, b);
    b->timer = lv_timer_create(bake_timer_cb, 0, b);
    return b->base;
}

void face_bake_dirty(lv_obj_t *holder)
{
    bake_t *b = holder ? lv_obj_get_user_data(holder) : NULL;
    if (!b || !b->timer) return;
    lv_timer_resume(b->timer);
}
