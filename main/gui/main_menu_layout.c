#include "gui/main_menu_layout.h"
#include "gui/design_tokens.h"
#include "sdkconfig.h"

static int clamp_int(int value, int min, int max) {
    if (value < min) return min;
    if (value > max) return max;
    return value;
}

main_menu_layout_kind_t main_menu_layout_from_setting(uint8_t setting) {
    switch (setting) {
        case 1:
            return MAIN_MENU_LAYOUT_LAUNCHER;
        case 2:
            return MAIN_MENU_LAYOUT_LIST;
        case 3:
            return MAIN_MENU_LAYOUT_COMPACT;
        case 4:
            return MAIN_MENU_LAYOUT_HERO;
        default:
            return MAIN_MENU_LAYOUT_CAROUSEL;
    }
}

main_menu_layout_kind_t main_menu_layout_resolve_for_size(main_menu_layout_kind_t kind,
                                                          int screen_width,
                                                          int screen_height) {
    /* The CrowPanel's LCD controller exposes a square framebuffer, but the
     * physical aperture is round.  Use the single-focus hero presentation
     * for both the main menu and app gallery so labels, icons, and selection
     * chrome stay inside the circular safe area.  Encoder rotation and touch
     * swipes remain the navigation model; side navigation buttons would sit in
     * the masked corners. */
#if defined(CONFIG_CROWPANEL_1P28_ROTARY)
    (void)screen_width;
    (void)screen_height;
    return MAIN_MENU_LAYOUT_HERO;
#endif

    /* A 128px square cannot display the carousel/hero chrome reliably. Use
     * the paginated launcher unless the user explicitly chose the list view. */
    if (screen_width <= 128 && screen_height <= 160 &&
        kind != MAIN_MENU_LAYOUT_LIST) {
        return MAIN_MENU_LAYOUT_LAUNCHER;
    }
    if (kind == MAIN_MENU_LAYOUT_LAUNCHER && (screen_width < 120 || screen_height <= 80)) {
        return MAIN_MENU_LAYOUT_CAROUSEL;
    }
    return kind;
}

void main_menu_layout_get_metrics_for_size(main_menu_layout_kind_t kind, int item_count,
                                           int screen_width, int screen_height,
                                           int status_bar_height,
                                           main_menu_layout_metrics_t *metrics) {
    if (!metrics) return;

    if (screen_width < 1) screen_width = 1;
    if (screen_height < 1) screen_height = 1;
#if defined(CONFIG_CROWPANEL_1P28_ROTARY)
    /* Generic views begin below the circular-safe status pill. */
    status_bar_height = 36;
#endif
    status_bar_height = clamp_int(status_bar_height, 0, screen_height);
    int content_height = screen_height - status_bar_height - GUI_HOME_SAFE_H;
    if (content_height < 60) content_height = screen_height;

#if GUI_LARGE_SCREEN
    bool large_screen = screen_width >= 480 && screen_height >= 400 && content_height >= 340;
#else
    bool large_screen = false;
#endif

    main_menu_density_t density = MAIN_MENU_DENSITY_REGULAR;
    if (screen_width <= 160 || content_height <= 96) {
        density = MAIN_MENU_DENSITY_COMPACT;
    } else if (screen_width >= 320 && content_height >= 216) {
        density = MAIN_MENU_DENSITY_COMFORTABLE;
    }

    *metrics = (main_menu_layout_metrics_t){
        .screen_width = screen_width,
        .screen_height = screen_height,
        .status_bar_height = status_bar_height,
        .content_height = content_height,
        .density = density,
        .container_align = LV_ALIGN_CENTER,
        .container_x = 0,
        .container_y = (status_bar_height - GUI_HOME_SAFE_H) / 2,
        .container_width = screen_width,
        .container_height = content_height,
    };

    int min_dim = LV_MIN(screen_width, screen_height);
    int carousel_size = (int)(min_dim * 0.55f);
    if (min_dim <= 128) {
        carousel_size = (int)(min_dim * 0.62f);
    } else if (min_dim >= 320) {
        carousel_size = (int)(min_dim * 0.42f);
    }
    metrics->carousel_button_size = clamp_int(carousel_size, 64, 160);
    metrics->carousel_icon_target = clamp_int((int)(metrics->carousel_button_size * 0.38f), 20, 56);
    metrics->carousel_icon_y_offset = metrics->carousel_button_size <= 80 ? -6 : -10;
    metrics->carousel_show_label = screen_width > 150;
    int carousel_side_space = (screen_width - metrics->carousel_button_size) / 2;
    metrics->carousel_preview_size = clamp_int(carousel_side_space - 16, 32, 72);
    metrics->carousel_preview_icon_target = clamp_int((int)(metrics->carousel_preview_size * 0.55f), 18, 40);
    metrics->carousel_preview_offset = metrics->carousel_button_size / 2 +
                                       metrics->carousel_preview_size / 2 + GUI_GRID * 2;
    metrics->carousel_show_previews = screen_width >= 200 && carousel_side_space >= 48;
    metrics->carousel_transition_distance = clamp_int(screen_width / 4, 48, 96);

    if (large_screen) {
        metrics->carousel_button_size = clamp_int((int)(min_dim * 0.46f), 220, 280);
        metrics->carousel_icon_target = clamp_int((int)(metrics->carousel_button_size * 0.42f), 88, 120);
        metrics->carousel_preview_size = clamp_int((screen_width - metrics->carousel_button_size) / 2 - 64, 64, 112);
        metrics->carousel_preview_icon_target = LV_MIN(64, metrics->carousel_preview_size - 16);
        metrics->carousel_preview_offset = metrics->carousel_button_size / 2 +
                                            metrics->carousel_preview_size / 2 + 32;
        metrics->carousel_transition_distance = 180;
    }

    metrics->nav_button_size = 52;
    metrics->nav_button_margin = 15;
    if (screen_width <= 128) {
        metrics->nav_button_size = 40;
        metrics->nav_button_margin = 10;
    } else if (screen_width >= 320) {
        metrics->nav_button_size = 60;
        metrics->nav_button_margin = 20;
    }
    if (large_screen) {
        metrics->nav_button_size = 56;
        metrics->nav_button_margin = 32;
    }

    if (kind == MAIN_MENU_LAYOUT_HERO) {
        /* Flipper-style: one big icon + title, minimal chrome. No card, no
         * shadows, no previews. Only the slide distance is shared with the
         * carousel render path. */
        int min_dim = LV_MIN(screen_width, content_height);
        int icon_target = (int)(min_dim * 0.42f);
#if defined(CONFIG_CROWPANEL_1P28_ROTARY)
        /* Give the single focused item more visual weight on the small,
         * circular aperture while leaving room for its one-line title. */
        icon_target = (int)(min_dim * 0.46f);
#endif
        if (min_dim <= 128) icon_target = (int)(min_dim * 0.48f);
        metrics->hero_icon_target = clamp_int(icon_target, 48, 176);
        metrics->hero_icon_y_offset = clamp_int(-((int)(metrics->hero_icon_target / 3)), -44, -14);
        metrics->hero_pip_count = LV_MIN(9, screen_width <= 160 ? 5 : 7);
        metrics->carousel_show_previews = false;
        metrics->carousel_show_label = screen_width > 120;
        metrics->carousel_transition_distance = clamp_int(screen_width / 4, 48, 96);
        metrics->carousel_button_size = metrics->hero_icon_target; /* unused by HERO */
        metrics->carousel_icon_target = metrics->hero_icon_target;
        metrics->carousel_icon_y_offset = metrics->hero_icon_y_offset;
        if (large_screen) {
            metrics->hero_icon_target = clamp_int(content_height / 3, 120, 200);
            metrics->hero_icon_y_offset = -32;
            metrics->hero_pip_count = 7;
        }
    }

    metrics->list_button_height = (screen_height <= 160 || screen_width <= 160) ? 32 : 44;
    metrics->list_icon_target = metrics->list_button_height <= 38 ? 20 : 26;
    metrics->list_button_pad = density == MAIN_MENU_DENSITY_COMPACT ? 6 : 8;
    metrics->list_pad = screen_width > 200 ? 16 : 10;
    metrics->list_row_gap = 6;
    metrics->list_column_gap = 12;

    if (large_screen) {
        metrics->list_button_height = 64;
        metrics->list_icon_target = 40;
        metrics->list_button_pad = 12;
        metrics->list_pad = 24;
        metrics->list_row_gap = 10;
        metrics->list_column_gap = 16;
    }

    bool portrait = screen_height > screen_width;
    int columns = (screen_width >= 320) ? 4 : (screen_width >= 240) ? 3 : 2;
#if GUI_LARGE_SCREEN
    if (screen_width >= 800) columns = 6;
#endif
    if (kind == MAIN_MENU_LAYOUT_COMPACT) {
        columns = (screen_width >= 320) ? 4 : (screen_width >= 240) ? 3 :
                  (screen_width >= 160) ? 2 : 1;
    }
    if (item_count > 0 && columns > item_count) columns = item_count;
    if (columns <= 0) columns = 1;

    metrics->columns = columns;
    metrics->rows = item_count > 0 ? (item_count + columns - 1) / columns : 1;
    if (metrics->rows <= 0) metrics->rows = 1;
    metrics->visible_rows = portrait && screen_width >= 200 ? 4 : 2;
    metrics->margin = (screen_width <= 240 || content_height <= 120) ? 4 : GUI_GRID;
    if (portrait && screen_width >= 200) {
        metrics->margin = 6;
    }
    metrics->page_indicator_height = 0;
    if (kind == MAIN_MENU_LAYOUT_LAUNCHER) {
        metrics->page_indicator_height = density == MAIN_MENU_DENSITY_COMPACT ? 10 : 14;
#if GUI_LARGE_SCREEN
        if (screen_width >= 800) metrics->page_indicator_height = 20;
#endif
        int page_content_height = content_height - metrics->page_indicator_height;
        metrics->visible_rows = page_content_height >= 360 ? 4 :
                                page_content_height >= 240 ? 3 :
                                page_content_height >= 120 ? 2 : 1;
    } else if (kind == MAIN_MENU_LAYOUT_COMPACT) {
        /* Compact is intentionally a single-screen layout: use as many rows
         * as the item count needs instead of creating another page. */
        metrics->page_indicator_height = 0;
        metrics->visible_rows = item_count > 0 ?
                                (item_count + columns - 1) / columns : 1;
    }

    int card_area_height = content_height - metrics->page_indicator_height;
    metrics->page_capacity = columns * metrics->visible_rows;
    metrics->page_count = item_count > 0 ?
                          (item_count + metrics->page_capacity - 1) / metrics->page_capacity : 1;
    metrics->card_width = (screen_width - (columns + 1) * metrics->margin) / columns;
    metrics->card_height = (card_area_height - (metrics->visible_rows + 1) * metrics->margin) / metrics->visible_rows;
    if (metrics->card_width < 1) metrics->card_width = 1;
    if (metrics->card_height < 1) metrics->card_height = 1;
    if (kind == MAIN_MENU_LAYOUT_COMPACT) {
        int compact_height = screen_width <= 160 || content_height <= 120 ? 22 :
                             screen_width >= 320 ? 32 : 28;
        metrics->margin = screen_width <= 160 || content_height <= 120 ? 2 : 4;
        metrics->card_width = (screen_width - (columns + 1) * metrics->margin) / columns;
        metrics->card_height = (card_area_height - (metrics->visible_rows + 1) * metrics->margin) /
                               metrics->visible_rows;
        if (metrics->card_width < 1) metrics->card_width = 1;
        if (metrics->card_height < 1) metrics->card_height = 1;
        if (metrics->card_height > compact_height) metrics->card_height = compact_height;
    }

    if (kind == MAIN_MENU_LAYOUT_LAUNCHER || kind == MAIN_MENU_LAYOUT_COMPACT) {
        metrics->container_align = LV_ALIGN_TOP_MID;
        metrics->container_y = status_bar_height;
    }

    if (large_screen) {
        /* Physical viewport width remains screen_width; page offsets use the
         * inset container width. Both launchers consume the same geometry. */
        metrics->container_width = LV_MIN(screen_width - 64, 880);
        if (kind == MAIN_MENU_LAYOUT_LIST) {
            metrics->container_width = LV_MIN(screen_width - 64, 720);
        }
        if (kind == MAIN_MENU_LAYOUT_LAUNCHER || kind == MAIN_MENU_LAYOUT_COMPACT) {
            bool compact = kind == MAIN_MENU_LAYOUT_COMPACT;
            metrics->columns = metrics->container_width >= 840 ? 5 :
                               metrics->container_width >= 640 ? 4 : 3;
            metrics->margin = compact ? 8 : 16;
            metrics->page_indicator_height = 28;
            int area_h = content_height - metrics->page_indicator_height;
            metrics->visible_rows = compact ? LV_MAX(1, (area_h - 8) / 56) :
                                             (portrait ? 4 : 2);
            metrics->rows = LV_MAX(1, (item_count + metrics->columns - 1) / metrics->columns);
            metrics->page_capacity = metrics->columns * metrics->visible_rows;
            metrics->page_count = LV_MAX(1, (item_count + metrics->page_capacity - 1) / metrics->page_capacity);
            metrics->card_width = (metrics->container_width - (metrics->columns + 1) * metrics->margin) / metrics->columns;
            metrics->card_height = (area_h - (metrics->visible_rows + 1) * metrics->margin) / metrics->visible_rows;
            metrics->card_height = LV_MIN(metrics->card_height, compact ? 48 : 184);
        }
    }
}

void main_menu_layout_get_metrics(main_menu_layout_kind_t kind, int item_count, main_menu_layout_metrics_t *metrics) {
    main_menu_layout_get_metrics_for_size(kind, item_count, LV_HOR_RES, LV_VER_RES,
                                          GUI_STATUS_BAR_H, metrics);
}
