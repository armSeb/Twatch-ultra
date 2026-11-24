/*
 * face_bake.c — voir face_bake.h.
 *
 * Le rendu de l'image se fait depuis un minuteur LVGL, donc dans la tâche
 * app (pile de 8 Ko, celle qui dessine l'écran) : les cadrans sont créés au
 * démarrage depuis la tâche main, dont la pile de 3,5 Ko déborde pendant un
 * rendu complet. D'ici là, le décor reste dessiné objet par objet.
 */
#include "face_bake.h"

static void img_delete_cb(lv_event_t *e)
{
    lv_draw_buf_t *buf = lv_event_get_user_data(e);
    lv_draw_buf_destroy(buf);
}

static void bake_now(lv_obj_t *base)
{
    /* fond de l'écran noir : l'image RGB565 sans alpha a ses coins noirs */
    lv_draw_buf_t *buf = lv_snapshot_take(base, LV_COLOR_FORMAT_RGB565);
    if (!buf) return;                    /* mémoire : le décor reste en objets */
    lv_obj_t *parent = lv_obj_get_parent(base);
    lv_obj_t *img = lv_image_create(parent);
    lv_obj_remove_style_all(img);
    lv_image_set_src(img, buf);
    lv_obj_set_pos(img, lv_obj_get_x(base), lv_obj_get_y(base));   /* base sans ombre ni débord */
    lv_obj_add_flag(img, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(img, img_delete_cb, LV_EVENT_DELETE, buf);
    lv_obj_move_to_index(img, lv_obj_get_index(base));
    lv_obj_delete(base);
}

/* cadran détruit avant le rendu : le minuteur part avec lui */
static void base_delete_cb(lv_event_t *e)
{
    lv_timer_delete(lv_event_get_user_data(e));
}

static void bake_timer_cb(lv_timer_t *t)
{
    lv_obj_t *base = lv_timer_get_user_data(t);
    lv_timer_delete(t);
    lv_obj_remove_event_cb_with_user_data(base, base_delete_cb, t);
    bake_now(base);
}

void face_bake(lv_obj_t *base)
{
    lv_timer_t *t = lv_timer_create(bake_timer_cb, 0, base);
    if (!t) return;
    lv_obj_add_event_cb(base, base_delete_cb, LV_EVENT_DELETE, t);
}
