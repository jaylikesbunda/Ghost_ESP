#include "gui/view_input.h"

bool view_input_is_back_key(uint32_t key) {
    return key == (uint32_t)LV_KEY_ESC || key == 27u || key == 29u || key == (uint32_t)'`';
}

bool view_input_is_back_event(const InputEvent *event) {
    if (!event) return false;
    if (event->type != INPUT_TYPE_KEYBOARD) return false;
    return view_input_is_back_key(event->data.key_value);
}

bool view_input_is_exit_event(const InputEvent *event) {
    if (!event) return false;
    if (event->type != INPUT_TYPE_EXIT_BUTTON) return false;
    return event->data.exit_pressed;
}

bool view_input_wants_back(const InputEvent *event) {
    if (!event) return false;
    if (view_input_is_exit_event(event)) return true;
    return view_input_is_back_event(event);
}

int view_input_swipe_threshold_x(int ratio) {
    if (ratio <= 0) ratio = 10;
    int w = LV_HOR_RES;
    int t = w / ratio;
    return t > 0 ? t : 1;
}

void view_input_scroll_page(lv_obj_t *scrollable, bool up) {
    if (!scrollable || !lv_obj_is_valid(scrollable)) return;
    lv_coord_t h = lv_obj_get_height(scrollable);
    lv_coord_t amt = h / 2;
    if (amt <= 0) return;
    lv_obj_scroll_by_bounded(scrollable, 0, up ? amt : -amt, LV_ANIM_OFF);
}

void view_input_make_list_scrollable(lv_obj_t *list) {
    if (!list || !lv_obj_is_valid(list)) return;
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
}

void view_input_make_log_scrollable(lv_obj_t *log) {
    if (!log || !lv_obj_is_valid(log)) return;
    lv_obj_add_flag(log, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(log, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_scroll_dir(log, LV_DIR_VER);
    lv_obj_set_scroll_snap_x(log, LV_SCROLL_SNAP_NONE);
    lv_obj_set_scrollbar_mode(log, LV_SCROLLBAR_MODE_AUTO);
}
