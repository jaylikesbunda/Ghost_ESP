#include "managers/views/keyboard_screen.h"
#include "core/serial_manager.h"
#include "managers/views/options_screen.h"
#include "managers/views/terminal_screen.h"
#include "managers/views/main_menu_screen.h"
#include "gui/screen_layout.h"
#include "gui/accessibility_fonts.h"
#include "gui/design_tokens.h"
#include "gui/theme_palette_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "managers/settings_manager.h"
#include "gui/lvgl_safe.h"
#include <string.h>
#include <ctype.h>

#if defined(CONFIG_IS_ATOMS3R) && !defined(CONFIG_USE_JOYSTICK)
// AtomS3R's single button drives the encoder-style keyboard (a horizontal glyph
// strip), which fits the 128px display far better than the full key matrix.
// The button's tap scheme maps onto encoder actions in the input handler:
// tap = next glyph, double tap = previous, hold = type, triple tap = close.
// This define is local to this translation unit (it is set after all includes),
// so it only reroutes this file's gating, not the rest of the firmware.
#define CONFIG_USE_ENCODER 1
#endif

#define KEYBOARD_COLUMNS 10

/* iOS-style matrix layout: every row of every page sums to KB_UNITS_PER_ROW so
 * lv_btnmatrix's per-row normalization produces one uniform key size. Units are
 * half-keys (a normal key is 2). */
#define KB_UNITS_PER_ROW 20
#define KB_IOS_ROWS      4
#define KB_IOS_MAX_BTNS  40
#define KB_KEY_TEXT_LEN  8

static inline int kb_clamp_int(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline int kb_pad_h(int screen_w) {
    int pad = kb_clamp_int(screen_w / 40, 2, GUI_SAFEAREA_HOR);
#if defined(CONFIG_CROWPANEL_1P28_ROTARY)
    /* Round aperture: keep the outer keys clear of the bezel. */
    pad += pad / 8 + 2;
#endif
    return pad;
}

static inline int kb_field_h(int screen_h) {
    return kb_clamp_int(screen_h / 8, GUI_CONTROL_H / 2, GUI_CONTROL_H);
}

static inline int keyboard_layout_padding(void) {
    return kb_pad_h(LV_HOR_RES);
}

static inline int keyboard_display_height(void) {
    return kb_field_h(LV_VER_RES);
}

static const char *TAG = "keyboard_screen";

uint32_t theme_palette_get_background(uint8_t theme);
uint32_t theme_palette_get_surface(uint8_t theme);
uint32_t theme_palette_get_text(uint8_t theme);
uint32_t theme_palette_get_accent(uint8_t theme);
uint32_t theme_palette_get_text_muted(uint8_t theme);
bool theme_palette_is_bright(uint8_t theme);

static inline lv_color_t kb_bg(void) {
    return lv_color_hex(theme_palette_get_background(settings_get_menu_theme(&G_Settings)));
}
static inline lv_color_t kb_surface(void) {
    return lv_color_hex(theme_palette_get_surface(settings_get_menu_theme(&G_Settings)));
}
static inline lv_color_t kb_text(void) {
    return lv_color_hex(theme_palette_get_text(settings_get_menu_theme(&G_Settings)));
}
static inline lv_color_t kb_accent(void) {
    return lv_color_hex(theme_palette_get_accent(settings_get_menu_theme(&G_Settings)));
}
static inline bool kb_is_bright(void) {
    return theme_palette_is_bright(settings_get_menu_theme(&G_Settings));
}
static inline lv_color_t kb_sel_text(void) {
    return kb_is_bright() ? lv_color_hex(0x000000) : lv_color_hex(0xFFFFFF);
}
static inline lv_coord_t kb_radius(void) {
    return settings_get_menu_rounded(&G_Settings) ? GUI_RADIUS_SM / 2 : 0;
}


static lv_obj_t *root = NULL;
static lv_obj_t *input_label = NULL;
/* Field height bounds resolved at create; the field grows between them as the
 * text wraps. Equal values mean the display has no spare room to grow into. */
static int kb_field_base_h = 0;
static int kb_field_max_h = 0;
static char input_buffer[128] = {0};
static int input_len = 0;
static char pending_initial_text[128] = {0};
static bool has_pending_initial_text = false;
static bool start_with_caps = true;
static KeyboardSubmitCallback submit_callback = NULL;
static KeyboardImmediateCallback immediate_callback = NULL;

static bool is_caps = true;
static bool is_symbols_mode = false;
static bool is_capslock = false;
/* Second symbols page (#+=). Only the btnmatrix path sets this; the legacy
 * per-key and encoder paths only ever use page one. */
static bool kb_sym2 = false;
#if defined(CONFIG_USE_ENCODER) && !defined(CONFIG_USE_JOYSTICK)
static lv_obj_t *encoder_cont = NULL;
static lv_obj_t *encoder_selector = NULL;
static int encoder_selector_w = 0;
static int encoder_selector_h = 0;
static lv_obj_t *encoder_labels[50];
static const char *encoder_alpha_items[41] = {
    "Aa","A","B","C","D","E","F","G","H","I","J",
    "K","L","M","N","O","P","Q","R","S","T",
    "U","V","W","X","Y","Z","0","1","2","3",
    "4","5","6","7","8","9","SPA","SYM",LV_SYMBOL_BACKSPACE,LV_SYMBOL_NEW_LINE
};
static const int encoder_alpha_count = 41;
static const char *encoder_sym_items[40] = {
    "1","2","3","4","5","6","7","8","9","0",
    "!","@","#","$","%","^","&","*","(",")",
    "-","_","=","+","[","]","{","}","\\","|",
    ";",":","'","\"","<",">","?","/","ABC",LV_SYMBOL_NEW_LINE
};
static const int encoder_sym_count = 40;
static const char **encoder_items = NULL;
static int encoder_item_count = 0;
static int encoder_sel_idx = 0;
static int encoder_item_spacing = 0;
static int encoder_screen_width = 0;
static int encoder_offset_x = 0;
static bool encoder_sym_mode = false;
static bool encoder_uppercase = true;

static void encoder_set_item_text(int i);
static void encoder_set_item_style(int i);
static void encoder_position_item(int i);
static void encoder_position_selector(void);
static int encoder_scroll_x_for_index(int i);
#endif

static char placeholder[64] = "Enter text...";

static bool styles_inited = false;
static lv_style_t style_key_btn;
static lv_style_t style_key_label;
// temporarily override rounded corners during initial build to avoid mask cost
static int saved_key_radius = 3;
static int saved_input_label_radius = 5;
static bool radius_override_active = false;
// btnmatrix-based keyboard to avoid per-key object creation
static lv_obj_t *key_matrix = NULL;
static const char *btn_map[64];
static uint8_t btn_widths[64];
static int btn_map_len = 0;
static int shift_btn_id = -1;
static int pressed_btn_id = -1;

static void key_matrix_event_cb(lv_event_t *e);
static void build_key_matrix(void);

/* One entry per key: label, width in half-keys, and whether it is a layout
 * spacer. NULL label terminates a row. */
typedef struct {
    const char *label;
    uint8_t units;
    uint8_t spacer;
} kb_key_desc_t;

/* Defined below; the geometry helpers need them here. */
static const kb_key_desc_t * const *kb_active_page(void);
static int kb_row_btn_count(int row);
static const char *(*get_current_keys(void))[10];
static const int *get_current_row_lengths(void);

/* Legacy tables: still consumed by the encoder strip and the per-key fallback
 * path. The btnmatrix path uses the iOS-style descriptor tables below. They sit
 * here because the geometry helpers reference num_rows. */
static const int row_lengths[] = {10, 9, 8, 5, 3};
static const int symbols_row_lengths[] = {10, 10, 10, 8, 3};
static const int max_row_lengths[] = {10, 10, 10, 8, 3};
static const int num_rows = 5;

/* ---------------------------------------------------------------------------
 * Resolved matrix geometry
 *
 * Single source of truth for the key sizes, gutters and key font, derived from
 * the display size so one layout scales from 128px panels to 1024x600.
 * ------------------------------------------------------------------------ */
typedef struct {
    int pad_h;
    int field_h;
    int field_max_h;   /* upper bound for content-driven growth */
    int matrix_w;
    int matrix_h;
    int matrix_y;
    int key_h;
    int gap;
    const lv_font_t *font;
} kb_metrics_t;

static void kb_compute_metrics(kb_metrics_t *m, int sw, int sh, int status_bar_h) {
    m->pad_h = kb_pad_h(sw);
    m->matrix_w = sw - 2 * m->pad_h;
    if (m->matrix_w < KB_UNITS_PER_ROW) m->matrix_w = KB_UNITS_PER_ROW;

    int key_w = (m->matrix_w * 2) / KB_UNITS_PER_ROW;
    if (key_w < 8) key_w = 8;
    int gap = kb_clamp_int(key_w / 6, 1, GUI_GRID);

    int top = status_bar_h + GUI_SAFEAREA_VER;
    int bottom = sh - GUI_SAFEAREA_VER;
    int avail_total = bottom - top;
    if (avail_total < GUI_CONTROL_H) avail_total = GUI_CONTROL_H;

    int field_min = GUI_CONTROL_H / 2;
    int field_min_prop = sh / 12;
    if (field_min_prop > field_min) field_min = field_min_prop;
    int avail_for_keys = avail_total - field_min;
    if (avail_for_keys < KB_IOS_ROWS) avail_for_keys = KB_IOS_ROWS;

    /* iOS keys are ~31.5 x 42 pt, so drive the height from the key width rather
     * than from leftover vertical space. Rows may grow to 1.6x that ratio on
     * tall portrait panels, but never further: the field below takes the rest,
     * which keeps the view full-bleed instead of leaving a dead band between the
     * text and the keys. */
    int key_h = (key_w * 4) / 3;
    int key_h_max = (key_w * 8) / 5;
    int need = KB_IOS_ROWS * key_h + (KB_IOS_ROWS - 1) * gap;
    if (need > avail_for_keys) {
        /* Tight screen: compress the rows, down to a floor. */
        key_h = (avail_for_keys - (KB_IOS_ROWS - 1) * gap) / KB_IOS_ROWS;
        if (key_h < 14) key_h = 14;
        need = KB_IOS_ROWS * key_h + (KB_IOS_ROWS - 1) * gap;
        if (need > avail_total - field_min) need = avail_total - field_min;
    } else {
        int grow = (avail_for_keys - (KB_IOS_ROWS - 1) * gap) / KB_IOS_ROWS;
        if (grow > key_h_max) grow = key_h_max;
        if (grow > key_h) key_h = grow;
        need = KB_IOS_ROWS * key_h + (KB_IOS_ROWS - 1) * gap;
    }

    m->key_h = key_h;
    m->gap = gap;
    m->matrix_h = need;
    /* Bottom-anchored, like the iOS keyboard. */
    m->matrix_y = bottom - need;

    /* The field starts at one line and grows with the text into whatever space
     * is left above the keys, so spare height on a large display goes to the
     * text area instead of sitting empty between the two. */
    m->field_h = field_min;
    int field_room = m->matrix_y - GUI_GRID - top;
    if (field_room < field_min) field_room = field_min;
    int field_cap = avail_total * 3 / 5;
    if (field_cap > field_room) field_cap = field_room;
    m->field_max_h = field_cap;
    m->font = gui_font_for_height((lv_coord_t)key_h);
}

/* Key identifier at a cursor position, or NULL. Matrix builds report their
 * descriptor labels (SHIFT/DEL/Exit/Done/123/ABC/#+=/" " and single chars). */
static const char *kb_key_label_at(int row, int col) {
    if (!key_matrix) {
        const char *(*ck)[10] = get_current_keys();
        const int *lens = get_current_row_lengths();
        if (row < 0 || row >= num_rows || col < 0 || col >= lens[row]) return NULL;
        return ck[row][col];
    }
    if (row < 0 || row >= KB_IOS_ROWS || col < 0) return NULL;
    const kb_key_desc_t * const *page = kb_active_page();
    const kb_key_desc_t *k = page[row];
    int sel = 0;
    while (k && k->label) {
        if (!k->spacer) {
            if (sel == col) return k->label;
            sel++;
        }
        k++;
    }
    return NULL;
}

/* Map a screen point to (row, selectable column) by replicating lv_btnmatrix's
 * own layout arithmetic, reading the live widget style, so hit-testing can
 * never drift from what is drawn. Spacers resolve to no column. */
static bool kb_matrix_hit_test(int tx, int ty, int *out_row, int *out_col) {
    if (!key_matrix) return false;
    int km_x = lv_obj_get_x(key_matrix);
    int km_y = lv_obj_get_y(key_matrix);
    int w = lv_obj_get_width(key_matrix);
    int h = lv_obj_get_height(key_matrix);
    if (tx < km_x || tx >= km_x + w || ty < km_y || ty >= km_y + h) return false;

    lv_coord_t ptop = lv_obj_get_style_pad_top(key_matrix, LV_PART_MAIN);
    lv_coord_t pleft = lv_obj_get_style_pad_left(key_matrix, LV_PART_MAIN);
    lv_coord_t prow = lv_obj_get_style_pad_row(key_matrix, LV_PART_MAIN);
    lv_coord_t pcol = lv_obj_get_style_pad_column(key_matrix, LV_PART_MAIN);
    lv_coord_t content_w = lv_obj_get_content_width(key_matrix);
    lv_coord_t content_h = lv_obj_get_content_height(key_matrix);

    int rows = KB_IOS_ROWS;
    lv_coord_t max_h_no_gap = content_h - prow * (rows - 1);
    if (max_h_no_gap < 1) max_h_no_gap = 1;

    int yy = ty - km_y;
    int row = -1;
    for (int r = 0; r < rows; r++) {
        int y1 = ptop + (max_h_no_gap * r) / rows + r * prow;
        int y2 = ptop + (max_h_no_gap * (r + 1)) / rows + r * prow;
        if (yy >= y1 && yy < y2) { row = r; break; }
    }
    if (row < 0) return false;

    const kb_key_desc_t * const *page = kb_active_page();
    const kb_key_desc_t *k = page[row];
    int btn_cnt = kb_row_btn_count(row);
    if (btn_cnt <= 0) return false;

    int unit_cnt = 0;
    for (const kb_key_desc_t *q = k; q && q->label; q++) unit_cnt += (q->units ? q->units : 1);
    if (unit_cnt <= 0) return false;

    lv_coord_t max_w_no_gap = content_w - pcol * (btn_cnt - 1);
    if (max_w_no_gap < 1) max_w_no_gap = 1;

    int xx = tx - km_x;
    int unit_pos = 0;
    int sel = 0;
    int col = -1;
    for (int i = 0; k && k->label; i++, k++) {
        int u = k->units ? k->units : 1;
        int x1 = (max_w_no_gap * unit_pos) / unit_cnt + i * pcol + pleft;
        int x2 = (max_w_no_gap * (unit_pos + u)) / unit_cnt + i * pcol + pleft;
        if (xx >= x1 && xx < x2) {
            col = k->spacer ? -1 : sel;
            break;
        }
        if (!k->spacer) sel++;
        unit_pos += u;
    }
    if (col < 0) return false;

    *out_row = row;
    *out_col = col;
    return true;
}

// cache for key button objects to avoid invalid parent during label create
static lv_obj_t *key_btns[5][KEYBOARD_COLUMNS];

static void init_keyboard_styles(void) {
    if (styles_inited) return;
    lv_color_t surface = kb_surface();
    lv_color_t text = kb_text();
    lv_coord_t radius = kb_radius();

    lv_style_init(&style_key_btn);
    lv_style_set_bg_color(&style_key_btn, surface);
    lv_style_set_bg_opa(&style_key_btn, LV_OPA_COVER);
    lv_style_set_border_color(&style_key_btn, text);
    lv_style_set_border_opa(&style_key_btn, LV_OPA_50);
    lv_style_set_border_width(&style_key_btn, 1);
    lv_style_set_radius(&style_key_btn, radius);
    lv_style_set_pad_ver(&style_key_btn, 1);
    lv_style_set_pad_hor(&style_key_btn, 2);

    lv_style_init(&style_key_label);
    lv_style_set_text_color(&style_key_label, text);
    lv_style_set_text_font(&style_key_label, accessibility_get_font_body());

    saved_key_radius = radius;
    styles_inited = true;
}

static const char *keys[][10] = {
    {"Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P"},
    {"A", "S", "D", "F", "G", "H", "J", "K", "L"},
    {"SHIFT", "Z", "X", "C", "V", "B", "N", "M"},
    {",", ".", "\"", " ", "DEL"},
    {"SYM", "Exit", "Done"}
};

static const char *symbols[][10] = {
    {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"},
    {"!", "@", "#", "$", "%", "^", "&", "*", "(", ")"},
    {"-", "_", "=", "+", "[", "]", "{", "}", "\\", "|"},
    {";", ":", "'", "\"", "<", ">", "?", "/"},
    {"ABC", "Exit", "Done"}
};

/* ---------------------------------------------------------------------------
 * iOS-style matrix layout
 *
 * Every row of every page sums to KB_UNITS_PER_ROW (20), so lv_btnmatrix's
 * per-row normalization gives all keys one uniform width. Rows that should be
 * inset (the staggered second row, the #+= punctuation row) pad themselves with
 * HIDDEN spacer entries: lv_btnmatrix counts hidden buttons when laying out and
 * only skips them when drawing and hit-testing, so they reserve exactly the
 * space we want while staying untappable.
 * ------------------------------------------------------------------------ */
/* Non-empty so lv_btnmatrix's map parser does not treat it as a row terminator. */
#define KB_SPACER_LABEL " "

/* Alpha: 10 / 9 (staggered) / 9 / 4 keys, matching the iOS letter layout. */
static const kb_key_desc_t kb_alpha_row1[] = {
    {"Q",2,0},{"W",2,0},{"E",2,0},{"R",2,0},{"T",2,0},
    {"Y",2,0},{"U",2,0},{"I",2,0},{"O",2,0},{"P",2,0},{NULL,0,0}
};
static const kb_key_desc_t kb_alpha_row2[] = {
    {KB_SPACER_LABEL,1,1},
    {"A",2,0},{"S",2,0},{"D",2,0},{"F",2,0},{"G",2,0},
    {"H",2,0},{"J",2,0},{"K",2,0},{"L",2,0},
    {KB_SPACER_LABEL,1,1},{NULL,0,0}
};
static const kb_key_desc_t kb_alpha_row3[] = {
    {"SHIFT",3,0},
    {"Z",2,0},{"X",2,0},{"C",2,0},{"V",2,0},{"B",2,0},{"N",2,0},{"M",2,0},
    {"DEL",3,0},{NULL,0,0}
};
static const kb_key_desc_t kb_alpha_row4[] = {
    {"123",3,0},{"Exit",3,0},{" ",10,0},{"Done",4,0},{NULL,0,0}
};

/* Numbers page (123). */
static const kb_key_desc_t kb_num_row1[] = {
    {"1",2,0},{"2",2,0},{"3",2,0},{"4",2,0},{"5",2,0},
    {"6",2,0},{"7",2,0},{"8",2,0},{"9",2,0},{"0",2,0},{NULL,0,0}
};
static const kb_key_desc_t kb_num_row2[] = {
    {"-",2,0},{"/",2,0},{":",2,0},{";",2,0},{"(",2,0},
    {")",2,0},{"$",2,0},{"&",2,0},{"@",2,0},{"\"",2,0},{NULL,0,0}
};
static const kb_key_desc_t kb_num_row3[] = {
    {KB_SPACER_LABEL,2,1},{"#+=",3,0},
    {".",2,0},{",",2,0},{"?",2,0},{"!",2,0},{"'",2,0},
    {"DEL",3,0},{KB_SPACER_LABEL,2,1},{NULL,0,0}
};
static const kb_key_desc_t kb_num_row4[] = {
    {"ABC",3,0},{"Exit",3,0},{" ",10,0},{"Done",4,0},{NULL,0,0}
};

/* Second symbols page (#+=). ASCII only: the Montserrat builds carry no
 * currency or other extended glyphs. */
static const kb_key_desc_t kb_sym_row1[] = {
    {"[",2,0},{"]",2,0},{"{",2,0},{"}",2,0},{"#",2,0},
    {"%",2,0},{"^",2,0},{"*",2,0},{"+",2,0},{"=",2,0},{NULL,0,0}
};
static const kb_key_desc_t kb_sym_row2[] = {
    {KB_SPACER_LABEL,3,1},
    {"<",2,0},{">",2,0},{"_",2,0},{"\\",2,0},{"|",2,0},{"~",2,0},{"`",2,0},
    {KB_SPACER_LABEL,3,1},{NULL,0,0}
};
static const kb_key_desc_t kb_sym_row3[] = {
    {KB_SPACER_LABEL,2,1},{"123",3,0},
    {".",2,0},{",",2,0},{"?",2,0},{"!",2,0},{"'",2,0},
    {"DEL",3,0},{KB_SPACER_LABEL,2,1},{NULL,0,0}
};
static const kb_key_desc_t kb_sym_row4[] = {
    {"ABC",3,0},{"Exit",3,0},{" ",10,0},{"Done",4,0},{NULL,0,0}
};

static const kb_key_desc_t * const kb_alpha_page[KB_IOS_ROWS] = {
    kb_alpha_row1, kb_alpha_row2, kb_alpha_row3, kb_alpha_row4
};
static const kb_key_desc_t * const kb_num_page[KB_IOS_ROWS] = {
    kb_num_row1, kb_num_row2, kb_num_row3, kb_num_row4
};
static const kb_key_desc_t * const kb_sym_page[KB_IOS_ROWS] = {
    kb_sym_row1, kb_sym_row2, kb_sym_row3, kb_sym_row4
};

static const kb_key_desc_t * const *kb_active_page(void) {
    if (!is_symbols_mode) return kb_alpha_page;
    return kb_sym2 ? kb_sym_page : kb_num_page;
}

static int kb_active_rows(void) {
    return KB_IOS_ROWS;
}

/* Buttons in a row, spacers included (this is the btnmatrix button count). */
static int kb_row_btn_count(int row) {
    if (row < 0 || row >= KB_IOS_ROWS) return 0;
    const kb_key_desc_t * const *page = kb_active_page();
    const kb_key_desc_t *k = page[row];
    int n = 0;
    while (k && k->label) { n++; k++; }
    return n;
}

/* Selectable keys in a row (cursor space; spacers excluded). */
static int kb_row_key_count(int row) {
    if (row < 0 || row >= KB_IOS_ROWS) return 0;
    const kb_key_desc_t * const *page = kb_active_page();
    const kb_key_desc_t *k = page[row];
    int n = 0;
    while (k && k->label) { if (!k->spacer) n++; k++; }
    return n;
}

/* Cursor column (selectable index) -> btnmatrix button id. */
static int kb_cursor_to_btn_id(int row, int col) {
    if (row < 0 || row >= KB_IOS_ROWS || col < 0) return -1;
    const kb_key_desc_t * const *page = kb_active_page();
    int id = 0;
    for (int r = 0; r < row; r++) id += kb_row_btn_count(r);
    const kb_key_desc_t *k = page[row];
    int sel = 0;
    while (k && k->label) {
        if (!k->spacer) {
            if (sel == col) return id;
            sel++;
        }
        id++;
        k++;
    }
    return -1;
}

/* Selectable key count per row, for cursor bounds in the matrix path. */
static const int *kb_matrix_row_lens(void) {
    static int lens[KB_IOS_ROWS];
    for (int r = 0; r < KB_IOS_ROWS; r++) lens[r] = kb_row_key_count(r);
    return lens;
}

static void submit_text();
static void add_char_to_buffer(char c);
static void remove_char_from_buffer();
static void update_input_label();
static void update_key_labels();
static void recreate_keyboard_buttons();
static void get_key_position(int row, int col, int *x, int *width, bool symbols_mode);
static void ensure_valid_cursor(void);
static lv_obj_t *get_key_button_at(int row, int col);
static void apply_selection_highlight(void);
static void activate_selected_key(void);
static void keyboard_build_step(lv_timer_t *t);
static void hide_all_key_buttons(void);
static void reveal_row(int row);
static void destroy_key_buttons(void);

static lv_obj_t *pressed_key_btn = NULL;
static lv_obj_t *selected_key_btn = NULL;
static int cursor_row = 0;
static int cursor_col = 0;
static lv_timer_t *keyboard_build_timer = NULL;
static int keyboard_build_row = 0;
static int keyboard_build_col = 0;
static int keyboard_build_phase = 0; // 0=create buttons, 1=create labels
// track which btnmatrix item is currently focused by joystick to manage CHECKED state
static int joy_focused_btn_id = -1;
// hold-to-invert state for joystick
#define KEYBOARD_HOLD_INVERT_MS 800
static bool joy_holding_letter = false;  // true after hold threshold crossed; label is inverted
static bool joy_pending_letter  = false; // true after press, before threshold or release
static bool joy_inverted_case   = false;
static lv_timer_t *joy_hold_timer = NULL;

static bool is_shift_key(const char *key) {
    return strcmp(key, "SHIFT") == 0;
}
static bool is_del_key(const char *key) {
    return strcmp(key, "DEL") == 0;
}
static bool is_space_key(const char *key) {
    return strcmp(key, " ") == 0;
}
static bool is_symbol_key(const char *key) {
    return strcmp(key, "SYM") == 0;
}
static bool is_exit_key(const char *key) {
    return strcmp(key, "Exit") == 0;
}
static bool is_done_key(const char *key) {
    return strcmp(key, "Done") == 0;
}
static bool is_alpha_key(const char *key) {
    return strlen(key) == 1 && isalpha((unsigned char)key[0]);
}

static const char* get_key_label(const char *key, bool caps, bool symbols_mode) {
    if (is_shift_key(key)) return LV_SYMBOL_UP;
    if (is_del_key(key)) return LV_SYMBOL_BACKSPACE;
    if (!symbols_mode && is_alpha_key(key)) {
        static char buf[2];
        buf[0] = caps ? toupper(key[0]) : tolower(key[0]);
        buf[1] = '\0';
        return buf;
    }
    return key;
}

static void style_shift_key(lv_obj_t *btn, lv_obj_t *label, bool capslock, bool caps) {
    lv_color_t accent = kb_accent();
    lv_color_t sel_fg = kb_sel_text();
    lv_color_t text = kb_text();
    lv_color_t surface = kb_surface();
    if (capslock) {
        lv_obj_set_style_bg_color(btn, accent, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(label, sel_fg, 0);
    } else if (caps) {
        lv_obj_set_style_bg_color(btn, accent, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_70, 0);
        lv_obj_set_style_text_color(label, sel_fg, 0);
    } else {
        lv_obj_set_style_bg_color(btn, surface, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(label, text, 0);
    }
}

static void clear_input_buffer(void) {
    memset(input_buffer, 0, sizeof(input_buffer));
    input_len = 0;
    update_input_label();
}
static void set_placeholder_text(const char *text) {
    if (text && strlen(text) <= sizeof(placeholder) - 1) {
        strncpy(placeholder, text, sizeof(placeholder) - 1);
        placeholder[sizeof(placeholder) - 1] = '\0';
    }
    clear_input_buffer();
}
static const char *(*get_current_keys(void))[10] {
    return is_symbols_mode ? symbols : keys;
}
static const int *get_current_row_lengths(void) {
    return is_symbols_mode ? symbols_row_lengths : row_lengths;
}

static void hide_all_key_buttons(void) {
#if defined(CONFIG_USE_TOUCHSCREEN) || defined(CONFIG_USE_JOYSTICK)
    if (!root) return;
    uint32_t child_count = lv_obj_get_child_cnt(root);
    for (uint32_t i = 1; i < child_count; i++) {
        lv_obj_t *child = lv_obj_get_child(root, i);
        if (child) {
            lv_obj_add_flag(child, LV_OBJ_FLAG_HIDDEN);
        }
    }
#endif
}

static void reveal_row(int row) {
#if defined(CONFIG_USE_TOUCHSCREEN) || defined(CONFIG_USE_JOYSTICK)
    if (!root || row < 0 || row >= num_rows) return;
    const int *row_lens = get_current_row_lengths();
    int offset = 0;
    for (int r = 0; r < row; r++) offset += max_row_lengths[r];
    for (int c = 0; c < max_row_lengths[row]; c++) {
        int child_idx = 1 + offset + c;
        uint32_t cnt = lv_obj_get_child_cnt(root);
        if ((uint32_t)child_idx >= cnt) continue;
        lv_obj_t *key_btn = lv_obj_get_child(root, child_idx);
        if (!key_btn) continue;
        if (c < row_lens[row]) {
            lv_obj_clear_flag(key_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(key_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }
#endif
}

static lv_obj_t* create_key_button(lv_obj_t *parent, int x, int y, int w, int h, const char *label_text) {
    init_keyboard_styles();
    lv_obj_t *btn = lv_obj_create(parent);
    if (!btn) return NULL;
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_pos(btn, x, y);
    lv_obj_add_style(btn, &style_key_btn, 0);

    // try to reuse an existing label child if present to avoid repeated allocations
    lv_obj_t *label = lv_obj_get_child(btn, 0);
    if (!label) {
        label = lv_label_create(btn);
        if (label) {
            lv_obj_add_style(label, &style_key_label, 0);
            lv_obj_center(label);
        }
    } else {
        // ensure style and centering are applied when reusing
        lv_obj_add_style(label, &style_key_label, 0);
        lv_obj_center(label);
    }

    if (label) {
        // keys array contains static string literals; use static set to avoid allocations
        if (label_text) lv_label_set_text_static(label, label_text);
        else lv_label_set_text_static(label, "");
    }
    return btn;
}

static void submit_text() {
    if (submit_callback) {
        char submitted_text[sizeof(input_buffer)];
        KeyboardSubmitCallback callback = submit_callback;
        memcpy(submitted_text, input_buffer, sizeof(submitted_text));
        memset(input_buffer, 0, sizeof(input_buffer));
        input_len = 0;
        update_input_label();
        /* Submit callbacks own the destination view.  Queuing a Back here
         * races callbacks such as the terminal callback, which must restore
         * and refresh its view before executing the command. */
        callback(submitted_text);
    } else if (input_len > 0) {
        terminal_set_return_view(&options_menu_view);
        display_manager_go_back();
        vTaskDelay(pdMS_TO_TICKS(10));
        simulateCommand(input_buffer);
        memset(input_buffer, 0, sizeof(input_buffer));
        input_len = 0;
        update_input_label();
    }
}

static void emit_immediate_char(char c) {
    if (immediate_callback) immediate_callback(c);
}

static void add_char_to_buffer(char c) {
    if (input_len < sizeof(input_buffer) - 1) {
        // Use capslock or SHIFT if active, otherwise lowercase
        bool use_caps = is_capslock || is_caps;
        if (isalpha((unsigned char)c)) {
            c = use_caps ? (char)toupper((unsigned char)c) : (char)tolower((unsigned char)c);
            input_buffer[input_len++] = c;
        } else {
            input_buffer[input_len++] = c; // Add non-alphabetic characters unchanged
        }
        input_buffer[input_len] = '\0';
        update_input_label();
        emit_immediate_char(c);
    }
    if (is_caps && !is_capslock) {
        is_caps = false; // Reset to lowercase after any key press unless capslock is on
        update_key_labels(); // Update key labels to reflect the change
        #if defined(CONFIG_USE_ENCODER) && !defined(CONFIG_USE_JOYSTICK)
        encoder_uppercase = (is_capslock || is_caps);
        if (encoder_cont && !encoder_sym_mode) {
            for (int j = 0; j < encoder_item_count; j++) {
                const char *t = encoder_items[j];
                if (strlen(t) == 1 && isalpha((unsigned char)t[0])) {
                    char tmp2[2] = { encoder_uppercase ? toupper((unsigned char)t[0]) : tolower((unsigned char)t[0]), '\0' };
                    lv_label_set_text(encoder_labels[j], tmp2);
                }
            }
        }
        #endif
    }
}

static void add_char_to_buffer_raw(char c) {
    if (input_len < sizeof(input_buffer) - 1) {
        input_buffer[input_len++] = c;
        input_buffer[input_len] = '\0';
        update_input_label();
        emit_immediate_char(c);
    }
}

static void remove_char_from_buffer() {
    if (input_len > 0) {
        input_buffer[--input_len] = '\0';
        update_input_label();
        emit_immediate_char('\b');
    }
}

static void update_input_label() {
    if (!input_label) return;
    if (input_len == 0) {
        lv_label_set_text(input_label, placeholder);
    } else {
        lv_label_set_text(input_label, input_buffer);
    }
    /* Where there is spare room, size the field to the wrapped text so it grows
     * as more is typed and shrinks again on backspace, capped at the gap above
     * the keys. Measured by LVGL rather than predicted, so wrapping and padding
     * are accounted for exactly. */
    if (kb_field_max_h > kb_field_base_h) {
        lv_obj_set_height(input_label, LV_SIZE_CONTENT);
        lv_obj_update_layout(input_label);
        lv_coord_t h = lv_obj_get_height(input_label);
        if (h < kb_field_base_h) h = kb_field_base_h;
        if (h > kb_field_max_h) h = kb_field_max_h;
        lv_obj_set_height(input_label, h);
    }
}

static void update_key_labels() {
#if defined(CONFIG_USE_TOUCHSCREEN) || defined(CONFIG_USE_JOYSTICK)
    // touch/joystick builds use btnmatrix; rebuild its map/texts
    if (key_matrix) {
        build_key_matrix();
        return;
    }
#endif
#if defined(CONFIG_USE_TOUCHSCREEN) || defined(CONFIG_USE_JOYSTICK)
    if (!root) return;

    int key_index = 0;
    uint32_t child_count = lv_obj_get_child_cnt(root);
    const char *(*current_keys)[10] = get_current_keys();
    const int *current_row_lengths = get_current_row_lengths();

    for (int r = 0; r < num_rows; r++) {
        for (int c = 0; c < max_row_lengths[r]; c++) {
            int child_idx = 1 + key_index;
            if (child_idx < child_count) {
                lv_obj_t *key_btn = lv_obj_get_child(root, child_idx);
                if (key_btn) {
                    lv_obj_t *key_label = lv_obj_get_child(key_btn, 0);
                    if (key_label) {
                        if (c < current_row_lengths[r]) {
                            const char* key_text = current_keys[r][c];
                            lv_label_set_text(key_label, get_key_label(key_text, is_caps, is_symbols_mode));
                            // Style SHIFT key
                            if (is_shift_key(key_text)) {
                                style_shift_key(key_btn, key_label, is_capslock, is_caps);
                            }
                            lv_obj_clear_flag(key_btn, LV_OBJ_FLAG_HIDDEN);
                        } else {
                            lv_label_set_text(key_label, "");
                            lv_obj_add_flag(key_btn, LV_OBJ_FLAG_HIDDEN);
                        }
                        lv_obj_set_style_text_color(key_label, kb_text(), 0);
                    }
                }
            }
            key_index++;
        }
    }
#endif
#if defined(CONFIG_USE_TOUCHSCREEN) || defined(CONFIG_USE_JOYSTICK)
    if (!keyboard_build_timer) apply_selection_highlight();
#endif
}

static void recreate_keyboard_buttons() {
#if defined(CONFIG_USE_TOUCHSCREEN) || defined(CONFIG_USE_JOYSTICK)
    if (!root) return;
    // For touch/joystick builds, just rebuild the btnmatrix map instead of recreating objects
    build_key_matrix();
    return;
#endif
#if defined(CONFIG_USE_TOUCHSCREEN) || defined(CONFIG_USE_JOYSTICK)
    if (!root) return;

    lvgl_timer_del_safe(&keyboard_build_timer);
    // hide the root during incremental rebuild to avoid heavy draw churn
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
    destroy_key_buttons();
    keyboard_build_phase = 0;
    hide_all_key_buttons();
    keyboard_build_row = 0;
    keyboard_build_col = 0;
    keyboard_build_timer = lv_timer_create(keyboard_build_step, 10, NULL);
#else
    lvgl_timer_del_safe(&keyboard_build_timer);
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
    int screen_height = LV_VER_RES;
    int status_bar_height = GUI_STATUS_BAR_HEIGHT;
    int display_height = keyboard_display_height();
    int padding = keyboard_layout_padding();
    int keys_start_y = status_bar_height + display_height + padding * 2;
    int keys_area_height = screen_height - keys_start_y;
    int key_height = (keys_area_height / num_rows) - 4;
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
    if (key_height > GUI_CONTROL_H) key_height = GUI_CONTROL_H;
#endif
    int key_y = keys_start_y;

    int max_keys = 0;
    for (int r = 0; r < num_rows; r++) {
        int keys_in_row = row_lengths[r] > symbols_row_lengths[r] ? row_lengths[r] : symbols_row_lengths[r];
        if (keys_in_row > max_keys) max_keys = keys_in_row;
    }

    for (int r = 0; r < num_rows; r++) {
        int total_key_width = LV_HOR_RES - (padding * 2);
        int current_row_length = max_row_lengths[r];
        int key_width = total_key_width / current_row_length;
        
        const int *row_lens = is_symbols_mode ? symbols_row_lengths : row_lengths;
        int actual_len = row_lens[r];
        int special_count = 0;
        if (!is_symbols_mode) {
            for (int i = 0; i < actual_len; i++) {
                const char *txt = keys[r][i];
                if (strcmp(txt, "SHIFT") == 0 || strcmp(txt, "DEL") == 0 || strcmp(txt, " ") == 0) {
                    special_count++;
                }
            }
        }
        int extra_space = special_count * (key_width / 2);
        int total_keys_width = actual_len * key_width + extra_space;
        int blank_space = total_key_width - total_keys_width;
        int key_x = padding + blank_space / 2;
        
        for (int c = 0; c < current_row_length; c++) {
            int current_key_width = key_width;
            
            // adjust for wider SHIFT and DEL buttons
            if (!is_symbols_mode && c < row_lens[r]) {
                const char *txt = keys[r][c];
                if (strcmp(txt, "SHIFT") == 0 || strcmp(txt, "DEL") == 0 || strcmp(txt, " ") == 0) {
                    current_key_width += key_width / 2;
                }
            }
            
            int key_x, key_w;
            get_key_position(r, c, &key_x, &key_w, is_symbols_mode);
            lv_obj_t *key_btn = create_key_button(root, key_x, key_y, key_w - 2, key_height, "");
            if (key_btn) {
                lv_obj_t *key_label = lv_obj_get_child(key_btn, 0);
                const char *(*current_keys)[KEYBOARD_COLUMNS] = is_symbols_mode ? symbols : keys;
                if (c < row_lens[r]) {
                    const char* key_text = current_keys[r][c];
                    if (strcmp(key_text, "SHIFT") == 0) {
                        lv_label_set_text(key_label, LV_SYMBOL_UP);
                    } else if (!is_symbols_mode && strlen(key_text) == 1) {
                        char new_text[2];
                        if (isalpha((unsigned char)key_text[0])) {
                            new_text[0] = is_caps ? toupper(key_text[0]) : tolower(key_text[0]);
                        } else {
                            new_text[0] = key_text[0];
                        }
                        new_text[1] = '\0';
                        lv_label_set_text(key_label, new_text);
                    } else {
                        lv_label_set_text(key_label, key_text);
                    }
                } else {
                    lv_label_set_text(key_label, "");
                    lv_obj_add_flag(key_btn, LV_OBJ_FLAG_HIDDEN);
                }
            }
            key_x += current_key_width;
        }
        key_y += key_height + 2;
    }
    lv_obj_clear_flag(root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(root);
#endif
#if defined(CONFIG_USE_TOUCHSCREEN) || defined(CONFIG_USE_JOYSTICK)
    if (!keyboard_build_timer) {
        apply_selection_highlight();
    }
#endif
}

static void keyboard_build_step(lv_timer_t *t) {
#if defined(CONFIG_USE_TOUCHSCREEN) || defined(CONFIG_USE_JOYSTICK)
    if (!root || !lv_obj_is_valid(root)) { lv_timer_del(t); keyboard_build_timer = NULL; return; }

    int screen_height = LV_VER_RES;
    int status_bar_height = GUI_STATUS_BAR_HEIGHT;
    int display_height = keyboard_display_height();
    int padding = keyboard_layout_padding();
    int keys_start_y = status_bar_height + display_height + padding * 2;
    int keys_area_height = screen_height - keys_start_y;
    int key_height = (keys_area_height / num_rows) - 4;
#ifdef CONFIG_CROWPANEL_ADVANCED_P4
    if (key_height > GUI_CONTROL_H) key_height = GUI_CONTROL_H;
#endif
    int built = 0;
    const int batch = 4;
    // timing for profiling heavy steps
    int64_t t0 = esp_timer_get_time();
    while (built < batch && keyboard_build_row < num_rows) {
        int r = keyboard_build_row;
        int key_y = keys_start_y + r * (key_height + 2);
        const char *(*current_keys)[10] = get_current_keys();
        const int *row_lens = get_current_row_lengths();
        int c = keyboard_build_col;
        int key_x, key_w;
        get_key_position(r, c, &key_x, &key_w, is_symbols_mode);
        lv_obj_t *key_btn = key_btns[r][c];
        if (key_btn && !lv_obj_is_valid(key_btn)) key_btn = NULL;
        if (key_btn && lv_obj_get_parent(key_btn) != root) key_btn = NULL;
        if (keyboard_build_phase == 0) {
            // phase 0: create buttons only (no labels yet)
            if (!key_btn) {
                key_btn = lv_obj_create(root);
                if (!key_btn) break;
                lv_obj_remove_style_all(key_btn);
                lv_obj_add_style(key_btn, &style_key_btn, 0);
                key_btns[r][c] = key_btn;
            }
            lv_obj_set_size(key_btn, key_w - 2, key_height);
            lv_obj_set_pos(key_btn, key_x, key_y);
            lv_obj_add_flag(key_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            // phase 1: attach/create labels and set text
            if (!key_btn) {
                // Should have been created in phase 0, but create defensively
                key_btn = lv_obj_create(root);
                if (!key_btn) break;
                lv_obj_remove_style_all(key_btn);
                lv_obj_add_style(key_btn, &style_key_btn, 0);
                lv_obj_set_size(key_btn, key_w - 2, key_height);
                lv_obj_set_pos(key_btn, key_x, key_y);
                lv_obj_add_flag(key_btn, LV_OBJ_FLAG_HIDDEN);
                key_btns[r][c] = key_btn;
            }
            if (!lv_obj_is_valid(key_btn) || lv_obj_get_parent(key_btn) != root) {
                // parent chain changed; skip this item safely
                built++;
                keyboard_build_col++;
                if (keyboard_build_col >= max_row_lengths[r]) { keyboard_build_col = 0; keyboard_build_row++; }
                continue;
            }
            lv_obj_t *key_label = lv_obj_get_child(key_btn, 0);
            if (!key_label) {
                key_label = lv_label_create(key_btn);
                if (key_label) {
                    lv_obj_add_style(key_label, &style_key_label, 0);
                    lv_obj_center(key_label);
                }
            }
            if (key_label) {
                if (c < row_lens[r]) {
                    const char* key_text = current_keys[r][c];
                    lv_label_set_text(key_label, get_key_label(key_text, is_caps, is_symbols_mode));
                    if (is_shift_key(key_text)) {
                        style_shift_key(key_btn, key_label, is_capslock, is_caps);
                    }
                } else {
                    lv_label_set_text(key_label, "");
                    lv_obj_add_flag(key_btn, LV_OBJ_FLAG_HIDDEN);
                }
            }
        }
        built++;
        keyboard_build_col++;
        if (keyboard_build_col >= max_row_lengths[r]) {
            keyboard_build_col = 0;
            // defer reveal until after build completes (root is hidden anyway)
            keyboard_build_row++;
        }
    }
    if (keyboard_build_row >= num_rows) {
        if (keyboard_build_phase == 0) {
            // move to phase 1: create labels
            keyboard_build_phase = 1;
            keyboard_build_row = 0;
            keyboard_build_col = 0;
        } else {
            // finished both phases
            lv_timer_del(t);
            keyboard_build_timer = NULL;
            cursor_row = 0;
            cursor_col = 0;
            apply_selection_highlight();

            // unhide root after build completes
            if (root) lv_obj_clear_flag(root, LV_OBJ_FLAG_HIDDEN);
            // restore radii after initial paint to re-enable rounded corners
            if (radius_override_active) {
                lv_style_set_radius(&style_key_btn, saved_key_radius);
                if (input_label) lv_obj_set_style_radius(input_label, saved_input_label_radius, 0);
                radius_override_active = false;
            }
            lv_obj_invalidate(root);
            int64_t dt = esp_timer_get_time() - t0;
            if (dt > 5000) {
                ESP_LOGW(TAG, "keyboard_build total took %lld us", dt);
            }
        }
    }
#else
    lv_timer_del(t);
    keyboard_build_timer = NULL;
#endif
}

#if defined(CONFIG_USE_ENCODER) && !defined(CONFIG_USE_JOYSTICK)
static void encoder_set_item_text(int i) {
    const char *txt = encoder_items[i];
    if (!encoder_sym_mode && strlen(txt) == 1 && isalpha((unsigned char)txt[0])) {
        char tmp[2] = { encoder_uppercase ? (char)toupper((unsigned char)txt[0]) : (char)tolower((unsigned char)txt[0]), '\0' };
        lv_label_set_text(encoder_labels[i], tmp);
    } else {
        lv_label_set_text(encoder_labels[i], txt);
    }
}

static void encoder_set_item_style(int i) {
    if (i == encoder_sel_idx) {
        lv_obj_set_style_text_color(encoder_labels[i], kb_text(), 0);
        lv_obj_set_style_text_font(encoder_labels[i], accessibility_get_font_title(), 0);
    } else {
        lv_obj_set_style_text_color(encoder_labels[i], lv_color_hex(theme_palette_get_text_muted(settings_get_menu_theme(&G_Settings))), 0);
        lv_obj_set_style_text_font(encoder_labels[i], accessibility_get_font_body(), 0);
    }
}

static void encoder_position_item(int i) {
    int cont_h = lv_obj_get_height(encoder_cont);
    int slot_center_x = encoder_offset_x + i * encoder_item_spacing;
    int lbl_w = lv_obj_get_width(encoder_labels[i]);
    int lbl_h = lv_obj_get_height(encoder_labels[i]);
    if (lbl_w <= 0) lbl_w = encoder_item_spacing / 2;
    if (lbl_h <= 0) lbl_h = cont_h;
    lv_obj_set_pos(encoder_labels[i],
        slot_center_x - lbl_w / 2,
        (cont_h - lbl_h) / 2);
}

static void encoder_position_selector(void) {
    if (!encoder_selector) return;
    int cont_h = lv_obj_get_height(encoder_cont);
    int slot_center_x = encoder_offset_x + encoder_sel_idx * encoder_item_spacing;
    lv_obj_set_pos(encoder_selector,
        slot_center_x - encoder_selector_w / 2,
        cont_h - encoder_selector_h - 2);
}

static int encoder_scroll_x_for_index(int i) {
    return encoder_offset_x + i * encoder_item_spacing - encoder_screen_width / 2;
}
#endif

static void destroy_key_buttons(void) {
#if defined(CONFIG_USE_TOUCHSCREEN) || defined(CONFIG_USE_JOYSTICK)
    for (int r = 0; r < num_rows; r++) {
        for (int c = 0; c < KEYBOARD_COLUMNS; c++) {
            lv_obj_t *btn = key_btns[r][c];
            lvgl_obj_del_safe(&btn);
            key_btns[r][c] = NULL;
        }
    }
    pressed_key_btn = NULL;
    selected_key_btn = NULL;
#endif
}

static void keyboard_create() {
    is_caps = start_with_caps;
    start_with_caps = true; /* reset so next open gets default (caps) */
    is_symbols_mode = false;
    kb_sym2 = false;
    input_len = 0;
    memset(input_buffer, 0, sizeof(input_buffer));
    if (has_pending_initial_text) {
        size_t n = strnlen(pending_initial_text, sizeof(pending_initial_text) - 1);
        if (n > 0) {
            memcpy(input_buffer, pending_initial_text, n + 1);
            input_len = (int)n;
        }
        has_pending_initial_text = false;
        memset(pending_initial_text, 0, sizeof(pending_initial_text));
    }

    int screen_height = LV_VER_RES;
    int status_bar_height = GUI_STATUS_BAR_HEIGHT;

    root = gui_screen_create_root(NULL, "Keyboard", kb_bg(), LV_OPA_COVER);
    keyboard_view.root = root;
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_HOR_RES, screen_height);
    lv_obj_set_style_bg_color(root, kb_bg(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    /* Field and keys share one geometry source so the fraction of the screen
     * given to text entry stays proportional at every resolution. */
    kb_metrics_t m;
    kb_compute_metrics(&m, LV_HOR_RES, screen_height, status_bar_height);
    lv_color_t text = kb_text();
    lv_color_t surface = kb_surface();
    lv_coord_t radius = kb_radius();
    input_label = lv_label_create(root);
    lv_obj_set_size(input_label, m.matrix_w, m.field_h);
    lv_obj_set_style_bg_color(input_label, surface, 0);
    lv_obj_set_style_bg_opa(input_label, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(input_label, text, 0);
    lv_obj_set_style_pad_hor(input_label, m.pad_h, 0);
    lv_obj_set_style_pad_ver(input_label, kb_clamp_int(m.field_h / 6, 2, GUI_SAFEAREA_VER), 0);
    lv_obj_set_style_radius(input_label, radius, 0);
    lv_obj_set_style_border_width(input_label, 1, 0);
    lv_obj_set_style_border_color(input_label, text, 0);
    lv_obj_set_style_border_opa(input_label, LV_OPA_30, 0);
    lv_obj_set_pos(input_label, m.pad_h, status_bar_height + GUI_SAFEAREA_VER);
    /* When the field can grow, wrap so more text stays visible as it expands;
     * otherwise keep the single scrolling line. */
    lv_label_set_long_mode(input_label,
                           m.field_max_h > m.field_h ? LV_LABEL_LONG_WRAP
                                                     : LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(input_label, LV_TEXT_ALIGN_LEFT, 0);
    kb_field_base_h = m.field_h;
    kb_field_max_h = m.field_max_h;
    update_input_label();

    // ensure styles are initialized so we can temporarily zero radius
    init_keyboard_styles();
    // hide root while building to avoid heavy invalidation churn
    lv_obj_add_flag(root, LV_OBJ_FLAG_HIDDEN);
    // zero out radii during build to bypass rounded-rect mask paths
    saved_key_radius = kb_radius();
    saved_input_label_radius = kb_radius();
    lv_style_set_radius(&style_key_btn, 0);
    lv_obj_set_style_radius(input_label, 0, 0);
    radius_override_active = true;

    // reset build state and key button cache
    for (int rr = 0; rr < num_rows; rr++) {
        for (int cc = 0; cc < KEYBOARD_COLUMNS; cc++) {
            key_btns[rr][cc] = NULL;
        }
    }
    keyboard_build_phase = 0;

#if defined(CONFIG_USE_TOUCHSCREEN) || defined(CONFIG_USE_JOYSTICK)
    // use a single btnmatrix to render the keyboard for touch/joystick builds
    recreate_keyboard_buttons();
#elif defined(CONFIG_USE_ENCODER) && !defined(CONFIG_USE_JOYSTICK)
    int display_height = keyboard_display_height();
    int padding = keyboard_layout_padding();
    encoder_cont = lv_obj_create(root);
    lv_obj_remove_style_all(encoder_cont);
    lv_obj_set_size(encoder_cont, LV_HOR_RES, display_height);
    lv_obj_set_pos(encoder_cont, 0, status_bar_height + display_height + padding);
    lv_obj_set_style_bg_opa(encoder_cont, LV_OPA_TRANSP, 0);
    // initialize encoder items and metrics
    encoder_items = encoder_alpha_items;
    encoder_item_count = encoder_alpha_count;
    encoder_sym_mode = false;
    encoder_sel_idx = 0;
    encoder_uppercase = true;
    encoder_screen_width = LV_HOR_RES;
    // Slot width: wider than the container so the title-font's widest glyph
    // (e.g. "W", "M") still leaves a visible gap to its neighbors.
    encoder_item_spacing = LV_MAX(display_height + 8, 48);
    // First slot's center sits one half-spacing from the container's left edge.
    encoder_offset_x = encoder_item_spacing / 2;
    lv_obj_set_scroll_dir(encoder_cont, LV_DIR_LEFT | LV_DIR_RIGHT);
    lv_obj_set_scrollbar_mode(encoder_cont, LV_SCROLLBAR_MODE_OFF);
    // pad right so the last slot can scroll all the way to center
    lv_obj_set_style_pad_right(encoder_cont, encoder_screen_width, 0);

    // selection indicator: thin underline beneath the active slot
    encoder_selector_w = LV_MAX(display_height / 3, 14);
    encoder_selector_h = 2;
    encoder_selector = lv_obj_create(encoder_cont);
    lv_obj_remove_style_all(encoder_selector);
    lv_obj_set_style_bg_color(encoder_selector, kb_accent(), 0);
    lv_obj_set_style_bg_opa(encoder_selector, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(encoder_selector, 1, 0);
    lv_obj_set_size(encoder_selector, encoder_selector_w, encoder_selector_h);

    // create each label with its final text + style
    for (int i = 0; i < encoder_item_count; i++) {
        encoder_labels[i] = lv_label_create(encoder_cont);
        encoder_set_item_text(i);
        encoder_set_item_style(i);
    }
    // flush layout so lv_obj_get_width/height return real values
    lv_obj_update_layout(encoder_cont);
    // position every label centered within its slot
    for (int i = 0; i < encoder_item_count; i++) {
        encoder_position_item(i);
    }
    encoder_position_selector();
    lv_obj_scroll_to_x(encoder_cont, encoder_scroll_x_for_index(encoder_sel_idx), LV_ANIM_OFF);

    // ensure encoder builds unhide the root and restore radii
    if (radius_override_active) {
        lv_style_set_radius(&style_key_btn, saved_key_radius);
        if (input_label) lv_obj_set_style_radius(input_label, saved_input_label_radius, 0);
        radius_override_active = false;
    }
    lv_obj_clear_flag(root, LV_OBJ_FLAG_HIDDEN);
#else
    // non-touch, non-encoder devices (e.g., cardputer keyboard): unhide and restore radii
    if (radius_override_active) {
        lv_style_set_radius(&style_key_btn, saved_key_radius);
        if (input_label) lv_obj_set_style_radius(input_label, saved_input_label_radius, 0);
        radius_override_active = false;
    }
    lv_obj_clear_flag(root, LV_OBJ_FLAG_HIDDEN);
#endif
    
    display_manager_add_status_bar("Keyboard");

#if defined(CONFIG_USE_TOUCHSCREEN) || defined(CONFIG_USE_JOYSTICK)
    if (!keyboard_build_timer) {
        update_key_labels();
        apply_selection_highlight();
    }
#else
    update_key_labels();
#endif
}

static void keyboard_destroy() {
    /* Hoisted outside the root guard: a partially-constructed view must not
     * leak timers when create() aborts early. */
    lvgl_timer_del_safe(&keyboard_build_timer);
    if (joy_hold_timer) { lv_timer_del(joy_hold_timer); joy_hold_timer = NULL; }
    if (keyboard_view.root) {
        destroy_key_buttons();
        lvgl_obj_del_safe(&keyboard_view.root);
        root = NULL;
        key_matrix = NULL;
        shift_btn_id = -1;
        pressed_btn_id = -1;
        input_label = NULL;
        kb_field_base_h = 0;
        kb_field_max_h = 0;
        submit_callback = NULL;
        immediate_callback = NULL;
        input_len = 0;
        input_buffer[0] = '\0';
        is_symbols_mode = false;
        kb_sym2 = false;
        is_caps = true;
        is_capslock = false;
#if defined(CONFIG_USE_ENCODER) && !defined(CONFIG_USE_JOYSTICK)
        encoder_cont = NULL;
        encoder_selector = NULL;
        encoder_item_count = 0;
        encoder_screen_width = 0;
        encoder_item_spacing = 0;
        encoder_sym_mode = false;
#endif
        selected_key_btn = NULL;
        cursor_row = 0;
        cursor_col = 0;
        joy_focused_btn_id = -1;
        joy_holding_letter = false;
        joy_pending_letter  = false;
        joy_inverted_case   = false;
    }
}

// Fires once after KEYBOARD_HOLD_INVERT_MS while select is still held on a letter.
// Flips the label to inverted case; release will then type the inverted character.
static void keyboard_hold_invert_cb(lv_timer_t *t) {
    (void)t;
    joy_hold_timer = NULL; // one-shot; LVGL auto-deletes after repeat_count reaches 0
    if (!joy_pending_letter || !key_matrix) return;
    joy_pending_letter = false;
    joy_holding_letter = true;
    build_key_matrix();
    // Re-apply focus highlight on the held key
    int id = kb_cursor_to_btn_id(cursor_row, cursor_col);
    if (id < 0) return;
    lv_btnmatrix_set_selected_btn(key_matrix, id);
    lv_btnmatrix_set_btn_ctrl(key_matrix, id, LV_BTNMATRIX_CTRL_CHECKED);
    joy_focused_btn_id = id;
}

#if defined(CONFIG_USE_ENCODER) && !defined(CONFIG_USE_JOYSTICK)
// Move the encoder selection by `dir` and, when `activate` is set, apply the
// selected glyph/action. Shared by the rotary encoder and the AtomS3R single
// button (tap = move, hold = activate).
static void encoder_apply(int dir, bool activate) {
        if (!encoder_cont) return;
        int prev = encoder_sel_idx;
        encoder_sel_idx = (encoder_sel_idx + dir + encoder_item_count) % encoder_item_count;
        lv_obj_scroll_to_x(encoder_cont, encoder_scroll_x_for_index(encoder_sel_idx), LV_ANIM_OFF);
        if (prev >= 0 && prev < encoder_item_count) {
            encoder_set_item_style(prev);
            // re-position the de-selected label: the body font may have a
            // different measured width than the title font it had moments ago,
            // so it needs to be re-centered in its slot.
            encoder_position_item(prev);
        }
        encoder_set_item_style(encoder_sel_idx);
        encoder_position_item(encoder_sel_idx);
        encoder_position_selector();
        if (activate) {
            const char *sel = encoder_items[encoder_sel_idx];
            if(strcmp(sel, "Aa") == 0) {
                // toggle case
                encoder_uppercase = !encoder_uppercase;
                // refresh text on every alpha item
                for (int j = 0; j < encoder_item_count; j++) {
                    encoder_set_item_text(j);
                }
                lv_obj_update_layout(encoder_cont);
                for (int j = 0; j < encoder_item_count; j++) {
                    encoder_position_item(j);
                }
                return;
            }
            if (!encoder_sym_mode && strcmp(sel, "SYM") == 0) {
                // switch to symbol mode
                for (int i = 0; i < encoder_item_count; i++) lv_obj_del(encoder_labels[i]);
                encoder_items = encoder_sym_items;
                encoder_item_count = encoder_sym_count;
                encoder_sym_mode = true;
                encoder_sel_idx = 0;
                for (int i = 0; i < encoder_item_count; i++) {
                    encoder_labels[i] = lv_label_create(encoder_cont);
                    encoder_set_item_text(i);
                    encoder_set_item_style(i);
                }
                lv_obj_update_layout(encoder_cont);
                for (int i = 0; i < encoder_item_count; i++) {
                    encoder_position_item(i);
                }
                encoder_position_selector();
                lv_obj_scroll_to_x(encoder_cont, encoder_scroll_x_for_index(encoder_sel_idx), LV_ANIM_OFF);
            } else if (encoder_sym_mode && strcmp(sel, "ABC") == 0) {
                // switch back to alpha mode
                for (int i = 0; i < encoder_item_count; i++) lv_obj_del(encoder_labels[i]);
                encoder_items = encoder_alpha_items;
                encoder_item_count = encoder_alpha_count;
                encoder_sym_mode = false;
                encoder_sel_idx = 0;
                for (int i = 0; i < encoder_item_count; i++) {
                    encoder_labels[i] = lv_label_create(encoder_cont);
                    encoder_set_item_text(i);
                    encoder_set_item_style(i);
                }
                lv_obj_update_layout(encoder_cont);
                for (int i = 0; i < encoder_item_count; i++) {
                    encoder_position_item(i);
                }
                encoder_position_selector();
                lv_obj_scroll_to_x(encoder_cont, encoder_scroll_x_for_index(encoder_sel_idx), LV_ANIM_OFF);
            } else if (strcmp(sel, "SPA") == 0) {
                add_char_to_buffer(' ');
            } else if (strcmp(sel, LV_SYMBOL_BACKSPACE) == 0) {
                remove_char_from_buffer();
            } else if (strcmp(sel, LV_SYMBOL_NEW_LINE) == 0) {
                submit_text();
            } else {
                char c = sel[0];
                if (!encoder_sym_mode && isalpha((unsigned char)c)) {
                    c = encoder_uppercase ? (char)toupper((unsigned char)c) : (char)tolower((unsigned char)c);
                }
                // bypass global is_caps so encoder "Aa" accurately controls case
                add_char_to_buffer_raw(c);
            }
        }
}
#endif

static void handle_hardware_button_press_keyboard(InputEvent *event) {
#if defined(CONFIG_USE_ENCODER) && !defined(CONFIG_USE_JOYSTICK)
    if (event->type == INPUT_TYPE_ENCODER) {
        encoder_apply(event->data.encoder.direction, event->data.encoder.button);
        return;
    }
#ifdef CONFIG_IS_ATOMS3R
    // AtomS3R single button -> encoder actions. The button state machine emits
    // joystick events: single tap = Down(4), double tap = Up(2), hold = Select(1).
    // Triple tap arrives separately as INPUT_TYPE_EXIT_BUTTON (handled below).
    if (event->type == INPUT_TYPE_JOYSTICK) {
        if (!encoder_cont || !event->data.joystick_pressed) return;
        switch (event->data.joystick_index) {
            case 4: encoder_apply(+1, false); break; // tap: next glyph
            case 2: encoder_apply(-1, false); break; // double tap: previous glyph
            case 1: encoder_apply(0, true);   break; // hold: type selected glyph
            default: break;
        }
        return;
    }
#endif
#endif
    if (event->type == INPUT_TYPE_JOYSTICK) {
        int button = event->data.joystick_index;
        bool pressed = event->data.joystick_pressed;
        /* The btnmatrix path navigates over selectable keys only, so layout
         * spacers are not part of the cursor space. */
        const int *row_lens = key_matrix ? kb_matrix_row_lens() : get_current_row_lengths();
        int rows = key_matrix ? kb_active_rows() : num_rows;

        // Navigation buttons (only on press, not release)
        if (pressed) {
            if (button == 0) { // left
                if (cursor_col > 0) cursor_col--; else cursor_col = row_lens[cursor_row] - 1;
            } else if (button == 3) { // right
                if (cursor_col < row_lens[cursor_row] - 1) cursor_col++; else cursor_col = 0;
            } else if (button == 2) { // up
                cursor_row = (cursor_row > 0) ? cursor_row - 1 : rows - 1;
                if (cursor_col >= row_lens[cursor_row]) cursor_col = row_lens[cursor_row] - 1;
            } else if (button == 4) { // down
#ifdef CONFIG_IS_ATOMS3R
                // With one button, make each short press advance linearly
                // through every key instead of requiring Right navigation.
                if (cursor_col + 1 < row_lens[cursor_row]) {
                    cursor_col++;
                } else {
                    cursor_row = (cursor_row + 1) % rows;
                    cursor_col = 0;
                }
#else
                cursor_row = (cursor_row < rows - 1) ? cursor_row + 1 : 0;
                if (cursor_col >= row_lens[cursor_row]) cursor_col = row_lens[cursor_row] - 1;
#endif
            }
        }

        ensure_valid_cursor();

#if defined(CONFIG_USE_TOUCHSCREEN) || defined(CONFIG_USE_JOYSTICK)
        if (key_matrix) {
            // Map (cursor_row, cursor_col) to btnmatrix button index, skipping spacers
            int id = kb_cursor_to_btn_id(cursor_row, cursor_col);
            if (id < 0) return;
            
            // Handle button 1 (select) with hold-to-invert for letters
            if (button == 1) {
                const char *txt = lv_btnmatrix_get_btn_text(key_matrix, id);
                bool is_letter = txt && strlen(txt) == 1 && isalpha((unsigned char)txt[0]);
                
                if (pressed) {
                    // On press: update focus highlight
                    lv_btnmatrix_set_selected_btn(key_matrix, id);
                    if (joy_focused_btn_id >= 0 && joy_focused_btn_id != id) {
                        bool shift_caps_active = (joy_focused_btn_id == shift_btn_id) && (is_caps || is_capslock);
                        if (!shift_caps_active) {
                            lv_btnmatrix_clear_btn_ctrl(key_matrix, joy_focused_btn_id, LV_BTNMATRIX_CTRL_CHECKED);
                        }
                    }
                    joy_focused_btn_id = id;
                    lv_btnmatrix_set_btn_ctrl(key_matrix, id, LV_BTNMATRIX_CTRL_CHECKED);

                    // Cancel any leftover hold timer from a previous press
                    if (joy_hold_timer) { lv_timer_del(joy_hold_timer); joy_hold_timer = NULL; }
                    joy_holding_letter = false;
                    joy_pending_letter = false;
                    joy_inverted_case  = false;

                    if (is_letter) {
                        // Start hold timer — inversion only activates after KEYBOARD_HOLD_INVERT_MS
                        joy_pending_letter = true;
                        joy_inverted_case  = !(is_capslock || is_caps);
                        joy_hold_timer = lv_timer_create(keyboard_hold_invert_cb, KEYBOARD_HOLD_INVERT_MS, NULL);
                        lv_timer_set_repeat_count(joy_hold_timer, 1);
                    } else {
                        // Non-letter key: trigger immediately on press
                        lv_event_send(key_matrix, LV_EVENT_VALUE_CHANGED, NULL);
                    }
                } else {
                    // On release: cancel timer if hold threshold wasn't reached yet
                    if (joy_hold_timer) { lv_timer_del(joy_hold_timer); joy_hold_timer = NULL; }

                    if (is_letter && (joy_holding_letter || joy_pending_letter)) {
                        // Capture the char before any rebuild that might invalidate txt
                        char typed = txt[0]; // btnmatrix label already reflects current/inverted case
                        bool was_holding = joy_holding_letter;
                        joy_holding_letter = false;
                        joy_pending_letter  = false;
                        joy_inverted_case   = false;
                        add_char_to_buffer_raw(typed);
                        bool needs_rebuild = was_holding; // always redraw after hold to restore labels
                        if (!was_holding && is_caps && !is_capslock) {
                            // Short press with one-shot shift: reset shift as normal
                            is_caps = false;
                            needs_rebuild = true;
                        }
                        if (needs_rebuild) build_key_matrix();
                    } else {
                        joy_holding_letter = false;
                        joy_pending_letter  = false;
                        joy_inverted_case   = false;
                    }
                }
                return;
            }
            
            lv_btnmatrix_set_selected_btn(key_matrix, id);
            // clear previous joystick highlight if any
            if (joy_focused_btn_id >= 0 && joy_focused_btn_id != id) {
                // don't clear SHIFT's CHECKED state when it's indicating caps/capslock
                bool shift_caps_active = (joy_focused_btn_id == shift_btn_id) && (is_caps || is_capslock);
                if (!shift_caps_active) {
                    lv_btnmatrix_clear_btn_ctrl(key_matrix, joy_focused_btn_id, LV_BTNMATRIX_CTRL_CHECKED);
                }
            }
            joy_focused_btn_id = id;
            // mark current key as CHECKED so it uses inverted colors
            lv_btnmatrix_set_btn_ctrl(key_matrix, id, LV_BTNMATRIX_CTRL_CHECKED);
            return;
        }
#endif

        // Fallback for non-btnmatrix (legacy per-key buttons)
        if (button == 1) {
            if (pressed) {
                // Cancel any leftover hold timer
                if (joy_hold_timer) { lv_timer_del(joy_hold_timer); joy_hold_timer = NULL; }
                joy_holding_letter = false;
                joy_pending_letter  = false;
                joy_inverted_case   = false;
                const char *(*current_keys)[10] = get_current_keys();
                if (cursor_row >= 0 && cursor_row < num_rows && cursor_col >= 0 && cursor_col < row_lens[cursor_row]) {
                    const char *key = current_keys[cursor_row][cursor_col];
                    if (strlen(key) == 1 && isalpha((unsigned char)key[0])) {
                        // Letter key: start hold timer
                        joy_pending_letter = true;
                        joy_inverted_case  = !(is_capslock || is_caps);
                        joy_hold_timer = lv_timer_create(keyboard_hold_invert_cb, KEYBOARD_HOLD_INVERT_MS, NULL);
                        lv_timer_set_repeat_count(joy_hold_timer, 1);
                        apply_selection_highlight();
                    } else {
                        // Non-letter: activate immediately
                        activate_selected_key();
                    }
                }
            } else {
                // Release: cancel timer if not yet fired
                if (joy_hold_timer) { lv_timer_del(joy_hold_timer); joy_hold_timer = NULL; }
                if (joy_holding_letter || joy_pending_letter) {
                    const char *(*current_keys)[10] = get_current_keys();
                    const char *key = current_keys[cursor_row][cursor_col];
                    char c = key[0];
                    bool was_holding = joy_holding_letter;
                    if (was_holding) {
                        c = joy_inverted_case ? (char)toupper((unsigned char)c) : (char)tolower((unsigned char)c);
                    } else {
                        c = is_caps ? (char)toupper((unsigned char)c) : (char)tolower((unsigned char)c);
                    }
                    joy_holding_letter = false;
                    joy_pending_letter  = false;
                    joy_inverted_case   = false;
                    add_char_to_buffer_raw(c);
                    bool needs_rebuild = was_holding; // restore inverted label after hold
                    if (!was_holding && is_caps && !is_capslock) {
                        is_caps = false;
                        needs_rebuild = true;
                    }
                    if (needs_rebuild) update_key_labels();
                } else {
                    joy_holding_letter = false;
                    joy_pending_letter  = false;
                    joy_inverted_case   = false;
                }
                apply_selection_highlight();
            }
            return;
        }

    } else if (event->type == INPUT_TYPE_TOUCH && event->data.touch_data.state == LV_INDEV_STATE_PR) {
        int touch_x = event->data.touch_data.point.x;
        int touch_y = event->data.touch_data.point.y;
        ESP_LOGD(TAG, "touch PR x=%d y=%d", touch_x, touch_y);
        
        int screen_height = LV_VER_RES;
        int status_bar_height = GUI_STATUS_BAR_H;
        int display_height = keyboard_display_height();
        int padding = keyboard_layout_padding();
        int keys_start_y = status_bar_height + display_height + padding * 2;

        int row = -1;
        int col = -1;
        if (key_matrix) {
            /* Geometry comes from the same arithmetic lv_btnmatrix uses, so the
             * drawn key and the hit key always agree. Spacers yield no column. */
            if (!kb_matrix_hit_test(touch_x, touch_y, &row, &col)) return;
        } else {
            if (touch_y < keys_start_y) return;
            int keys_area_height = screen_height - keys_start_y - padding;
            int key_height = keys_area_height / num_rows; if (key_height <= 0) key_height = 1;
            row = (touch_y - keys_start_y) / key_height;
            const int *current_row_lengths = get_current_row_lengths();
            if (row >= 0 && row < num_rows) {
                for (int c = 0; c < current_row_lengths[row]; c++) {
                    int key_x, key_w;
                    get_key_position(row, c, &key_x, &key_w, is_symbols_mode);
                    if (touch_x >= key_x && touch_x < key_x + key_w) {
                        col = c;
                        break;
                    }
                }
            }
        }

        ESP_LOGD(TAG, "touch row=%d col=%d (sym=%d caps=%d capslock=%d)", row, col, (int)is_symbols_mode, (int)is_caps, (int)is_capslock);
        if (row >= 0 && col >= 0) {
            const char *key = kb_key_label_at(row, col);
            if (!key) return;

            if (key_matrix) {
                /* Mirror the joystick focus style for touch presses; cleared on release. */
                int bid = kb_cursor_to_btn_id(row, col);
                if (bid >= 0) {
                    pressed_btn_id = bid;
                    lv_btnmatrix_set_btn_ctrl(key_matrix, bid, LV_BTNMATRIX_CTRL_CHECKED);
                }
            } else {
                // Find the key button object (legacy per-key objects)
                int key_index = 0;
                for (int rr = 0; rr < num_rows; rr++) {
                    for (int cc = 0; cc < max_row_lengths[rr]; cc++) {
                        if (rr == row && cc == col) {
                            int child_idx = 1 + key_index;
                            pressed_key_btn = lv_obj_get_child(root, child_idx);
                            if (pressed_key_btn) {
                                lv_obj_set_style_bg_color(pressed_key_btn, kb_accent(), 0);
                            }
                        }
                        key_index++;
                    }
                }
            }

            if (is_shift_key(key)) {
                if (is_caps) {
                    // If SHIFT is already active, toggle capslock
                    is_capslock = !is_capslock;
                    is_caps = is_capslock; // Keep caps active if capslock is on
                } else {
                    is_caps = true;
                }
                update_key_labels();
                ESP_LOGD(TAG, "shift toggled: caps=%d capslock=%d", (int)is_caps, (int)is_capslock);
            } else if (is_symbol_key(key) || strcmp(key, "123") == 0) {
                /* Switching pages rebuilds the matrix and deletes existing key
                 * buttons. Clear any stored pointer to the previously pressed
                 * key to avoid touching a freed object on the REL event. */
                pressed_key_btn = NULL;
                pressed_btn_id = -1;
                kb_sym2 = false;
                is_symbols_mode = true;
                recreate_keyboard_buttons();
                ensure_valid_cursor();
            } else if (strcmp(key, "#+=") == 0) {
                pressed_key_btn = NULL;
                pressed_btn_id = -1;
                is_symbols_mode = true;
                kb_sym2 = true;
                recreate_keyboard_buttons();
                ensure_valid_cursor();
            } else if (strcmp(key, "ABC") == 0) {
                pressed_key_btn = NULL;
                pressed_btn_id = -1;
                is_symbols_mode = false;
                kb_sym2 = false;
                recreate_keyboard_buttons();
                ensure_valid_cursor();
            } else if (is_exit_key(key)) {
                display_manager_go_back();
            } else if (is_done_key(key)) {
                submit_text();
            } else if (is_del_key(key)) {
                remove_char_from_buffer();
            } else if (is_space_key(key)) {
                add_char_to_buffer(' ');
            } else if (strlen(key) == 1) {
                char adjusted_char = key[0];
                if (!is_symbols_mode && isalpha((unsigned char)adjusted_char)) {
                    adjusted_char = is_caps ? toupper(adjusted_char) : tolower(adjusted_char);
                }
                add_char_to_buffer(adjusted_char);
                ESP_LOGD(TAG, "char added: %c", adjusted_char);
            }
        }
    } else if (event->type == INPUT_TYPE_TOUCH && event->data.touch_data.state == LV_INDEV_STATE_REL) {
        if (pressed_btn_id >= 0 && key_matrix) {
            bool keep = (pressed_btn_id == joy_focused_btn_id) ||
                        (pressed_btn_id == shift_btn_id && (is_caps || is_capslock));
            if (!keep) {
                lv_btnmatrix_clear_btn_ctrl(key_matrix, pressed_btn_id, LV_BTNMATRIX_CTRL_CHECKED);
            }
            pressed_btn_id = -1;
        }
        if (pressed_key_btn) {
            // Only restore style if not SHIFT key, otherwise let update_key_labels() handle it
            lv_obj_t *key_label = lv_obj_get_child(pressed_key_btn, 0);
            const char *label_text = lv_label_get_text(key_label);
            if (strcmp(label_text, LV_SYMBOL_UP) != 0) {
                lv_obj_set_style_bg_color(pressed_key_btn, kb_surface(), 0);
                lv_obj_set_style_bg_opa(pressed_key_btn, LV_OPA_COVER, 0);
            }
            pressed_key_btn = NULL;
            // Always update key labels to refresh SHIFT key highlight
            update_key_labels();
            apply_selection_highlight();
        }
    } else if (event->type == INPUT_TYPE_KEYBOARD) {
        char c = (char)event->data.key_value;
        if (c == '`') {
            display_manager_go_back();
        } else if (c == '\n' || c == '\r' || c == '=') {
            if (immediate_callback) {
                // Real-time typing mode (e.g. BadUSB): send Enter, don't close
                immediate_callback('\n');
            } else {
                submit_text();
            }
        } else if (c == '\b') {
            if (input_len == 0 && immediate_callback) immediate_callback('\b');
            remove_char_from_buffer();
        } else if (c >= ' ' && c <= '~') {
            add_char_to_buffer_raw(c);
        }
#if defined(CONFIG_USE_ENCODER) || defined(CONFIG_IS_ATOMS3R)
    } else if (event->type == INPUT_TYPE_EXIT_BUTTON) {
        ESP_LOGI(TAG, "IO6 exit button pressed, returning to previous view");
        display_manager_go_back();
#endif
    }
}

static void get_keyboard_callback(void **callback) {
    *callback = keyboard_view.input_callback;
}

void keyboard_view_set_submit_callback(KeyboardSubmitCallback cb){
    submit_callback = cb;
}

void keyboard_view_set_immediate_callback(KeyboardImmediateCallback cb){
    immediate_callback = cb;
}

void keyboard_view_set_placeholder(const char *text){
    if (text && strlen(text) < sizeof(placeholder)) {
        strncpy(placeholder, text, sizeof(placeholder) - 1);
        placeholder[sizeof(placeholder) - 1] = '\0';
    }
    memset(input_buffer, 0, sizeof(input_buffer));
    input_len = 0;
    update_input_label();
}

void keyboard_view_set_start_caps(bool start_caps) {
    start_with_caps = start_caps;
}

void keyboard_view_set_initial_text(const char *text) {
    memset(pending_initial_text, 0, sizeof(pending_initial_text));
    has_pending_initial_text = true;
    if (text && text[0] != '\0') {
        size_t n = strlen(text);
        if (n >= sizeof(pending_initial_text)) n = sizeof(pending_initial_text) - 1;
        memcpy(pending_initial_text, text, n + 1);
    }
    /* If view already exists, apply now so it shows without waiting for next create */
    if (root != NULL) {
        memset(input_buffer, 0, sizeof(input_buffer));
        input_len = 0;
        if (text && text[0] != '\0') {
            size_t n = strlen(text);
            if (n >= sizeof(input_buffer)) n = sizeof(input_buffer) - 1;
            memcpy(input_buffer, text, n + 1);
            input_buffer[n] = '\0';
            input_len = (int)n;
        }
        has_pending_initial_text = false;
        update_input_label();
    }
}

void keyboard_view_set_return_view(View *view) {
    /* Compatibility no-op. Done and Exit both pop the keyboard route. */
    (void)view;
}

View keyboard_view = {
    .root = NULL,
    .create = keyboard_create,
    .destroy = keyboard_destroy,
    .input_callback = handle_hardware_button_press_keyboard,
    .name = "Keyboard Screen",
    .get_hardwareinput_callback = get_keyboard_callback
};

static void key_matrix_event_cb(lv_event_t *e) {
#if defined(CONFIG_USE_TOUCHSCREEN) || defined(CONFIG_USE_JOYSTICK)
    lv_obj_t *m = lv_event_get_target(e);
    if (!m) return;
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    int id = lv_btnmatrix_get_selected_btn(m);
    if (id < 0) return;
    const char *txt = lv_btnmatrix_get_btn_text(m, id);
    if (!txt) return;
    if (strcmp(txt, LV_SYMBOL_UP) == 0 || strcmp(txt, "SHIFT") == 0) {
        if (is_caps) {
            is_capslock = !is_capslock;
            is_caps = is_capslock;
        } else {
            is_caps = true;
        }
        build_key_matrix();
    } else if (strcmp(txt, "123") == 0) {
        is_symbols_mode = true;
        kb_sym2 = false;
        build_key_matrix();
        ensure_valid_cursor();
    } else if (strcmp(txt, "#+=") == 0) {
        is_symbols_mode = true;
        kb_sym2 = true;
        build_key_matrix();
        ensure_valid_cursor();
    } else if (strcmp(txt, "ABC") == 0) {
        is_symbols_mode = false;
        kb_sym2 = false;
        build_key_matrix();
        ensure_valid_cursor();
    } else if (strcmp(txt, "Exit") == 0 || strcmp(txt, LV_SYMBOL_CLOSE) == 0) {
        display_manager_go_back();
    } else if (strcmp(txt, "Done") == 0 || strcmp(txt, LV_SYMBOL_NEW_LINE) == 0) {
        submit_text();
    } else if (strcmp(txt, LV_SYMBOL_BACKSPACE) == 0 || strcmp(txt, "DEL") == 0) {
        remove_char_from_buffer();
    } else if (strcmp(txt, " ") == 0) {
        add_char_to_buffer(' ');
    } else if (strlen(txt) == 1) {
        char c = txt[0];
        if (!is_symbols_mode && isalpha((unsigned char)c)) {
            c = (is_capslock || is_caps) ? (char)toupper((unsigned char)c) : (char)tolower((unsigned char)c);
        }
        add_char_to_buffer_raw(c);
        // Only reset temporary SHIFT/caps and rebuild when in alpha mode; symbols shouldn't rebuild
        if (!is_symbols_mode && is_caps && !is_capslock) {
            is_caps = false;
            build_key_matrix();
        }
    }
#else
    (void)e;
#endif
}

static void build_key_matrix(void) {
#if defined(CONFIG_USE_TOUCHSCREEN) || defined(CONFIG_USE_JOYSTICK)
    if (!root) return;

    kb_metrics_t m;
    kb_compute_metrics(&m, LV_HOR_RES, LV_VER_RES, GUI_STATUS_BAR_H);

    const kb_key_desc_t * const *page = kb_active_page();

    static char map_text_storage[KB_IOS_MAX_BTNS][KB_KEY_TEXT_LEN];
    int map_idx = 0;
    int btn_idx = 0;
    shift_btn_id = -1;
    /* Inverted-case preview applies to the button the joystick is holding. */
    int focused_btn_idx = joy_holding_letter ? kb_cursor_to_btn_id(cursor_row, cursor_col) : -1;

    for (int r = 0; r < KB_IOS_ROWS; r++) {
        const kb_key_desc_t *k = page[r];
        for (; k && k->label; k++) {
            if (btn_idx >= KB_IOS_MAX_BTNS - 1) break;
            const char *label = k->label;
            if (!k->spacer) {
                if (is_shift_key(label)) label = LV_SYMBOL_UP;
                else if (is_del_key(label)) label = LV_SYMBOL_BACKSPACE;
                else if (is_exit_key(label)) label = LV_SYMBOL_CLOSE;
                else if (is_done_key(label)) label = LV_SYMBOL_NEW_LINE;
                else if (!is_symbols_mode && is_alpha_key(label)) {
                    char ch;
                    if (joy_holding_letter && btn_idx == focused_btn_idx) {
                        ch = joy_inverted_case ? (char)toupper((unsigned char)label[0])
                                               : (char)tolower((unsigned char)label[0]);
                    } else {
                        ch = is_caps ? (char)toupper((unsigned char)label[0])
                                     : (char)tolower((unsigned char)label[0]);
                    }
                    map_text_storage[btn_idx][0] = ch;
                    map_text_storage[btn_idx][1] = '\0';
                    label = map_text_storage[btn_idx];
                }
            }
            if (label == k->label) {
                size_t n = strlen(label);
                if (n > KB_KEY_TEXT_LEN - 1) n = KB_KEY_TEXT_LEN - 1;
                memcpy(map_text_storage[btn_idx], label, n);
                map_text_storage[btn_idx][n] = '\0';
                label = map_text_storage[btn_idx];
            }
            btn_map[map_idx++] = label;
            if (is_shift_key(k->label)) shift_btn_id = btn_idx;
            btn_idx++;
        }
        if (r < KB_IOS_ROWS - 1) btn_map[map_idx++] = "\n";
    }
    btn_map[map_idx] = "";
    btn_map_len = map_idx;

    if (!key_matrix) {
        key_matrix = lv_btnmatrix_create(root);
        if (!key_matrix) return;
        lv_obj_remove_style_all(key_matrix);
        lv_obj_add_style(key_matrix, &style_key_btn, LV_PART_ITEMS);
        lv_obj_add_style(key_matrix, &style_key_label, LV_PART_ITEMS);
        lv_obj_add_event_cb(key_matrix, key_matrix_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
        /* Transparent so the gutter shows the root background, iOS style. */
        lv_obj_set_style_bg_opa(key_matrix, LV_OPA_TRANSP, LV_PART_MAIN);
    }

    lv_obj_set_pos(key_matrix, m.pad_h, m.matrix_y);
    lv_obj_set_size(key_matrix, m.matrix_w, m.matrix_h);
    lv_obj_set_style_radius(key_matrix, kb_radius(), LV_PART_MAIN);
    lv_obj_set_style_pad_all(key_matrix, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(key_matrix, m.gap, LV_PART_MAIN);
    lv_obj_set_style_pad_column(key_matrix, m.gap, LV_PART_MAIN);
    lv_obj_set_style_text_font(key_matrix, m.font, LV_PART_ITEMS);

    lv_btnmatrix_set_map(key_matrix, btn_map);

    /* Widths and controls must be applied after set_map, which allocates the
     * button areas. Spacers keep their reserved width but stay HIDDEN, so they
     * lay out without being drawn or tappable. */
    int id = 0;
    for (int r = 0; r < KB_IOS_ROWS; r++) {
        const kb_key_desc_t *k = page[r];
        for (; k && k->label && id < btn_idx; k++, id++) {
            lv_btnmatrix_set_btn_width(key_matrix, id, k->units ? k->units : 1);
            lv_btnmatrix_clear_btn_ctrl(key_matrix, id, LV_BTNMATRIX_CTRL_CHECKED);
            lv_btnmatrix_clear_btn_ctrl(key_matrix, id, LV_BTNMATRIX_CTRL_HIDDEN);
            lv_btnmatrix_clear_btn_ctrl(key_matrix, id, LV_BTNMATRIX_CTRL_CLICK_TRIG);
            /* Never recolor: the layout contains a literal '#' key. */
            lv_btnmatrix_clear_btn_ctrl(key_matrix, id, LV_BTNMATRIX_CTRL_RECOLOR);
            if (k->spacer) {
                lv_btnmatrix_set_btn_ctrl(key_matrix, id, LV_BTNMATRIX_CTRL_HIDDEN);
            } else {
                lv_btnmatrix_set_btn_ctrl(key_matrix, id, LV_BTNMATRIX_CTRL_CHECKABLE);
            }
        }
    }

    if (shift_btn_id >= 0) {
        // SHIFT is click-triggered and also reflects caps/capslock state via CHECKED (inverted colors)
        lv_btnmatrix_set_btn_ctrl(key_matrix, shift_btn_id,
                                  LV_BTNMATRIX_CTRL_CHECKABLE | LV_BTNMATRIX_CTRL_CLICK_TRIG);
        if (is_caps || is_capslock) {
            lv_btnmatrix_set_btn_ctrl(key_matrix, shift_btn_id, LV_BTNMATRIX_CTRL_CHECKED);
        }
    }

    lv_color_t accent = kb_accent();
    lv_color_t sel_fg = kb_sel_text();
    lv_obj_set_style_bg_color(key_matrix, kb_surface(), LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(key_matrix, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(key_matrix, kb_text(), LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(key_matrix, accent, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(key_matrix, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(key_matrix, sel_fg, LV_PART_ITEMS | LV_STATE_CHECKED);

    // a rebuild invalidates the previous focus/press ids; they are re-established on the next input
    joy_focused_btn_id = -1;
    pressed_btn_id = -1;

    if (radius_override_active) {
        lv_style_set_radius(&style_key_btn, saved_key_radius);
        if (input_label) lv_obj_set_style_radius(input_label, saved_input_label_radius, 0);
        radius_override_active = false;
    }
    lv_obj_clear_flag(root, LV_OBJ_FLAG_HIDDEN);
#endif
}

static void get_key_position(int row, int col, int *x, int *width, bool symbols_mode) {
    int screen_width = LV_HOR_RES;
    int padding = keyboard_layout_padding();
    const int *row_lens = symbols_mode ? symbols_row_lengths : row_lengths;
    int actual_len = row_lens[row];
    int special_count = 0;
    if (!symbols_mode) {
        for (int i = 0; i < actual_len; i++) {
            const char *txt = keys[row][i];
            if (strcmp(txt, "SHIFT") == 0 || strcmp(txt, "DEL") == 0 || strcmp(txt, " ") == 0) {
                special_count++;
            }
        }
    }
    int total_key_width = screen_width - (padding * 2);
    int current_row_length = max_row_lengths[row];
    int key_width = total_key_width / current_row_length;
    int extra_space = special_count * (key_width / 2);
    int total_keys_width = actual_len * key_width + extra_space;
    int blank_space = total_key_width - total_keys_width;
    int key_x = padding + blank_space / 2;

    // Calculate position for each key
    for (int c = 0; c < col; c++) {
        int current_key_width = key_width;
        if (!symbols_mode && c < row_lens[row]) {
            const char *txt = keys[row][c];
            if (strcmp(txt, "SHIFT") == 0 || strcmp(txt, "DEL") == 0 || strcmp(txt, " ") == 0) {
                current_key_width += key_width / 2;
            }
        }
        key_x += current_key_width;
    }
    int current_key_width = key_width;
    if (!symbols_mode && col < row_lens[row]) {
        const char *txt = keys[row][col];
        if (strcmp(txt, "SHIFT") == 0 || strcmp(txt, "DEL") == 0 || strcmp(txt, " ") == 0) {
            current_key_width += key_width / 2;
        }
    }
    *x = key_x;
    *width = current_key_width;
}

static void ensure_valid_cursor(void) {
    const int *row_lens = key_matrix ? kb_matrix_row_lens() : get_current_row_lengths();
    int rows = key_matrix ? kb_active_rows() : num_rows;
    if (cursor_row < 0) cursor_row = 0;
    if (cursor_row >= rows) cursor_row = rows - 1;
    int max_col = row_lens[cursor_row] - 1;
    if (max_col < 0) max_col = 0;
    if (cursor_col < 0) cursor_col = 0;
    if (cursor_col > max_col) cursor_col = max_col;
}

static lv_obj_t *get_key_button_at(int row, int col) {
    if (!root) return NULL;
    int key_index = 0;
    for (int rr = 0; rr < row; rr++) key_index += max_row_lengths[rr];
    key_index += col;
    uint32_t child_count = lv_obj_get_child_cnt(root);
    int child_idx = 1 + key_index; // skip input_label at 0
    if (child_idx >= 0 && (uint32_t)child_idx < child_count) {
        return lv_obj_get_child(root, child_idx);
    }
    return NULL;
}

static void apply_selection_highlight(void) {
    if (!root) return;
#if defined(CONFIG_USE_TOUCHSCREEN) || defined(CONFIG_USE_JOYSTICK)
    if (key_matrix) return; // btnmatrix handles its own visuals on touch/joystick builds
#endif
    ensure_valid_cursor();
    // reset all key borders to default
    int key_index = 0;
    uint32_t child_count = lv_obj_get_child_cnt(root);
    for (int r = 0; r < num_rows; r++) {
        for (int c = 0; c < max_row_lengths[r]; c++) {
            int child_idx = 1 + key_index;
            if ((uint32_t)child_idx < child_count) {
                lv_obj_t *btn = lv_obj_get_child(root, child_idx);
                if (btn) {
                    lv_obj_set_style_border_color(btn, kb_text(), 0);
                    lv_obj_set_style_border_opa(btn, LV_OPA_30, 0);
                    lv_obj_set_style_border_width(btn, 1, 0);
                }
            }
            key_index++;
        }
    }
    // highlight current cursor key
    lv_obj_t *btn = get_key_button_at(cursor_row, cursor_col);
    if (btn) {
        lv_obj_set_style_border_color(btn, kb_accent(), 0);
        lv_obj_set_style_border_width(btn, 3, 0);
        selected_key_btn = btn;
    } else {
        selected_key_btn = NULL;
    }
}

static void activate_selected_key(void) {
    const char *(*current_keys)[10] = get_current_keys();
    const int *current_row_lengths = get_current_row_lengths();
    if (cursor_row < 0 || cursor_row >= num_rows) return;
    if (cursor_col < 0 || cursor_col >= current_row_lengths[cursor_row]) return;
    const char *key = current_keys[cursor_row][cursor_col];
    if (strcmp(key, "SHIFT") == 0) {
        if (is_caps) {
            is_capslock = !is_capslock;
            is_caps = is_capslock;
        } else {
            is_caps = true;
        }
        update_key_labels();
        apply_selection_highlight();
    } else if (strcmp(key, "SYM") == 0) {
        pressed_key_btn = NULL;
        is_symbols_mode = true;
        recreate_keyboard_buttons();
        ensure_valid_cursor();
        apply_selection_highlight();
    } else if (strcmp(key, "ABC") == 0) {
        pressed_key_btn = NULL;
        is_symbols_mode = false;
        recreate_keyboard_buttons();
        ensure_valid_cursor();
        apply_selection_highlight();
    } else if (strcmp(key, "Exit") == 0) {
        display_manager_go_back();
    } else if (strcmp(key, "Done") == 0) {
        submit_text();
    } else if (strcmp(key, "DEL") == 0) {
        remove_char_from_buffer();
        apply_selection_highlight();
    } else if (strcmp(key, " ") == 0) {
        add_char_to_buffer(' ');
        apply_selection_highlight();
    } else if (strlen(key) == 1) {
        char adjusted_char = key[0];
        if (!is_symbols_mode && isalpha((unsigned char)adjusted_char)) {
            adjusted_char = is_caps ? (char)toupper((unsigned char)adjusted_char) : (char)tolower((unsigned char)adjusted_char);
        }
        add_char_to_buffer(adjusted_char);
        apply_selection_highlight();
    }
}
