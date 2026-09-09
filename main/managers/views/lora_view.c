// lora_view.c - touch-first LoRa dashboard, nodes, messages and composer.

#include "managers/views/lora_view.h"
#include "sdkconfig.h"

#if defined(CONFIG_WITH_SCREEN) && defined(CONFIG_HAS_LORA)

#include "esp_timer.h"
#include "gui/gui_router.h"
#include "gui/lvgl_safe.h"
#include "gui/options_view.h"
#include "gui/design_tokens.h"
#include "gui/toast.h"
#include "managers/display_manager.h"
#include "managers/lora_manager.h"
#include "managers/lora_channels.h"
#include "managers/lora_modem.h"
#include "managers/lora_mesh.h"
#include "managers/views/keyboard_screen.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LORA_UI_NODES_MAX 8
#define LORA_UI_TAP_SLOP 12

typedef enum {
    PAGE_MAIN = 0,
    PAGE_ACTIVITY,
    PAGE_SETTINGS,
    PAGE_MODEM,
    PAGE_DEVICE,
    PAGE_CHANNELS,
    PAGE_INFO,
    PAGE_NODES,
    PAGE_NODE,
    PAGE_MESSAGES,
    PAGE_MESSAGE,
} lora_page_t;

typedef enum {
    ACT_RADIO = 1,
    ACT_MESSAGES,
    ACT_PUBLIC,
    ACT_NODES,
    ACT_ACTIVITY,
    ACT_REGION,
    ACT_TX,
    ACT_COMPANION,
    ACT_DISCOVER,
    ACT_BACK,
    ACT_NODE_DM = 40,
    ACT_NODE_INFO,
    ACT_NODE_FAVORITE,
    ACT_NODE_MUTE,
    ACT_NODE_IGNORE,
    ACT_NODE_BACK,
    ACT_NODE_PREV,
    ACT_NODE_NEXT,
    ACT_MESSAGE_REPLY,
    ACT_MESSAGE_BACK,
    ACT_NEW_DM = 60,
    ACT_SETTINGS = 61,
    ACT_INFO = 62,
    ACT_MODEM = 63,
    ACT_DEVICE = 64,
    ACT_CHANNELS = 65,
    ACT_MODEM_MODE = 66,
    ACT_MODEM_PRESET,
    ACT_MODEM_SF,
    ACT_MODEM_BW,
    ACT_MODEM_CR,
    ACT_MODEM_OFFSET,
    ACT_MODEM_OVERRIDE,
    ACT_MODEM_CHANNEL,
    ACT_DEVICE_TX,
    ACT_DEVICE_HOP,
    ACT_DEVICE_ROLE,
    ACT_CHANNEL_BASE = 300,
    ACT_NODE_BASE = 100,
    ACT_MESSAGE_BASE = 200,
} lora_action_t;

enum {
    MAIN_CHAT = 0,
    MAIN_NODES,
    MAIN_SETTINGS,
    MAIN_INFO,
    MAIN_BACK,
    MAIN_COUNT,
};

static options_view_t *s_ov;
static lv_timer_t *s_timer;
static touch_drag_t s_touch;
static lora_page_t s_page;
static lora_page_t s_resume_page;
static bool s_resume_pending;
static bool s_compose_dm;
static uint32_t s_compose_node;
static uint32_t s_selected_node;
static uint32_t s_visible_node_ids[LORA_UI_NODES_MAX];
static uint16_t s_visible_node_count;
static uint16_t s_visible_node_total;
static uint16_t s_node_offset;
static uint16_t s_visible_message_count;
static uint32_t s_conversations[CONFIG_LORA_MSG_RING + 1];
static uint32_t s_conversation;
static uint32_t s_chat_signature;
static bool s_node_picker;
static uint32_t s_chat_seq;
static uint8_t s_unread_count;
static char s_chat_preview[56];
static char s_pending_notice[64];
static uint8_t s_pending_notice_type;

static const int TX_STEPS[] = {2, 5, 10, 14, 17, 20, 22};
#define TX_NSTEPS (sizeof(TX_STEPS) / sizeof(TX_STEPS[0]))

static void rebuild_page(void);
static void action_click(lv_event_t *e);
static void open_composer(bool dm, uint32_t node, lora_page_t return_page);
static void set_page(lora_page_t page);

static bool in_conversation(const lora_msg_t *m) {
    return s_conversation ? m->direct && m->node_num == s_conversation : !m->direct;
}

static void notice(const char *text, uint8_t type) {
    if (text && text[0]) toast_show_duration(text, type, 1800);
}

static void notice_after_return(const char *text, uint8_t type) {
    snprintf(s_pending_notice, sizeof(s_pending_notice), "%s", text ? text : "");
    s_pending_notice_type = type;
}

static const char *node_name(const lora_mesh_node_t *node, char *out, size_t cap) {
    if (node->has_user && node->long_name[0])
        snprintf(out, cap, "%s", node->long_name);
    else if (node->has_user && node->short_name[0])
        snprintf(out, cap, "%s", node->short_name);
    else
        snprintf(out, cap, "Meshtastic !%08X", (unsigned)node->node_num);
    return out;
}

static void message_name(const lora_msg_t *message, char *out, size_t cap) {
    if (!message->direct) {
        snprintf(out, cap, "%.23s", message->outgoing ? "you" :
                 (message->who[0] ? message->who : "unknown"));
        return;
    }
    if (message->direct && message->node_num) {
        lora_mesh_node_t node;
        if (lora_mesh_node_get(message->node_num, &node)) {
            node_name(&node, out, cap);
            return;
        }
        if (message->outgoing) {
            snprintf(out, cap, "DM !%08X", (unsigned)message->node_num);
            return;
        }
    }
    const char *who = message->who[0] ? message->who : "unknown";
    if (!message->outgoing && strncmp(who, "DM:", 3) == 0) who += 3;
    snprintf(out, cap, "%.23s", who);
}

static void message_age(const lora_msg_t *message, char *out, size_t cap) {
    if (!message->timestamp_ms) {
        snprintf(out, cap, "--");
        return;
    }
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t age = (now - message->timestamp_ms) / 1000;
    if (age < 10) snprintf(out, cap, "now");
    else if (age < 120) snprintf(out, cap, "%us", (unsigned)age);
    else if (age < 7200) snprintf(out, cap, "%um", (unsigned)(age / 60));
    else snprintf(out, cap, "%uh", (unsigned)(age / 3600));
}

static void seen_text(const lora_mesh_node_t *node, char *out, size_t cap) {
    if (!node->last_seen_ms) {
        snprintf(out, cap, "Last seen: unknown");
        return;
    }
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t age_s = (now - node->last_seen_ms) / 1000;
    if (age_s < 10) snprintf(out, cap, "Last seen: now");
    else if (age_s < 120) snprintf(out, cap, "Last seen: %us ago", (unsigned)age_s);
    else if (age_s < 7200) snprintf(out, cap, "Last seen: %um ago", (unsigned)(age_s / 60));
    else snprintf(out, cap, "Last seen: %uh ago", (unsigned)(age_s / 3600));
}

static lv_obj_t *add_row(const char *label, int action) {
    lv_obj_t *row = options_view_add_item(s_ov, label, action_click,
                                          (void *)(intptr_t)action);
    if (!row) return NULL;
    /* A fixed row must not capture the swipe itself. HackChat uses this same
     * child-off/parent-on arrangement so the containing list receives drag. */
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    return row;
}

static void style_message_bubble(lv_obj_t *bubble, lv_obj_t *list,
                                 const char *text, bool outgoing) {
    if (!bubble || !list) return;
    lv_obj_t *label = lv_obj_get_child(bubble, 0);
    if (!label) return;

    /* A percentage-width label inside a content-height list button creates a
     * circular LVGL layout dependency. It leaves the stock one-line height in
     * place and LV_LABEL_LONG_DOT renders only "...". Resolve the width in
     * pixels, make the label wrap, then give the bubble the measured height. */
    lv_obj_update_layout(list);
    lv_coord_t available = lv_obj_get_content_width(list);
    if (available <= 0) available = GUI_OPTIONS_LIST_WIDTH - 2 * GUI_OPTIONS_LIST_PAD_HOR;
    lv_coord_t bubble_w = (available * 88) / 100;
    lv_coord_t pad_h = GUI_GRID;
    lv_coord_t pad_v = GUI_GRID;
    lv_coord_t text_w = bubble_w - 2 * pad_h;
    if (text_w < 32) text_w = 32;

    lv_label_set_recolor(label, false);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_size(label, text_w, LV_SIZE_CONTENT);
    const lv_font_t *font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    lv_point_t text_size = {0};
    lv_txt_get_size(&text_size, text ? text : "", font,
                    lv_obj_get_style_text_letter_space(label, LV_PART_MAIN),
                    lv_obj_get_style_text_line_space(label, LV_PART_MAIN),
                    text_w, LV_TEXT_FLAG_NONE);

    lv_coord_t bubble_h = text_size.y + 2 * pad_v;
    if (bubble_h < GUI_CONTROL_H) bubble_h = GUI_CONTROL_H;
    lv_obj_set_size(bubble, bubble_w, bubble_h);
    lv_obj_set_style_pad_left(bubble, pad_h, 0);
    lv_obj_set_style_pad_right(bubble, pad_h, 0);
    lv_obj_set_style_pad_top(bubble, pad_v, 0);
    lv_obj_set_style_pad_bottom(bubble, pad_v, 0);
    lv_obj_set_flex_align(bubble, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_translate_x(bubble, outgoing ? available - bubble_w : 0, 0);
}

static void configure_list(void) {
    lv_obj_t *list = options_view_get_list(s_ov);
    if (!list) return;
    lv_coord_t side_pad = s_page == PAGE_MESSAGE ? GUI_GRID : GUI_OPTIONS_LIST_PAD_HOR;
    lv_obj_set_style_pad_left(list, side_pad, 0);
    lv_obj_set_style_pad_right(list, side_pad, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLL_ELASTIC);
}

static void set_page(lora_page_t page) {
    s_page = page;
    if (page == PAGE_MESSAGES) s_unread_count = 0;
    touch_drag_reset(&s_touch);
    rebuild_page();
}

static void update_row_text(int index, const char *text) {
    lv_obj_t *list = options_view_get_list(s_ov);
    lv_obj_t *row = list ? lv_obj_get_child(list, index) : NULL;
    lv_obj_t *label = row ? lv_obj_get_child(row, 0) : NULL;
    const char *current = label ? lv_label_get_text(label) : NULL;
    /* Avoid a label reallocation and redraw every second when telemetry did
     * not change. This matters on the no-PSRAM Heltec builds. */
    if (!current || strcmp(current, text ? text : "") != 0)
        options_view_update_item_text(s_ov, index, text);
}

static void main_label(int row, char *out, size_t cap) {
    lora_status_t st = {0};
    lora_manager_get_status(&st);
    switch (row) {
    case MAIN_CHAT:
        if (s_unread_count)
            snprintf(out, cap, "Messages: %u new - %.56s",
                     (unsigned)s_unread_count, s_chat_preview);
        else if (s_chat_preview[0]) snprintf(out, cap, "Messages: %s", s_chat_preview);
        else snprintf(out, cap, "Messages: none yet");
        break;
    case MAIN_NODES:
        snprintf(out, cap, "Nodes: %u", (unsigned)lora_manager_node_count());
        break;
    case MAIN_SETTINGS: snprintf(out, cap, "Radio settings"); break;
    case MAIN_INFO:
        snprintf(out, cap, "Device info: RX %u / TX %u",
                 (unsigned)st.rx_ok, (unsigned)st.tx_ok);
        break;
    case MAIN_BACK: snprintf(out, cap, LV_SYMBOL_LEFT " Back"); break;
    default: out[0] = '\0'; break;
    }
}

static void refresh_main(void) {
    if (!s_ov || s_page != PAGE_MAIN) return;
    char label[96];
    for (int i = 0; i < MAIN_COUNT; ++i) {
        main_label(i, label, sizeof(label));
        update_row_text(i, label);
    }
}

static void build_main(void) {
    static const int actions[MAIN_COUNT] = {
        ACT_MESSAGES, ACT_NODES, ACT_SETTINGS, ACT_INFO, ACT_BACK,
    };
    options_view_set_title(s_ov, "LoRa");
    char label[96];
    for (int i = 0; i < MAIN_COUNT; ++i) {
        main_label(i, label, sizeof(label));
        add_row(label, actions[i]);
    }
}

static void activity_label(int row, const lora_status_t *st, char *line, size_t cap) {
    switch (row) {
    case 0:
        snprintf(line, cap, "Radio: %s  %lu.%03lu MHz",
                 st->running ? "ON" : "OFF", (unsigned long)(st->freq_hz / 1000000),
                 (unsigned long)((st->freq_hz / 1000) % 1000));
        break;
    case 1:
        snprintf(line, cap, "Modem: SF%d BW%d CR4/%d", st->sf, st->bw_khz, st->cr);
        break;
    case 2:
        snprintf(line, cap, "Received: %u  duplicates: %u",
                 (unsigned)st->rx_ok, (unsigned)st->rx_dups);
        break;
    case 3:
        snprintf(line, cap, "Sent: %u  failed: %u  relayed: %u",
                 (unsigned)st->tx_ok, (unsigned)st->tx_fail, (unsigned)st->tx_relay);
        break;
    case 4:
        snprintf(line, cap, "Last signal: %d dBm / %.1f dB",
                 st->last_rssi, (double)st->last_snr);
        break;
    default:
        snprintf(line, cap, "Queue drops: %u  duty drops: %u",
                 (unsigned)st->q_drops, (unsigned)st->duty_drops);
        break;
    }
}
static void refresh_activity(void) {
    if (!s_ov || s_page != PAGE_ACTIVITY) return;
    lora_status_t st = {0};
    lora_manager_get_status(&st);
    char line[96];
    for (int i = 0; i < 6; ++i) {
        activity_label(i, &st, line, sizeof(line));
        update_row_text(i, line);
    }
}

static void build_activity(void) {
    lora_status_t st = {0};
    lora_manager_get_status(&st);
    options_view_set_title(s_ov, "LoRa Activity");
    char line[96];
    for (int i = 0; i < 6; ++i) {
        activity_label(i, &st, line, sizeof(line));
        add_row(line, ACT_ACTIVITY);
    }
    add_row(LV_SYMBOL_LEFT " Back", ACT_BACK);
}

static void build_settings(void) {
    lora_status_t st = {0};
    lora_manager_get_status(&st);
    options_view_set_title(s_ov, "Radio Settings");
    char line[96];
    snprintf(line, sizeof(line), "Radio: %s - tap to %s", st.running ? "ON" : "OFF",
             st.running ? "stop" : "start");
    add_row(line, ACT_RADIO);
    snprintf(line, sizeof(line), "Region: %s%s - tap to change",
             lora_region_name((int)st.region),
             lora_manager_region_saved() ? "" : " *");
    add_row(line, ACT_REGION);
    snprintf(line, sizeof(line), "TX power: %d dBm - tap to change", st.tx_dbm);
    add_row(line, ACT_TX);
    snprintf(line, sizeof(line), "Phone link: %s - tap to change",
             st.companion == LORA_COMPANION_WIFI ? "WiFi" : "BLE");
    add_row(line, ACT_COMPANION);
    add_row("Modem and frequency", ACT_MODEM);
    add_row("Device behavior", ACT_DEVICE);
    add_row("Channels", ACT_CHANNELS);
    add_row(LV_SYMBOL_LEFT " Back", ACT_BACK);
}

static const char *device_role_name(int role) {
    switch (role) {
    case LORA_MESH_ROLE_CLIENT: return "Client";
    case LORA_MESH_ROLE_CLIENT_MUTE: return "Client mute";
    case LORA_MESH_ROLE_ROUTER: return "Router";
    case LORA_MESH_ROLE_ROUTER_LATE: return "Router late";
    case LORA_MESH_ROLE_CLIENT_BASE: return "Client base";
    default: return "Role";
    }
}

static void build_modem(void) {
    bool use_preset = false;
    int preset = 0, sf = 11, bw = 250, cr = 5;
    float offset = 0, override = 0;
    uint32_t channel = 0;
    lora_manager_get_modem_cfg(&use_preset, &preset, &sf, &bw, &cr, &offset,
                               &override, &channel, NULL);
    options_view_set_title(s_ov, "Modem & Frequency");
    char line[104];
    snprintf(line, sizeof(line), "Mode: %s - tap to switch", use_preset ? "preset" : "custom");
    add_row(line, ACT_MODEM_MODE);
    snprintf(line, sizeof(line), "Preset: %s - tap to cycle",
             lora_preset_display_name(preset, use_preset));
    add_row(line, ACT_MODEM_PRESET);
    if (!use_preset) {
        snprintf(line, sizeof(line), "Spread factor: SF%d - tap to cycle", sf);
        add_row(line, ACT_MODEM_SF);
        snprintf(line, sizeof(line), "Bandwidth: %d kHz - tap to cycle", bw);
        add_row(line, ACT_MODEM_BW);
        snprintf(line, sizeof(line), "Coding rate: CR4/%d - tap to cycle", cr);
        add_row(line, ACT_MODEM_CR);
    }
    snprintf(line, sizeof(line), "Frequency offset: %.1f MHz - tap to cycle", (double)offset);
    add_row(line, ACT_MODEM_OFFSET);
    snprintf(line, sizeof(line), "Channel slot: %u - tap to cycle", (unsigned)channel);
    add_row(line, ACT_MODEM_CHANNEL);
    snprintf(line, sizeof(line), "Frequency override: %.1f MHz - tap to cycle", (double)override);
    add_row(line, ACT_MODEM_OVERRIDE);
    add_row(LV_SYMBOL_LEFT " Back", ACT_BACK);
}

static void build_device(void) {
    lora_status_t st = {0};
    lora_manager_get_status(&st);
    bool tx_enabled = true;
    lora_manager_get_modem_cfg(NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, &tx_enabled);
    options_view_set_title(s_ov, "Device Behavior");
    char line[104];
    snprintf(line, sizeof(line), "TX enabled: %s - tap to toggle", tx_enabled ? "yes" : "no");
    add_row(line, ACT_DEVICE_TX);
    snprintf(line, sizeof(line), "TX power: %d dBm - tap to cycle", st.tx_dbm);
    add_row(line, ACT_TX);
    snprintf(line, sizeof(line), "Hop limit: %d - tap to cycle", st.hop_limit);
    add_row(line, ACT_DEVICE_HOP);
    snprintf(line, sizeof(line), "Role: %s (%d) - tap to cycle", device_role_name(st.role), st.role);
    add_row(line, ACT_DEVICE_ROLE);
    add_row("Request NodeInfo now", ACT_DISCOVER);
    add_row(LV_SYMBOL_LEFT " Back", ACT_BACK);
}

static void build_channels(void) {
    options_view_set_title(s_ov, "Channels");
    char line[104];
    for (uint8_t i = 0; i < LORA_CH_MAX; ++i) {
        const lora_channel_t *ch = lora_channel_get(i);
        if (!ch || !ch->used || ch->role == LORA_CH_DISABLED) {
            snprintf(line, sizeof(line), "Channel %u: disabled  (app adds)", (unsigned)i);
        } else {
            snprintf(line, sizeof(line), "Channel %u: %s  %s  hash %02X",
                     (unsigned)i, ch->name[0] ? ch->name : "Default",
                     ch->role == LORA_CH_PRIMARY ? "primary" : "secondary",
                     (unsigned)ch->hash);
        }
        add_row(line, ACT_CHANNEL_BASE + i);
    }
    add_row("Channel changes are saved immediately", ACT_CHANNELS);
    add_row(LV_SYMBOL_LEFT " Back", ACT_BACK);
}

static void build_info(void) {
    lora_status_t st = {0};
    lora_manager_get_status(&st);
    options_view_set_title(s_ov, "Device Info");
    char line[104];
    snprintf(line, sizeof(line), "Node ID: !%08X", (unsigned)lora_mesh_node_num());
    add_row(line, ACT_INFO);
    snprintf(line, sizeof(line), "Radio: %s  %lu.%03lu MHz",
             st.running ? "ON" : "OFF", (unsigned long)(st.freq_hz / 1000000),
             (unsigned long)((st.freq_hz / 1000) % 1000));
    add_row(line, ACT_INFO);
    snprintf(line, sizeof(line), "Modem: SF%d BW%d CR4/%d", st.sf, st.bw_khz, st.cr);
    add_row(line, ACT_INFO);
    snprintf(line, sizeof(line), "Region: %s  channel %u",
             lora_region_name((int)st.region), (unsigned)st.channel_num);
    add_row(line, ACT_INFO);
    snprintf(line, sizeof(line), "Known nodes: %u", (unsigned)lora_manager_node_count());
    add_row(line, ACT_INFO);
    add_row("Traffic details", ACT_ACTIVITY);
    add_row("Refresh node discovery", ACT_DISCOVER);
    add_row(LV_SYMBOL_LEFT " Back", ACT_BACK);
}

static void node_row_label(const lora_mesh_node_t *node, char *out, size_t cap) {
    char name[32];
    node_name(node, name, sizeof(name));
    snprintf(out, cap, "%s  !%08X  %d dBm%s%s", name,
             (unsigned)node->node_num, (int)node->last_rssi,
             node->has_pubkey ? "  PKI" : "",
             s_node_picker ? "  select" : "");
}

static void build_nodes(void) {
    options_view_set_title(s_ov, s_node_picker ? "Choose DM contact" : "LoRa Nodes");
    uint16_t total = lora_mesh_nodes(NULL, 0);
    s_visible_node_total = total;
    if (total == 0) s_node_offset = 0;
    else if (s_node_offset >= total)
        s_node_offset = (uint16_t)(((total - 1) / LORA_UI_NODES_MAX) * LORA_UI_NODES_MAX);
    uint16_t remaining = total > s_node_offset ? (uint16_t)(total - s_node_offset) : 0;
    s_visible_node_count = remaining < LORA_UI_NODES_MAX ? remaining : LORA_UI_NODES_MAX;
    char line[112];
    if (total) {
        snprintf(line, sizeof(line), "Refresh - showing %u-%u of %u",
                 (unsigned)(s_node_offset + 1),
                 (unsigned)(s_node_offset + s_visible_node_count), (unsigned)total);
        add_row(line, ACT_DISCOVER);
    } else {
        add_row("Refresh discovery", ACT_DISCOVER);
    }
    for (uint16_t i = 0; i < s_visible_node_count; ++i) {
        lora_mesh_node_t node;
        if (!lora_mesh_node_at((uint16_t)(s_node_offset + i), &node)) break;
        s_visible_node_ids[i] = node.node_num;
        node_row_label(&node, line, sizeof(line));
        add_row(line, ACT_NODE_BASE + i);
    }
    if (total == 0) {
        add_row("No nodes yet - tap to request NodeInfo", ACT_DISCOVER);
    }
    if (s_node_offset > 0) add_row(LV_SYMBOL_LEFT " Previous nodes", ACT_NODE_PREV);
    if (s_node_offset + s_visible_node_count < total)
        add_row("Next nodes", ACT_NODE_NEXT);
    add_row(LV_SYMBOL_LEFT " Back", s_node_picker ? ACT_NODE_BACK : ACT_BACK);
}

static void refresh_nodes(void) {
    if (!s_ov || s_page != PAGE_NODES) return;
    uint16_t total = lora_mesh_nodes(NULL, 0);
    if (total != s_visible_node_total) {
        rebuild_page();
        return;
    }
    uint16_t remaining = total > s_node_offset ? (uint16_t)(total - s_node_offset) : 0;
    uint16_t visible = remaining < LORA_UI_NODES_MAX ? remaining : LORA_UI_NODES_MAX;
    if (visible != s_visible_node_count) {
        rebuild_page();
        return;
    }
    char line[112];
    for (uint16_t i = 0; i < visible; ++i) {
        lora_mesh_node_t node;
        if (!lora_mesh_node_at((uint16_t)(s_node_offset + i), &node) ||
            node.node_num != s_visible_node_ids[i]) {
            rebuild_page();
            return;
        }
        node_row_label(&node, line, sizeof(line));
        update_row_text(1 + i, line);
    }
}

static void build_node(void) {
    lora_mesh_node_t node;
    if (!lora_mesh_node_get(s_selected_node, &node)) {
        notice("Node is no longer in the database", TOAST_WARN);
        s_page = PAGE_NODES;
        build_nodes();
        return;
    }
    char name[32];
    node_name(&node, name, sizeof(name));
    options_view_set_title(s_ov, name);
    char line[104];
    snprintf(line, sizeof(line), "Node ID: !%08X", (unsigned)node.node_num);
    add_row(line, ACT_NODE_INFO);
    if (node.has_user && node.short_name[0]) {
        snprintf(line, sizeof(line), "Short name: %s", node.short_name);
        add_row(line, ACT_NODE_INFO);
    }
    seen_text(&node, line, sizeof(line));
    add_row(line, ACT_NODE_INFO);
    snprintf(line, sizeof(line), "Signal: %d dBm / %.1f dB", node.last_rssi,
             (double)node.last_snr);
    add_row(line, ACT_NODE_INFO);
    if (node.hops_valid)
        snprintf(line, sizeof(line), "Route: %u hop%s  channel %u",
                 (unsigned)node.hops_away, node.hops_away == 1 ? "" : "s",
                 (unsigned)node.channel);
    else
        snprintf(line, sizeof(line), "Route: unknown  channel %u", (unsigned)node.channel);
    add_row(line, ACT_NODE_INFO);
    snprintf(line, sizeof(line), "Identity key: %s%s",
             node.has_pubkey ? "available" : "unavailable",
             node.key_verified ? " / verified" : "");
    add_row(line, ACT_NODE_INFO);
    add_row(node.has_pubkey ? "Send encrypted DM" : "Get DM key (request NodeInfo)",
            node.has_pubkey ? ACT_NODE_DM : ACT_NODE_INFO);
    add_row("Request fresh NodeInfo", ACT_NODE_INFO);
    snprintf(line, sizeof(line), "Favorite: %s - tap to toggle", node.favorite ? "yes" : "no");
    add_row(line, ACT_NODE_FAVORITE);
    snprintf(line, sizeof(line), "Muted: %s - tap to toggle", node.muted ? "yes" : "no");
    add_row(line, ACT_NODE_MUTE);
    snprintf(line, sizeof(line), "Ignored: %s - tap to toggle", node.ignored ? "yes" : "no");
    add_row(line, ACT_NODE_IGNORE);
    add_row(LV_SYMBOL_LEFT " Nodes", ACT_NODE_BACK);
}

static void build_messages(void) {
    options_view_set_title(s_ov, "Conversations");
    s_visible_message_count = 1;
    s_conversations[0] = 0;
    uint16_t count = lora_manager_msg_count();
    for (uint16_t i = count; i > 0; --i) {
        lora_msg_t m;
        if (!lora_manager_msg_at(i - 1, &m) || !m.direct || !m.node_num) continue;
        bool found = false;
        for (uint16_t j = 0; j < s_visible_message_count; ++j)
            if (s_conversations[j] == m.node_num) found = true;
        if (!found) s_conversations[s_visible_message_count++] = m.node_num;
    }
    char line[104];
    for (uint16_t c = 0; c < s_visible_message_count; ++c) {
        unsigned unread = 0;
        for (uint16_t i = 0; i < count; ++i) {
            lora_msg_t m;
            if (lora_manager_msg_at(i, &m) && !m.read &&
                (c ? m.direct && m.node_num == s_conversations[c] : !m.direct)) unread++;
        }
        snprintf(line, sizeof(line), c ? "Direct message" : "Public chat");
        for (uint16_t i = count; i > 0; --i) {
            lora_msg_t m;
            if (!lora_manager_msg_at(i - 1, &m)) continue;
            if (c ? (!m.direct || m.node_num != s_conversations[c]) : m.direct) continue;
            char name[24], age[12];
            message_name(&m, name, sizeof(name));
            message_age(&m, age, sizeof(age));
            if (unread) {
                snprintf(line, sizeof(line), "%.20s  %.8s  %u new  %.28s",
                         c ? name : "Public chat", age, unread, m.text);
            } else {
                snprintf(line, sizeof(line), "%.20s  %.8s  %.36s",
                         c ? name : "Public chat", age, m.text);
            }
            break;
        }
        add_row(line, ACT_MESSAGE_BASE + c);
    }
    add_row("New direct message", ACT_NEW_DM);
    add_row(LV_SYMBOL_LEFT " Back", ACT_BACK);
}

static void build_message(void) {
    char title[32] = "Public chat";
    if (s_conversation) {
        lora_msg_t peer = {.direct = true, .outgoing = true, .node_num = s_conversation};
        message_name(&peer, title, sizeof(title));
    }
    options_view_set_title(s_ov, title);
    lora_manager_chat_read(s_conversation);
    lv_obj_t *list = options_view_get_list(s_ov);
    /* Conversation bubbles use the screen width; the generic options view
     * reserves menu gutters that otherwise appear as a large empty strip. */
    lv_obj_set_style_pad_left(list, GUI_GRID, 0);
    lv_obj_set_style_pad_right(list, GUI_GRID, 0);
    uint16_t count = lora_manager_msg_count();
    for (uint16_t i = 0; i < count; ++i) {
        lora_msg_t m;
        if (!lora_manager_msg_at(i, &m) || !in_conversation(&m)) continue;
        char name[24], age[12], text[224];
        message_name(&m, name, sizeof(name));
        message_age(&m, age, sizeof(age));
        const char *state = !m.outgoing ? "" : m.delivery == 1 ? "Pending" :
                            m.delivery == 2 ? "Delivered" : m.delivery == 3 ? "Failed" : "Sent";
        snprintf(text, sizeof(text), "%.23s  %.11s  %.9s\n%.159s",
                 m.outgoing ? "You" : name, age, state, m.text[0] ? m.text : "(empty message)");
        lv_obj_t *bubble = add_row(text, 0);
        if (!bubble) continue;
        lv_obj_set_style_radius(bubble, 12, 0);
        lv_obj_set_style_border_width(bubble, m.outgoing ? 2 : 0, 0);
        style_message_bubble(bubble, list, text, m.outgoing);
    }
    if (!options_view_get_item_count(s_ov))
        add_row("No messages yet", 0);
    add_row("Write a message", ACT_MESSAGE_REPLY);
    add_row(LV_SYMBOL_LEFT " Back to conversations", ACT_MESSAGE_BACK);
    lv_obj_update_layout(list);
    lv_obj_scroll_to_y(list, LV_COORD_MAX, LV_ANIM_OFF);
}

static void rebuild_page(void) {
    if (!s_ov) return;
    options_view_clear(s_ov);
    switch (s_page) {
    case PAGE_MAIN: build_main(); break;
    case PAGE_ACTIVITY: build_activity(); break;
    case PAGE_SETTINGS: build_settings(); break;
    case PAGE_MODEM: build_modem(); break;
    case PAGE_DEVICE: build_device(); break;
    case PAGE_CHANNELS: build_channels(); break;
    case PAGE_INFO: build_info(); break;
    case PAGE_NODES: build_nodes(); break;
    case PAGE_NODE: build_node(); break;
    case PAGE_MESSAGES: build_messages(); break;
    case PAGE_MESSAGE: build_message(); break;
    }
    if (s_page != PAGE_MESSAGE) options_view_set_selected(s_ov, 0);
    configure_list();
}

static void go_back(void) {
    switch (s_page) {
    case PAGE_NODE: set_page(PAGE_NODES); break;
    case PAGE_MESSAGE: set_page(PAGE_MESSAGES); break;
    case PAGE_ACTIVITY: set_page(PAGE_INFO); break;
    case PAGE_SETTINGS:
    case PAGE_MODEM:
    case PAGE_DEVICE:
    case PAGE_CHANNELS:
    case PAGE_INFO:
    case PAGE_NODES:
    case PAGE_MESSAGES:
        set_page(PAGE_MAIN);
        break;
    case PAGE_MAIN:
    default:
        gui_router_back();
        break;
    }
}

static void cycle_region(void) {
    lora_status_t st = {0};
    lora_manager_get_status(&st);
    bool ok = lora_manager_set_region((lora_region_t)lora_region_next((int)st.region));
    notice(ok ? "Region saved" : lora_manager_last_error(),
           ok ? TOAST_SUCCESS : TOAST_ERROR);
    rebuild_page();
}

static void cycle_tx(void) {
    lora_status_t st = {0};
    lora_manager_get_status(&st);
    lora_hw_t hw;
    int max = lora_manager_get_hw(&hw) ? hw.max_tx_dbm : 22;
    int next = TX_STEPS[0];
    for (unsigned i = 0; i < TX_NSTEPS; ++i) {
        if (TX_STEPS[i] > st.tx_dbm) { next = TX_STEPS[i]; break; }
    }
    if (next > max || st.tx_dbm >= max) next = TX_STEPS[0];
    bool ok = lora_manager_set_params(st.sf, st.bw_khz, next);
    notice(ok ? "TX power saved" : lora_manager_last_error(),
           ok ? TOAST_SUCCESS : TOAST_ERROR);
    rebuild_page();
}

static void compose_submit(const char *text) {
    keyboard_view_set_submit_callback(NULL);
    keyboard_view_set_immediate_callback(NULL);
    bool ok = false;
    if (text && text[0]) {
        ok = s_compose_dm
                 ? lora_manager_send_dm_text(text, s_compose_node, 0, true, NULL)
                 : lora_manager_send_text(text);
    }
    if (!text || !text[0])
        notice_after_return("Empty message not sent", TOAST_INFO);
    else if (ok)
        notice_after_return(s_compose_dm ? "Encrypted DM sent" : "Public message sent",
                            TOAST_SUCCESS);
    else
        notice_after_return(lora_manager_last_error(), TOAST_ERROR);
    display_manager_go_back();
}

static void open_composer(bool dm, uint32_t node, lora_page_t return_page) {
    if (!lora_manager_is_running()) {
        notice("Start the LoRa radio first", TOAST_WARN);
        return;
    }
    s_compose_dm = dm;
    s_compose_node = node;
    s_resume_page = return_page;
    s_resume_pending = true;
    keyboard_view_set_return_view(&lora_view);
    keyboard_view_set_submit_callback(compose_submit);
    keyboard_view_set_immediate_callback(NULL);
    keyboard_view_set_placeholder(dm ? "Encrypted direct message" : "Public message");
    keyboard_view_set_initial_text("");
    keyboard_view_set_start_caps(true);
    display_manager_switch_view(&keyboard_view);
}

static void action_click(lv_event_t *e) {
    int action = (int)(intptr_t)lv_event_get_user_data(e);
    if (action >= ACT_NODE_BASE && action < ACT_NODE_BASE + LORA_UI_NODES_MAX) {
        uint16_t i = (uint16_t)(action - ACT_NODE_BASE);
        if (i < s_visible_node_count) {
            s_selected_node = s_visible_node_ids[i];
            if (s_node_picker) {
                s_conversation = s_selected_node;
                s_node_picker = false;
                set_page(PAGE_MESSAGE);
            } else {
                set_page(PAGE_NODE);
            }
        }
        return;
    }
    if (action >= ACT_MESSAGE_BASE && action <= ACT_MESSAGE_BASE + CONFIG_LORA_MSG_RING) {
        uint16_t visual = (uint16_t)(action - ACT_MESSAGE_BASE);
        if (visual < s_visible_message_count) {
            s_conversation = s_conversations[visual];
            set_page(PAGE_MESSAGE);
        }
        return;
    }
    if (action >= ACT_CHANNEL_BASE && action < ACT_CHANNEL_BASE + LORA_CH_MAX) {
        uint8_t idx = (uint8_t)(action - ACT_CHANNEL_BASE);
        const lora_channel_t *ch = lora_channel_get(idx);
        bool ok = false;
        if (ch && ch->used && ch->role != LORA_CH_DISABLED) {
            if (ch->role == LORA_CH_PRIMARY) {
                notice("Primary channel is protected", TOAST_INFO);
                return;
            }
            ok = lora_manager_disable_channel(idx);
        }
        notice(ok ? "Channel role saved" : "Use the Meshtastic app to add this channel",
               ok ? TOAST_SUCCESS : TOAST_INFO);
        rebuild_page();
        return;
    }

    switch ((lora_action_t)action) {
    case ACT_RADIO: {
        bool stopping = lora_manager_is_running();
        bool ok = true;
        if (stopping) lora_manager_stop();
        else ok = lora_manager_start();
        notice(ok ? (stopping ? "LoRa stopped" : "LoRa started")
                  : lora_manager_last_error(), ok ? TOAST_SUCCESS : TOAST_ERROR);
        rebuild_page();
        break;
    }
    case ACT_MESSAGES: set_page(PAGE_MESSAGES); break;
    case ACT_PUBLIC: s_conversation = 0; set_page(PAGE_MESSAGE); break;
    case ACT_NODES:
        s_node_picker = false;
        s_node_offset = 0;
        set_page(PAGE_NODES);
        break;
    case ACT_SETTINGS: set_page(PAGE_SETTINGS); break;
    case ACT_INFO: set_page(PAGE_INFO); break;
    case ACT_MODEM: set_page(PAGE_MODEM); break;
    case ACT_DEVICE: set_page(PAGE_DEVICE); break;
    case ACT_CHANNELS: set_page(PAGE_CHANNELS); break;
    case ACT_ACTIVITY:
        set_page(PAGE_ACTIVITY);
        break;
    case ACT_REGION: cycle_region(); break;
    case ACT_TX: cycle_tx(); break;
    case ACT_COMPANION: {
        lora_status_t st = {0};
        lora_manager_get_status(&st);
        bool ok = lora_manager_set_companion(st.companion == LORA_COMPANION_WIFI
                                                 ? LORA_COMPANION_BLE
                                                 : LORA_COMPANION_WIFI);
        notice(ok ? "Phone link changes on next LoRa start" : lora_manager_last_error(),
               ok ? TOAST_SUCCESS : TOAST_ERROR);
        rebuild_page();
        break;
    }
    case ACT_MODEM_MODE: {
        bool use_preset = false;
        int preset = 0, sf = 11, bw = 250, cr = 5;
        lora_manager_get_modem_cfg(&use_preset, &preset, &sf, &bw, &cr,
                                   NULL, NULL, NULL, NULL);
        bool ok = use_preset ? lora_manager_set_modem(sf, bw, cr)
                             : lora_manager_set_preset(preset);
        notice(ok ? (use_preset ? "Custom modem enabled" : "Preset modem enabled")
                  : lora_manager_last_error(), ok ? TOAST_SUCCESS : TOAST_ERROR);
        rebuild_page();
        break;
    }
    case ACT_MODEM_PRESET: {
        bool use_preset = false;
        int preset = 0;
        lora_manager_get_modem_cfg(&use_preset, &preset, NULL, NULL, NULL,
                                   NULL, NULL, NULL, NULL);
        int next = use_preset ? (preset + 1) % LORA_PRESET_COUNT : LORA_PRESET_LONG_FAST;
        bool ok = lora_manager_set_preset(next);
        notice(ok ? "Preset saved" : lora_manager_last_error(),
               ok ? TOAST_SUCCESS : TOAST_ERROR);
        rebuild_page();
        break;
    }
    case ACT_MODEM_SF:
    case ACT_MODEM_BW:
    case ACT_MODEM_CR: {
        bool use_preset = false;
        int preset = 0, sf = 11, bw = 250, cr = 5;
        lora_manager_get_modem_cfg(&use_preset, &preset, &sf, &bw, &cr,
                                   NULL, NULL, NULL, NULL);
        if (action == ACT_MODEM_SF) sf = sf >= 12 ? 7 : sf + 1;
        else if (action == ACT_MODEM_BW) bw = bw == 125 ? 250 : bw == 250 ? 500 : 125;
        else cr = cr >= 8 ? 5 : cr + 1;
        bool ok = lora_manager_set_modem(sf, bw, cr);
        notice(ok ? "Custom modem saved" : lora_manager_last_error(),
               ok ? TOAST_SUCCESS : TOAST_ERROR);
        rebuild_page();
        break;
    }
    case ACT_MODEM_OFFSET: {
        bool use_preset = false, tx_enabled = true;
        int preset = 0, sf = 11, bw = 250, cr = 5;
        float offset = 0, override = 0;
        uint32_t channel = 0;
        lora_manager_get_modem_cfg(&use_preset, &preset, &sf, &bw, &cr, &offset,
                                   &override, &channel, &tx_enabled);
        offset += 1.0f;
        if (offset > 2.0f) offset = -2.0f;
        bool ok = lora_manager_set_freq_offset(offset);
        notice(ok ? "Frequency offset saved" : lora_manager_last_error(),
               ok ? TOAST_SUCCESS : TOAST_ERROR);
        rebuild_page();
        break;
    }
    case ACT_MODEM_OVERRIDE: {
        bool use_preset = false, tx_enabled = true;
        int preset = 0, sf = 11, bw = 250, cr = 5;
        float offset = 0, override = 0;
        uint32_t channel = 0;
        lora_manager_get_modem_cfg(&use_preset, &preset, &sf, &bw, &cr, &offset,
                                   &override, &channel, &tx_enabled);
        const float choices[] = {0, 433.0f, 868.0f, 915.0f};
        int selected = 0;
        for (unsigned i = 0; i < sizeof(choices) / sizeof(choices[0]); ++i)
            if (override == choices[i]) selected = (int)i;
        float next = choices[(selected + 1) % (sizeof(choices) / sizeof(choices[0]))];
        bool ok = lora_manager_set_override_freq(next);
        notice(ok ? "Frequency override saved" : lora_manager_last_error(),
               ok ? TOAST_SUCCESS : TOAST_ERROR);
        rebuild_page();
        break;
    }
    case ACT_MODEM_CHANNEL: {
        bool use_preset = false, tx_enabled = true;
        int preset = 0, sf = 11, bw = 250, cr = 5;
        float offset = 0, override = 0;
        uint32_t channel = 0;
        lora_manager_get_modem_cfg(&use_preset, &preset, &sf, &bw, &cr, &offset,
                                   &override, &channel, &tx_enabled);
        bool ok = lora_manager_set_channel_num(channel >= 7 ? 0 : channel + 1);
        notice(ok ? "Channel slot saved" : lora_manager_last_error(),
               ok ? TOAST_SUCCESS : TOAST_ERROR);
        rebuild_page();
        break;
    }
    case ACT_DEVICE_TX: {
        bool enabled = true;
        lora_manager_get_modem_cfg(NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, &enabled);
        bool ok = lora_manager_set_tx_enabled(!enabled);
        notice(ok ? "TX setting saved" : lora_manager_last_error(),
               ok ? TOAST_SUCCESS : TOAST_ERROR);
        rebuild_page();
        break;
    }
    case ACT_DEVICE_HOP: {
        lora_status_t st = {0};
        lora_manager_get_status(&st);
        bool ok = lora_manager_set_hop_limit(st.hop_limit >= 7 ? 1 : st.hop_limit + 1);
        notice(ok ? "Hop limit saved" : lora_manager_last_error(),
               ok ? TOAST_SUCCESS : TOAST_ERROR);
        rebuild_page();
        break;
    }
    case ACT_DEVICE_ROLE: {
        static const int roles[] = {LORA_MESH_ROLE_CLIENT, LORA_MESH_ROLE_CLIENT_MUTE,
                                    LORA_MESH_ROLE_ROUTER, LORA_MESH_ROLE_ROUTER_LATE,
                                    LORA_MESH_ROLE_CLIENT_BASE};
        int current = lora_manager_get_role();
        unsigned selected = 0;
        for (unsigned i = 0; i < sizeof(roles) / sizeof(roles[0]); ++i)
            if (roles[i] == current) selected = i;
        bool ok = lora_manager_set_role(roles[(selected + 1) % (sizeof(roles) / sizeof(roles[0]))]);
        notice(ok ? "Device role saved" : lora_manager_last_error(),
               ok ? TOAST_SUCCESS : TOAST_ERROR);
        rebuild_page();
        break;
    }
    case ACT_DISCOVER:
        if (!lora_manager_is_running()) {
            notice("Start the LoRa radio first", TOAST_WARN);
            break;
        }
        lora_mesh_request_nodeinfo();
        notice("NodeInfo discovery requested", TOAST_SUCCESS);
        break;
    case ACT_NEW_DM:
        s_node_picker = true;
        s_node_offset = 0;
        set_page(PAGE_NODES);
        break;
    case ACT_NODE_DM: s_conversation = s_selected_node; set_page(PAGE_MESSAGE); break;
    case ACT_NODE_INFO: {
        bool ok = lora_manager_is_running() &&
                  lora_mesh_send_nodeinfo(s_selected_node, true);
        notice(ok ? "NodeInfo request sent" : "Could not send NodeInfo request",
               ok ? TOAST_SUCCESS : TOAST_ERROR);
        break;
    }
    case ACT_NODE_FAVORITE:
    case ACT_NODE_MUTE:
    case ACT_NODE_IGNORE: {
        lora_mesh_node_t node;
        if (!lora_mesh_node_get(s_selected_node, &node)) break;
        bool ok = action == ACT_NODE_FAVORITE
                      ? lora_mesh_peer_set_favorite(node.node_num, !node.favorite)
                      : action == ACT_NODE_MUTE
                            ? lora_mesh_peer_set_muted(node.node_num, !node.muted)
                            : lora_mesh_peer_set_ignored(node.node_num, !node.ignored);
        notice(ok ? "Node preference saved" : "Node preference failed",
               ok ? TOAST_SUCCESS : TOAST_ERROR);
        rebuild_page();
        break;
    }
    case ACT_NODE_BACK:
        if (s_node_picker) {
            s_node_picker = false;
            set_page(PAGE_MESSAGES);
        } else {
            set_page(PAGE_NODES);
        }
        break;
    case ACT_NODE_PREV:
        s_node_offset = s_node_offset >= LORA_UI_NODES_MAX
                            ? (uint16_t)(s_node_offset - LORA_UI_NODES_MAX) : 0;
        rebuild_page();
        break;
    case ACT_NODE_NEXT:
        s_node_offset = (uint16_t)(s_node_offset + LORA_UI_NODES_MAX);
        rebuild_page();
        break;
    case ACT_MESSAGE_REPLY:
        open_composer(s_conversation != 0, s_conversation, PAGE_MESSAGE);
        break;
    case ACT_MESSAGE_BACK: set_page(PAGE_MESSAGES); break;
    case ACT_BACK: go_back(); break;
    default: break;
    }
}

static void poll_messages(bool rebuild_messages) {
    (void)rebuild_messages;
    lora_msg_t last;
    uint32_t seq = 0;
    if (!lora_manager_latest_message(&last, &seq) || seq == s_chat_seq) return;
    uint32_t previous = s_chat_seq;
    if (previous != 0 && s_page != PAGE_MESSAGES && !last.outgoing &&
        s_unread_count < 99) {
        s_unread_count++;
    }
    s_chat_seq = seq;
    snprintf(s_chat_preview, sizeof(s_chat_preview), "%.20s: %.32s",
             last.outgoing ? "you" : (last.who[0] ? last.who : "unknown"),
             last.text[0] ? last.text : "(empty message)");
    if (previous != 0 && !last.outgoing && s_page != PAGE_MESSAGES &&
        s_page != PAGE_MESSAGE) {
        char who[24];
        char toast[64];
        message_name(&last, who, sizeof(who));
        snprintf(toast, sizeof(toast), "New %s from %.16s: %.24s",
                 last.direct ? "DM" : "message", who,
                 last.text[0] ? last.text : "(empty message)");
        notice(toast, TOAST_INFO);
    }
}

static void lora_tick(lv_timer_t *timer) {
    (void)timer;
    poll_messages(true);
    uint32_t signature = 0;
    s_unread_count = 0;
    for (uint16_t i = 0, n = lora_manager_msg_count(); i < n; ++i) {
        lora_msg_t m;
        if (!lora_manager_msg_at(i, &m)) continue;
        signature = signature * 33u + m.timestamp_ms + m.packet_id + m.delivery;
        if (!m.read && s_unread_count < 99) s_unread_count++;
    }
    if (signature != s_chat_signature && !s_touch.started) {
        s_chat_signature = signature;
        if (s_page == PAGE_MESSAGES) {
            int selected = options_view_get_selected(s_ov);
            rebuild_page();
            options_view_set_selected(s_ov, selected);
        } else if (s_page == PAGE_MESSAGE) {
            lv_obj_t *list = options_view_get_list(s_ov);
            lv_coord_t y = lv_obj_get_scroll_y(list);
            bool bottom = lv_obj_get_scroll_bottom(list) < 12;
            rebuild_page();
            lv_obj_scroll_to_y(list, bottom ? LV_COORD_MAX : y, LV_ANIM_OFF);
        }
    }
    switch (s_page) {
    case PAGE_MAIN: refresh_main(); break;
    case PAGE_NODES: refresh_nodes(); break;
    case PAGE_ACTIVITY: refresh_activity(); break;
    default: break;
    }
}

static void activate_selected(void) {
    if (!s_ov) return;
    int selected = options_view_get_selected(s_ov);
    lv_obj_t *list = options_view_get_list(s_ov);
    if (!list || selected < 0 || selected >= options_view_get_item_count(s_ov)) return;
    lv_obj_t *item = lv_obj_get_child(list, selected);
    if (item) lv_event_send(item, LV_EVENT_CLICKED, NULL);
}

static void handle_touch(InputEvent *event) {
    lv_indev_data_t *data = &event->data.touch_data;
    lv_obj_t *list = options_view_get_list(s_ov);
    if (!list) return;
    if (data->state == LV_INDEV_STATE_PR) {
        if (!s_touch.started) touch_drag_begin(&s_touch, data);
        else touch_drag_update(&s_touch, data, list);
        return;
    }
    if (data->state != LV_INDEV_STATE_REL || !s_touch.started) return;
    int start_x = s_touch.start_x;
    int start_y = s_touch.start_y;
    bool dragged = touch_drag_release(&s_touch, data);
    if (dragged || abs(data->point.x - start_x) > LORA_UI_TAP_SLOP ||
        abs(data->point.y - start_y) > LORA_UI_TAP_SLOP) return;
    int count = options_view_get_item_count(s_ov);
    lv_area_t viewport;
    lv_obj_get_coords(list, &viewport);
    if (start_y < viewport.y1 || start_y > viewport.y2 ||
        data->point.y < viewport.y1 || data->point.y > viewport.y2) return;
    for (int i = 0; i < count; ++i) {
        lv_obj_t *item = lv_obj_get_child(list, i);
        if (!item) continue;
        lv_area_t area;
        lv_obj_get_coords(item, &area);
        bool start_inside = start_x >= area.x1 && start_x <= area.x2 &&
                            start_y >= area.y1 && start_y <= area.y2;
        bool end_inside = data->point.x >= area.x1 && data->point.x <= area.x2 &&
                          data->point.y >= area.y1 && data->point.y <= area.y2;
        if (start_inside && end_inside) {
            options_view_set_selected(s_ov, i);
            lv_event_send(item, LV_EVENT_CLICKED, NULL);
            return;
        }
    }
}

static void move_selection(int delta) {
    options_view_move_selection(s_ov, delta);
}

static void lora_input(InputEvent *event) {
    if (!event || !s_ov) return;
    if (event->type == INPUT_TYPE_TOUCH) {
        handle_touch(event);
        return;
    }
    if (event->type == INPUT_TYPE_KEYBOARD) {
        int key = event->data.key_value;
        if (key == LV_KEY_ESC || key == LV_KEY_LEFT || key == 29 || key == '`' || key == 'h')
            go_back();
        else if (key == LV_KEY_ENTER || key == 13)
            activate_selected();
        else if (key == LV_KEY_UP || key == 'k')
            move_selection(-1);
        else if (key == LV_KEY_DOWN || key == 'j')
            move_selection(1);
        else if (key == '\t') move_selection(1);
        else if (key == 'n' && s_page == PAGE_MESSAGE)
            open_composer(s_conversation != 0, s_conversation, PAGE_MESSAGE);
        return;
    }
    if (event->type == INPUT_TYPE_ENCODER) {
        if (event->data.encoder.button) activate_selected();
        else if (event->data.encoder.direction > 0) move_selection(1);
        else if (event->data.encoder.direction < 0) move_selection(-1);
        return;
    }
    if (event->type == INPUT_TYPE_JOYSTICK) {
        if (!event->data.joystick_pressed) return;
        int button = event->data.joystick_index;
        if (button == 0) go_back();
        else if (button == 2) move_selection(-1);
        else if (button == 4 || button == 3) move_selection(1);
        else if (button == 1) activate_selected();
        return;
    }
#if defined(CONFIG_USE_ENCODER) || defined(CONFIG_IS_ATOMS3R)
    if (event->type == INPUT_TYPE_EXIT_BUTTON) go_back();
#endif
}

void lora_view_create(void) {
    s_page = s_resume_pending ? s_resume_page : PAGE_MAIN;
    s_resume_pending = false;
    touch_drag_reset(&s_touch);
    if (s_page == PAGE_MESSAGES) s_unread_count = 0;
    poll_messages(false);
    s_ov = options_view_create(NULL, "LoRa");
    if (!s_ov) return;
    lora_view.root = options_view_get_list(s_ov);
    configure_list();
    rebuild_page();
    s_timer = lv_timer_create(lora_tick, 1000, NULL);
    if (s_pending_notice[0]) {
        notice(s_pending_notice, s_pending_notice_type);
        s_pending_notice[0] = '\0';
    }
}

void lora_view_destroy(void) {
    lvgl_timer_del_safe(&s_timer);
    touch_drag_reset(&s_touch);
    if (s_ov) {
        options_view_destroy(s_ov);
        s_ov = NULL;
    }
    lora_view.root = NULL;
}

void lora_view_update_remote_state(const char *state) { (void)state; }

static void lora_get_input(void **callback) {
    if (callback) *callback = lora_view.input_callback;
}

View lora_view = {
    .root = NULL,
    .create = lora_view_create,
    .destroy = lora_view_destroy,
    .input_callback = lora_input,
    .name = "LoRa",
    .get_hardwareinput_callback = lora_get_input,
};

#else

#include <stddef.h>
void lora_view_update_remote_state(const char *state) { (void)state; }

#endif
