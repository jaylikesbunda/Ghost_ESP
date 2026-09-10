// ethernet_screen.c - Multi-state Ethernet dashboard view for GhostESP2
// Supports both local mode (CONFIG_WITH_ETHERNET) and remote GhostLink peer mode.

#include "managers/views/ethernet_screen.h"
#include "managers/display_manager.h"
#include "managers/settings_manager.h"
#include "gui/detail_view.h"
#include "gui/scan_status.h"
#include "gui/screen_layout.h"
#include "gui/accessibility_fonts.h"
#include "gui/options_view.h"
#include "gui/theme_palette_api.h"
#include "gui/lvgl_safe.h"
#include "lvgl.h"
#include "core/esp_comm_manager.h"
#include "sdkconfig.h"
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>

#define ETH_MAX_ARP_HOSTS 64
#define ETH_MAX_PORT_RESULTS 256
#define ETH_MAX_FP_HOSTS 32
#define ETH_MAX_POISON_DOMAINS 50
#define ETH_MAX_POISON_COOKIES 10
#define ETH_MAX_POISON_CREDS 10

// Local mode: device has physical ethernet hardware
#ifdef CONFIG_WITH_ETHERNET
#define ETH_HAS_LOCAL 1
#include "managers/ethernet_manager.h"
#include "managers/ethernet/eth_scan_async.h"
#include "managers/ethernet/eth_fingerprint.h"
#include "attacks/ethernet/eth_arp_poison.h"
#include "esp_netif.h"
#include "lwip/ip4_addr.h"
#endif

// ============================================================
// State enum
// ============================================================

typedef enum {
    ETH_STATE_DASHBOARD,
    ETH_STATE_SCANNING,
    ETH_STATE_FP_LIST,
    ETH_STATE_FP_RESULTS,
    ETH_STATE_ARP_LIST,
    ETH_STATE_ARP_RESULTS,
    ETH_STATE_PORT_RESULTS,
    ETH_STATE_PING_RESULTS,
    ETH_STATE_POISON_MONITOR,
} eth_screen_state_t;

static View *s_return_view = &options_menu_view;

// ============================================================
// Theme colors
// ============================================================

static uint32_t bg_color     = 0x0A0A0A;
static uint32_t card_color   = 0x1A1A1A;
static uint32_t text_color   = 0xFFFFFF;
static uint32_t accent_color = 0x00FFFF;
static uint32_t muted_color  = 0x888888;

static void load_theme_colors(void) {
    uint8_t t    = settings_get_menu_theme(&G_Settings);
    bg_color     = theme_palette_get_background(t);
    card_color   = theme_palette_get_surface(t);
    text_color   = theme_palette_get_text(t);
    accent_color = theme_palette_get_accent(t);
    muted_color  = theme_palette_get_text_muted(t);
}

// ============================================================
// Display-only structs (always defined, no CONFIG dependency).
// These are populated either from local scan APIs (ETH_HAS_LOCAL)
// or from parsed GhostLink stream records (remote mode).
// All UI build functions read from these - no conditional code in build functions.
// ============================================================

typedef struct {
    char    ip_str[16];
    uint8_t mac[6];
    char    hostname[48];
} eth_disp_host_t;

typedef struct {
    uint16_t port;
    char     service[24];
} eth_disp_port_t;

typedef struct {
    bool             done;
    bool             cancelled;
    int              type;   // 0=none,1=arp,2=port_local,3=port_all,4=ping
    int              arp_count;
    int              port_count;
    char             target_ip[16];
    int              ping_alive;
    int              ping_total;
    int              progress_current;
    int              progress_total;
} eth_disp_scan_t;

typedef struct {
    char ip_str[16];
    char name[64];
    char device_type[48];
    char protocol[16];
    char service_type[96];
    char os_info[64];
} eth_disp_fp_host_t;

typedef struct {
    int                count;
} eth_disp_fp_t;

typedef struct {
    bool running;
    int  host_count;
    int  domain_count;
    int  cookie_count;
    int  cred_count;
} eth_disp_poison_t;

typedef union {
    eth_disp_host_t    arp_hosts[ETH_MAX_ARP_HOSTS];
    eth_disp_port_t    port_results[ETH_MAX_PORT_RESULTS];
    eth_disp_fp_host_t fp_hosts[ETH_MAX_FP_HOSTS];
    char               poison_domains[ETH_MAX_POISON_DOMAINS][64];
    char               poison_cookies[ETH_MAX_POISON_COOKIES][48];
    char               poison_creds[ETH_MAX_POISON_CREDS][64];
} eth_workspace_data_t;

typedef struct {
    eth_workspace_data_t *data;
    size_t                size;
} eth_workspace_t;

typedef struct {
    char ip[16];
    char mask[16];
    char gw[16];
    char mac[20];
    int  speed_mbps;
    bool connected;
} eth_disp_iface_t;

static eth_disp_scan_t   s_scan        = {0};
static eth_disp_fp_t     s_fp          = {0};
static eth_disp_poison_t s_poison      = {0};
static eth_disp_iface_t  s_iface       = {0};
static eth_workspace_t   s_workspace   = {0};
static bool              s_scan_running = false;

// Remote-mode acknowledgement tracking: records are only meaningful once the
// peer echoes "S|scanning". If no echo arrives within a few seconds, the peer
// likely never received/recognized the command (e.g. outdated peer firmware)
// -- surface that instead of showing a fake scanning screen forever.
static bool     s_peer_ack      = false;
static uint32_t s_no_ack_ticks  = 0;

// ============================================================
// Static UI state
// ============================================================

static lv_obj_t           *s_root         = NULL;
static lv_obj_t           *s_content      = NULL;
static eth_screen_state_t  s_state        = ETH_STATE_DASHBOARD;
static detail_view_t      *s_result_dv    = NULL;
static scan_status_t      *s_scan_status  = NULL;
static lv_timer_t         *s_status_timer = NULL;
static lv_timer_t         *s_poll_timer   = NULL;
static bool                s_fp_active    = false;
static options_view_t     *s_action_ov    = NULL;
static int                 s_selected_fp_host  = -1;
static int                 s_selected_arp_host = -1;
static bool                s_touch_started = false;
static lv_point_t          s_touch_start   = {0};
static touch_drag_t        s_eth_touch_drag = {0};
static eth_screen_state_t  s_touch_state   = ETH_STATE_DASHBOARD;

// Dashboard status labels
static lv_obj_t *s_lbl_status = NULL;
static lv_obj_t *s_lbl_ip     = NULL;
static lv_obj_t *s_lbl_mask   = NULL;
static lv_obj_t *s_lbl_gw     = NULL;
static lv_obj_t *s_lbl_mac    = NULL;
static lv_obj_t *s_lbl_link   = NULL;

// Poison monitor
static int       s_poison_section        = 0;   // 0=Domains 1=Cookies 2=Creds 3=Stop
static int       s_poison_prev[3]        = {0, 0, 0};
static lv_obj_t *s_poison_tab_labels[4]  = {NULL, NULL, NULL, NULL};
static lv_obj_t *s_poison_content_list   = NULL;
static lv_obj_t *s_poison_stats_lbl      = NULL;

// ============================================================
// Forward declarations
// ============================================================

static void build_dashboard(void);
static void build_scanning(void);
static void build_fp_list(void);
static void build_fp_results(void);
static void build_arp_list(void);
static void build_arp_results(void);
static void build_port_results(void);
static void build_ping_results(void);
static void build_poison_monitor(void);
static void rebuild_content(eth_screen_state_t new_state);
static void refresh_dashboard_labels(void);
static void populate_poison_content(int sec);
static void switch_poison_tab(int new_sec);
static bool ensure_workspace(size_t size);
static void release_workspace(void);
static eth_disp_host_t *workspace_arp_hosts(void);
static eth_disp_port_t *workspace_port_results(void);
static eth_disp_fp_host_t *workspace_fp_hosts(void);
static char (*workspace_poison_domains(void))[64];
static char (*workspace_poison_cookies(void))[48];
static char (*workspace_poison_creds(void))[64];

static void on_fp_scan(lv_event_t *e);
static void on_arp_scan(lv_event_t *e);
static void on_port_local(lv_event_t *e);
static void on_port_all(lv_event_t *e);
static void on_ping(lv_event_t *e);
static void on_poison_start(lv_event_t *e);
static void on_poison_stop(lv_event_t *e);
static void on_poison_monitor(lv_event_t *e);
static void on_back(lv_event_t *e);
static void on_back_to_dashboard(lv_event_t *e);
static void on_back_to_fp_list(lv_event_t *e);
static void on_back_to_arp_list(lv_event_t *e);
static void on_fp_host_selected(lv_event_t *e);
static void on_arp_host_selected(lv_event_t *e);

static void scan_poll_cb(lv_timer_t *t);
static void poison_monitor_cb(lv_timer_t *t);
static void status_timer_cb(lv_timer_t *t);

// ============================================================
// Remote helper
// ============================================================

static bool eth_is_remote(void) {
#ifdef ETH_HAS_LOCAL
    return false;
#else
    return true;
#endif
}

static bool ensure_workspace(size_t size) {
    if (size == 0) return true;
    if (s_workspace.data && s_workspace.size >= size) {
        memset(s_workspace.data, 0, s_workspace.size);
        return true;
    }

    void *buf = calloc(1, size);
    if (!buf) return false;

    free(s_workspace.data);
    s_workspace.data = (eth_workspace_data_t *)buf;
    s_workspace.size = size;
    return true;
}

static void release_workspace(void) {
    free(s_workspace.data);
    s_workspace.data = NULL;
    s_workspace.size = 0;
}

static eth_disp_host_t *workspace_arp_hosts(void) {
    return s_workspace.data ? s_workspace.data->arp_hosts : NULL;
}

static eth_disp_port_t *workspace_port_results(void) {
    return s_workspace.data ? s_workspace.data->port_results : NULL;
}

static eth_disp_fp_host_t *workspace_fp_hosts(void) {
    return s_workspace.data ? s_workspace.data->fp_hosts : NULL;
}

static char (*workspace_poison_domains(void))[64] {
    return s_workspace.data ? s_workspace.data->poison_domains : NULL;
}

static char (*workspace_poison_cookies(void))[48] {
    return s_workspace.data ? s_workspace.data->poison_cookies : NULL;
}

static char (*workspace_poison_creds(void))[64] {
    return s_workspace.data ? s_workspace.data->poison_creds : NULL;
}

// ============================================================
// Scan dispatch functions
// ============================================================

static void dispatch_arp_scan(void) {
    memset(&s_scan, 0, sizeof(s_scan));
    if (!ensure_workspace(sizeof(s_workspace.data->arp_hosts))) {
        s_scan.done = true;
        s_scan_running = false;
        return;
    }
    s_scan.type = 1;
    s_scan_running = true;
#ifdef ETH_HAS_LOCAL
    eth_scan_start_arp();
#else
    esp_comm_manager_send_command("ethernet", "arp_scan");
#endif
}

static void dispatch_fp_scan(void) {
    memset(&s_fp, 0, sizeof(s_fp));
    if (!ensure_workspace(sizeof(s_workspace.data->fp_hosts))) {
        s_scan.done = true;
        s_scan_running = false;
        return;
    }
    s_scan_running = true;
    s_scan.type = 0; // FP uses s_fp, not s_scan
    // Remote mode reads s_scan.done to decide when the scan finished, but
    // it's only cleared by the peer's async "S|scanning" echo -- if it was
    // left true by a previous ARP/port/ping scan, the very next poll tick
    // reads it as already done and backs out before the peer even starts.
    s_scan.done = false;
#ifdef ETH_HAS_LOCAL
    eth_fingerprint_start_async();
#else
    esp_comm_manager_send_command("ethernet", "fp_scan");
#endif
}

static void dispatch_port_scan(bool all) {
    memset(&s_scan, 0, sizeof(s_scan));
    if (!ensure_workspace(sizeof(s_workspace.data->port_results))) {
        s_scan.done = true;
        s_scan_running = false;
        return;
    }
    s_scan.type = all ? 3 : 2;
    s_scan_running = true;
#ifdef ETH_HAS_LOCAL
    esp_netif_ip_info_t ip_info;
    char gw[16] = "";
    if (ethernet_manager_get_ip_info(&ip_info) == ESP_OK)
        esp_ip4addr_ntoa(&ip_info.gw, gw, sizeof(gw));
    eth_scan_start_port(gw[0] ? gw : NULL, all);
#else
    esp_comm_manager_send_command("ethernet", all ? "port_scan_all" : "port_scan_local");
#endif
}

static void dispatch_ping(void) {
    memset(&s_scan, 0, sizeof(s_scan));
    release_workspace();
    s_scan.type = 4;
    s_scan_running = true;
#ifdef ETH_HAS_LOCAL
    eth_scan_start_ping();
#else
    esp_comm_manager_send_command("ethernet", "ping_sweep");
#endif
}

static void dispatch_poison_start(void) {
    memset(&s_poison, 0, sizeof(s_poison));
    if (!ensure_workspace(sizeof(eth_workspace_data_t))) {
        s_poison.running = false;
        return;
    }
#ifdef ETH_HAS_LOCAL
    eth_arp_poison_start();
#else
    esp_comm_manager_send_command("ethernet", "poison_start");
#endif
}

static void dispatch_poison_stop(void) {
#ifdef ETH_HAS_LOCAL
    eth_arp_poison_stop();
#else
    esp_comm_manager_send_command("ethernet", "poison_stop");
#endif
    release_workspace();
}

static void dispatch_cancel(void) {
#ifdef ETH_HAS_LOCAL
    eth_scan_cancel();
    eth_fingerprint_scan_cancel();
#else
    esp_comm_manager_send_command("ethernet", "cancel");
#endif
    s_scan_running = false;
    s_scan.done = true;
}

// ============================================================
// Remote stream receive callback
// Only registered in remote mode.
// Records are null-terminated strings with '|'-separated fields.
// ============================================================

static void eth_stream_rx_cb(uint8_t channel, const uint8_t *data, size_t length, void *ud) {
    (void)channel;
    (void)ud;
    if (!data || length < 2) return;
    const char *rec = (const char *)data;
    char type = rec[0];
    if (rec[1] != '|') return;
    const char *payload = rec + 2;

    if (type == 'H') {
        // H|ip|mac_str|hostname
        eth_disp_host_t *hosts = workspace_arp_hosts();
        if (!hosts || s_scan.arp_count >= ETH_MAX_ARP_HOSTS) return;
        eth_disp_host_t *h = &hosts[s_scan.arp_count];
        char mac_str[18] = "";
        sscanf(payload, "%15[^|]|%17[^|]|%47s", h->ip_str, mac_str, h->hostname);
        // Parse MAC string into bytes
        unsigned m[6] = {0};
        sscanf(mac_str, "%02X:%02X:%02X:%02X:%02X:%02X",
               &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]);
        for (int i = 0; i < 6; i++) h->mac[i] = (uint8_t)m[i];
        s_scan.arp_count++;

    } else if (type == 'F') {
        // F|ip|name|device_type|protocol|service_type|os_info
        eth_disp_fp_host_t *hosts = workspace_fp_hosts();
        if (!hosts || s_fp.count >= ETH_MAX_FP_HOSTS) return;
        eth_disp_fp_host_t *h = &hosts[s_fp.count];
        char tmp[320];
        strlcpy(tmp, payload, sizeof(tmp));
        char *tok = strtok(tmp, "|");
        if (tok) { strlcpy(h->ip_str,      tok, sizeof(h->ip_str));      tok = strtok(NULL, "|"); }
        if (tok) { strlcpy(h->name,         tok, sizeof(h->name));         tok = strtok(NULL, "|"); }
        if (tok) { strlcpy(h->device_type,  tok, sizeof(h->device_type));  tok = strtok(NULL, "|"); }
        if (tok) { strlcpy(h->protocol,     tok, sizeof(h->protocol));     tok = strtok(NULL, "|"); }
        if (tok) { strlcpy(h->service_type, tok, sizeof(h->service_type)); tok = strtok(NULL, "|"); }
        if (tok)   strlcpy(h->os_info,      tok, sizeof(h->os_info));
        s_fp.count++;

    } else if (type == 'P') {
        // P|port|service
        eth_disp_port_t *ports = workspace_port_results();
        if (!ports || s_scan.port_count >= ETH_MAX_PORT_RESULTS) return;
        eth_disp_port_t *p = &ports[s_scan.port_count];
        int port = 0;
        char svc[24] = "";
        sscanf(payload, "%d|%23s", &port, svc);
        p->port = (uint16_t)port;
        strlcpy(p->service, svc, sizeof(p->service));
        s_scan.port_count++;

    } else if (type == 'T') {
        // T|target_ip
        strlcpy(s_scan.target_ip, payload, sizeof(s_scan.target_ip));

    } else if (type == 'D') {
        // D|domain
        char (*domains)[64] = workspace_poison_domains();
        if (domains && s_poison.domain_count < ETH_MAX_POISON_DOMAINS)
            strlcpy(domains[s_poison.domain_count++], payload, 64);

    } else if (type == 'K') {
        // K|cookie
        char (*cookies)[48] = workspace_poison_cookies();
        if (cookies && s_poison.cookie_count < ETH_MAX_POISON_COOKIES)
            strlcpy(cookies[s_poison.cookie_count++], payload, 48);

    } else if (type == 'C') {
        // C|cred
        char (*creds)[64] = workspace_poison_creds();
        if (creds && s_poison.cred_count < ETH_MAX_POISON_CREDS)
            strlcpy(creds[s_poison.cred_count++], payload, 64);

    } else if (type == 'M') {
        // M|host_count|domain_count|cookie_count|cred_count
        sscanf(payload, "%d|%d|%d|%d",
               &s_poison.host_count, &s_poison.domain_count,
               &s_poison.cookie_count, &s_poison.cred_count);

    } else if (type == 'N') {
        // N|alive|total
        sscanf(payload, "%d|%d", &s_scan.ping_alive, &s_scan.ping_total);

    } else if (type == 'G') {
        // G|current|total
        sscanf(payload, "%d|%d", &s_scan.progress_current, &s_scan.progress_total);

    } else if (type == 'I') {
        // I|ip|mask|gw|mac|speed
        char tmp[128];
        strlcpy(tmp, payload, sizeof(tmp));
        char *tok = strtok(tmp, "|");
        if (tok) { strlcpy(s_iface.ip,   tok, sizeof(s_iface.ip));   tok = strtok(NULL, "|"); }
        if (tok) { strlcpy(s_iface.mask, tok, sizeof(s_iface.mask)); tok = strtok(NULL, "|"); }
        if (tok) { strlcpy(s_iface.gw,   tok, sizeof(s_iface.gw));   tok = strtok(NULL, "|"); }
        if (tok) { strlcpy(s_iface.mac,  tok, sizeof(s_iface.mac));  tok = strtok(NULL, "|"); }
        if (tok)   s_iface.speed_mbps = atoi(tok);
        s_iface.connected = (s_iface.ip[0] != '0' && s_iface.ip[0] != '\0');

    } else if (type == 'S') {
        // S|<status>
        if (strcmp(payload, "scanning") == 0) {
            s_scan_running = true;
            s_scan.done    = false;
            // Peer acknowledged the scan command; live records are flowing.
            s_peer_ack     = true;
            s_no_ack_ticks = 0;
        } else if (strcmp(payload, "done") == 0) {
            s_scan_running = false;
            s_scan.done    = true;
        } else if (strcmp(payload, "running") == 0 || strcmp(payload, "poison_running") == 0) {
            s_poison.running = true;
        } else if (strcmp(payload, "stopped") == 0) {
            s_poison.running = false;
            s_scan_running   = false;
        }
    }
}

// ============================================================
// Card helper
// ============================================================

static lv_obj_t *create_card(lv_obj_t *parent, int width_pct) {
    lv_obj_t *card = lv_obj_create(parent);
    int padding = LV_VER_RES <= 100 ? 3 : (LV_VER_RES <= 160 ? 5 : 8);
    lv_coord_t radius = settings_get_menu_rounded(&G_Settings) ? GUI_RADIUS_SM : 0;
    lv_obj_set_size(card, LV_PCT(width_pct), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_hex(card_color), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, radius, 0);
    lv_obj_set_style_pad_all(card, padding, 0);
    lv_obj_set_style_text_color(card, lv_color_hex(text_color), 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(card, 2, 0);
    return card;
}

// ============================================================
// Stat row helper - returns the value label
// ============================================================

static lv_obj_t *add_stat_row(lv_obj_t *parent, const char *key) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *lbl_key = lv_label_create(row);
    lv_label_set_text(lbl_key, key);
    lv_obj_set_style_text_color(lbl_key, lv_color_hex(muted_color), 0);

    lv_obj_t *lbl_val = lv_label_create(row);
    lv_label_set_text(lbl_val, "--");
    lv_obj_set_style_text_color(lbl_val, lv_color_hex(text_color), 0);
    lv_label_set_long_mode(lbl_val, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(lbl_val, LV_PCT(65));
    lv_obj_set_style_text_align(lbl_val, LV_TEXT_ALIGN_RIGHT, 0);

    return lbl_val;
}

// ============================================================
// refresh_dashboard_labels
// ============================================================

static void refresh_dashboard_labels(void) {
    if (!s_lbl_status || !lv_obj_is_valid(s_lbl_status)) return;

#ifdef ETH_HAS_LOCAL
    bool connected = ethernet_manager_is_connected();
    s_iface.connected = connected;
    if (connected) {
        esp_netif_ip_info_t ip_info;
        if (ethernet_manager_get_ip_info(&ip_info) == ESP_OK) {
            esp_ip4addr_ntoa(&ip_info.ip,      s_iface.ip,   sizeof(s_iface.ip));
            esp_ip4addr_ntoa(&ip_info.netmask, s_iface.mask, sizeof(s_iface.mask));
            esp_ip4addr_ntoa(&ip_info.gw,      s_iface.gw,   sizeof(s_iface.gw));
        }
        ethernet_link_info_t link;
        if (ethernet_manager_get_link_info(&link) == ESP_OK)
            s_iface.speed_mbps = link.speed_mbps;
        esp_netif_t *netif = ethernet_manager_get_netif();
        if (netif) {
            uint8_t mac[6];
            if (esp_netif_get_mac(netif, mac) == ESP_OK)
                snprintf(s_iface.mac, sizeof(s_iface.mac),
                         "%02X:%02X:%02X:%02X:%02X:%02X",
                         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        }
    }
#endif
    // Render from s_iface (populated locally above, or from stream 'I' records in remote mode)
    lv_label_set_text(s_lbl_status, s_iface.connected ? "Connected" : "Disconnected");
    lv_obj_set_style_text_color(s_lbl_status,
        s_iface.connected ? lv_color_hex(0x00CC66) : lv_color_hex(0xFF4444), 0);
    lv_label_set_text(s_lbl_ip,   s_iface.ip[0]   ? s_iface.ip   : "--");
    lv_label_set_text(s_lbl_mask, s_iface.mask[0] ? s_iface.mask : "--");
    lv_label_set_text(s_lbl_gw,   s_iface.gw[0]   ? s_iface.gw   : "--");
    lv_label_set_text(s_lbl_mac,  s_iface.mac[0]  ? s_iface.mac  : "--");
    if (s_lbl_link) {
        char link_str[32];
        if (s_iface.speed_mbps > 0)
            snprintf(link_str, sizeof(link_str), "%d Mbps", s_iface.speed_mbps);
        else
            strlcpy(link_str, "--", sizeof(link_str));
        lv_label_set_text(s_lbl_link, link_str);
    }
}

// ============================================================
// populate_poison_content - rebuild the single content list for `sec`
// ============================================================

static void populate_poison_content(int sec) {
    if (!s_poison_content_list || !lv_obj_is_valid(s_poison_content_list)) return;
    lv_obj_clean(s_poison_content_list);

    if (sec == 3) {
        // Stop tab - single destructive action item
        lv_obj_t *item = lv_list_add_btn(s_poison_content_list, NULL,
                                         "Press SELECT to stop ARP poison");
        lv_obj_set_style_text_color(item, lv_color_hex(0xFF4444), 0);
        lv_obj_set_style_text_font(item, accessibility_get_font_small(), 0);
        lv_obj_add_event_cb(item, on_poison_stop, LV_EVENT_CLICKED, NULL);
        return;
    }

    int cnt = (sec == 0) ? s_poison.domain_count
            : (sec == 1) ? s_poison.cookie_count
            :              s_poison.cred_count;
    char (*domains)[64] = workspace_poison_domains();
    char (*cookies)[48] = workspace_poison_cookies();
    char (*creds)[64]   = workspace_poison_creds();
    for (int i = 0; i < cnt; i++) {
        const char *text = (sec == 0) ? (domains ? domains[i] : "")
                         : (sec == 1) ? (cookies ? cookies[i] : "")
                         :              (creds   ? creds[i]   : "");
        lv_obj_t *item = lv_list_add_btn(s_poison_content_list, NULL, text);
        lv_obj_set_style_bg_color(item, lv_color_hex(card_color), 0);
        lv_obj_set_style_bg_opa(item, LV_OPA_COVER, 0);
        lv_obj_set_style_text_font(item, accessibility_get_font_small(), 0);
        lv_obj_set_style_text_color(item, lv_color_hex(text_color), 0);
        lv_obj_set_style_pad_ver(item, 4, 0);
    }
    if (cnt == 0) {
        lv_obj_t *item = lv_list_add_btn(s_poison_content_list, NULL, "(none captured yet)");
        lv_obj_set_style_bg_color(item, lv_color_hex(card_color), 0);
        lv_obj_set_style_bg_opa(item, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(item, lv_color_hex(muted_color), 0);
        lv_obj_set_style_text_font(item, accessibility_get_font_small(), 0);
    }
    if (sec < 3) s_poison_prev[sec] = cnt;
}

// ============================================================
// switch_poison_tab - update tab highlights and repopulate content
// ============================================================

static void switch_poison_tab(int new_sec) {
    s_poison_section = new_sec;
    for (int i = 0; i < 4; i++) {
        lv_obj_t *lbl = s_poison_tab_labels[i];
        if (!lbl || !lv_obj_is_valid(lbl)) continue;
        bool active = (i == new_sec);
        uint32_t col = active
            ? (i == 3 ? 0xFF4444 : accent_color)
            : (i == 3 ? 0x883333 : muted_color);
        lv_obj_set_style_text_color(lbl, lv_color_hex(col), 0);
    }
    populate_poison_content(new_sec);
}

// ============================================================
// Action callbacks
// ============================================================

static void on_fp_scan(lv_event_t *e) {
    (void)e;
    s_fp_active = true;
    dispatch_fp_scan();
    rebuild_content(ETH_STATE_SCANNING);
    if (s_scan_status) scan_status_update(s_scan_status, "Fingerprint Scan");
}

static void on_arp_scan(lv_event_t *e) {
    (void)e;
    dispatch_arp_scan();
    rebuild_content(ETH_STATE_SCANNING);
    if (s_scan_status) scan_status_update(s_scan_status, "ARP Host Scan");
}

static void on_port_local(lv_event_t *e) {
    (void)e;
    dispatch_port_scan(false);
    rebuild_content(ETH_STATE_SCANNING);
    if (s_scan_status) scan_status_update(s_scan_status, "Port Scan (Common)");
}

static void on_port_all(lv_event_t *e) {
    (void)e;
    dispatch_port_scan(true);
    rebuild_content(ETH_STATE_SCANNING);
    if (s_scan_status) scan_status_update(s_scan_status, "Port Scan (All)");
}

static void on_ping(lv_event_t *e) {
    (void)e;
    dispatch_ping();
    rebuild_content(ETH_STATE_SCANNING);
    if (s_scan_status) scan_status_update(s_scan_status, "Ping Sweep");
}

static void on_poison_start(lv_event_t *e) {
    (void)e;
    dispatch_poison_start();
    s_poison.running = true;
    rebuild_content(ETH_STATE_POISON_MONITOR);
}

static void on_poison_stop(lv_event_t *e) {
    (void)e;
    dispatch_poison_stop();
    s_poison.running = false;
    rebuild_content(ETH_STATE_DASHBOARD);
}

static void on_poison_monitor(lv_event_t *e) {
    (void)e;
    rebuild_content(ETH_STATE_POISON_MONITOR);
}

static void on_back(lv_event_t *e) {
    (void)e;
    display_manager_go_back();
}

void ethernet_screen_set_return_view(View *view) {
    s_return_view = view ? view : &options_menu_view;
}

static void on_back_to_dashboard(lv_event_t *e) {
    (void)e;
    rebuild_content(ETH_STATE_DASHBOARD);
}

static void on_back_to_fp_list(lv_event_t *e) {
    (void)e;
    rebuild_content(ETH_STATE_FP_LIST);
}

static void on_back_to_arp_list(lv_event_t *e) {
    (void)e;
    rebuild_content(ETH_STATE_ARP_LIST);
}

static void on_fp_host_selected(lv_event_t *e) {
    s_selected_fp_host = (int)(intptr_t)lv_event_get_user_data(e);
    rebuild_content(ETH_STATE_FP_RESULTS);
}

static void on_arp_host_selected(lv_event_t *e) {
    s_selected_arp_host = (int)(intptr_t)lv_event_get_user_data(e);
    rebuild_content(ETH_STATE_ARP_RESULTS);
}

// ============================================================
// scan_poll_cb - handles both local and remote modes
// ============================================================

static void scan_poll_cb(lv_timer_t *t) {
    (void)t;
    bool done = false;

#ifdef ETH_HAS_LOCAL
    if (s_fp_active) {
        done = eth_fingerprint_scan_is_done();
        if (!done) return;
        // Copy local FP results into display struct
        eth_fp_results_t local_fp;
        int count = eth_fingerprint_get_results(&local_fp);
        s_fp.count = count;
        eth_disp_fp_host_t *fp_hosts = workspace_fp_hosts();
        for (int i = 0; i < count && i < ETH_MAX_FP_HOSTS && fp_hosts; i++) {
            strlcpy(fp_hosts[i].ip_str,      local_fp.hosts[i].ip_str,      16);
            strlcpy(fp_hosts[i].name,         local_fp.hosts[i].name,         64);
            strlcpy(fp_hosts[i].device_type,  local_fp.hosts[i].device_type,  48);
            strlcpy(fp_hosts[i].protocol,     local_fp.hosts[i].protocol,     16);
            strlcpy(fp_hosts[i].service_type, local_fp.hosts[i].service_type, 96);
            strlcpy(fp_hosts[i].os_info,      local_fp.hosts[i].os_info,      64);
        }
    } else {
        done = eth_scan_is_done();
        if (!done) {
            // Update live progress display
            const eth_scan_results_t *r = eth_scan_get_results();
            s_scan.progress_current = r->progress_current;
            s_scan.progress_total   = r->progress_total;
            s_scan.arp_count        = r->arp_count;
            s_scan.port_count       = r->port_count;
            if (s_scan_status) {
                if (r->progress_total > 0)
                    scan_status_set_progress(s_scan_status, s_scan.progress_current, s_scan.progress_total);
                char sub[48];
                if (s_scan.type == 1)
                    snprintf(sub, sizeof(sub), "Found %d hosts...", s_scan.arp_count);
                else if (s_scan.type == 2 || s_scan.type == 3)
                    snprintf(sub, sizeof(sub), "Port %d/%d, %d open",
                             s_scan.progress_current, s_scan.progress_total, s_scan.port_count);
                else
                    snprintf(sub, sizeof(sub), "%d/%d alive", r->ping_alive, r->ping_total);
                scan_status_set_subtext(s_scan_status, sub);
            }
            return;
        }
        // Copy completed local scan results into display struct
        const eth_scan_results_t *r = eth_scan_get_results();
        s_scan.arp_count  = r->arp_count;
        s_scan.port_count = r->port_count;
        s_scan.ping_alive = r->ping_alive;
        s_scan.ping_total = r->ping_total;
        strlcpy(s_scan.target_ip, r->target_ip, sizeof(s_scan.target_ip));
        eth_disp_host_t *arp_hosts = workspace_arp_hosts();
        for (int i = 0; i < r->arp_count && i < ETH_MAX_ARP_HOSTS && arp_hosts; i++) {
            strlcpy(arp_hosts[i].ip_str,  r->arp_hosts[i].ip_str,  16);
            memcpy(arp_hosts[i].mac,      r->arp_hosts[i].mac,     6);
            strlcpy(arp_hosts[i].hostname, r->arp_hosts[i].hostname, 48);
        }
        eth_disp_port_t *port_results = workspace_port_results();
        for (int i = 0; i < r->port_count && i < ETH_MAX_PORT_RESULTS && port_results; i++) {
            port_results[i].port = r->port_results[i].port;
            strlcpy(port_results[i].service, r->port_results[i].service, 24);
        }
        // Map local enum type to display type integer
        switch (r->type) {
            case ETH_SCAN_TYPE_ARP:       s_scan.type = 1; break;
            case ETH_SCAN_TYPE_PORT_LOCAL: s_scan.type = 2; break;
            case ETH_SCAN_TYPE_PORT_ALL:   s_scan.type = 3; break;
            case ETH_SCAN_TYPE_PING:       s_scan.type = 4; break;
            default:                       s_scan.type = 0; break;
        }
    }
#else
    // Remote mode: s_scan.done and s_scan_running are set by eth_stream_rx_cb
    done = s_scan.done;
    if (!done) {
        if (s_scan_status) {
            // If the peer never echoed "S|scanning", it did not receive or
            // recognize the command (outdated peer firmware). After ~3 s,
            // stop showing fake live progress and say so instead.
            if (!s_peer_ack && ++s_no_ack_ticks > 15) {
                scan_status_set_subtext(s_scan_status,
                    "Peer not responding - check peer firmware/link");
                return;
            }
            scan_status_set_progress(s_scan_status, s_scan.progress_current, s_scan.progress_total);
            char sub[48];
            if (s_scan.type == 1)
                snprintf(sub, sizeof(sub), "Found %d hosts...", s_scan.arp_count);
            else if (s_scan.type == 2 || s_scan.type == 3)
                snprintf(sub, sizeof(sub), "Port %d/%d, %d open",
                         s_scan.progress_current, s_scan.progress_total, s_scan.port_count);
            else if (s_scan.type == 0)
                snprintf(sub, sizeof(sub), "Discovering services...");
            else
                snprintf(sub, sizeof(sub), "Sweeping...");
            scan_status_set_subtext(s_scan_status, sub);
        }
        return;
    }
#endif

    s_scan_running = false;

    // Transition to result state based on what was scanned
    if (s_fp_active) {
        s_fp_active = false;
        s_selected_fp_host = -1;
        rebuild_content(ETH_STATE_FP_LIST);
    } else if (s_scan.type == 1) {
        s_selected_arp_host = -1;
        rebuild_content(ETH_STATE_ARP_LIST);
    } else if (s_scan.type == 2 || s_scan.type == 3) {
        rebuild_content(ETH_STATE_PORT_RESULTS);
    } else {
        rebuild_content(ETH_STATE_PING_RESULTS);
    }
}

// ============================================================
// poison_monitor_cb - handles both local and remote modes
// ============================================================

static void poison_monitor_cb(lv_timer_t *t) {
    (void)t;
#ifdef ETH_HAS_LOCAL
    if (!eth_arp_poison_is_running()) {
        rebuild_content(ETH_STATE_DASHBOARD);
        return;
    }
    eth_arp_poison_snapshot_t snap;
    if (!eth_arp_poison_get_snapshot(&snap)) return;
    s_poison.running      = snap.running;
    s_poison.host_count   = snap.host_count;
    s_poison.domain_count = snap.domain_count;
    s_poison.cookie_count = snap.cookie_count;
    s_poison.cred_count   = snap.cred_count;
    char (*p_domains)[64] = workspace_poison_domains();
    char (*p_cookies)[48] = workspace_poison_cookies();
    char (*p_creds)[64]   = workspace_poison_creds();
    for (int i = 0; i < snap.domain_count && i < ETH_MAX_POISON_DOMAINS && p_domains; i++)
        strlcpy(p_domains[i], snap.domains[i], 64);
    for (int i = 0; i < snap.cookie_count && i < ETH_MAX_POISON_COOKIES && p_cookies; i++)
        strlcpy(p_cookies[i], snap.cookies[i], 48);
    for (int i = 0; i < snap.cred_count && i < ETH_MAX_POISON_CREDS && p_creds; i++)
        strlcpy(p_creds[i], snap.creds[i], 64);
#else
    if (!s_poison.running) {
        rebuild_content(ETH_STATE_DASHBOARD);
        return;
    }
    // s_poison is updated by eth_stream_rx_cb 'D'/'K'/'C'/'M' records
#endif

    // Update live stats header
    if (s_poison_stats_lbl && lv_obj_is_valid(s_poison_stats_lbl)) {
        char stats[96];
        snprintf(stats, sizeof(stats), "%d hosts  |  %d domains  |  %d cookies  |  %d creds",
                 s_poison.host_count, s_poison.domain_count, s_poison.cookie_count, s_poison.cred_count);
        lv_label_set_text(s_poison_stats_lbl, stats);
    }

    // Refresh content list only if the current tab's count changed
    int counts[3] = { s_poison.domain_count, s_poison.cookie_count, s_poison.cred_count };
    if (s_poison_section < 3 && counts[s_poison_section] != s_poison_prev[s_poison_section]) {
        populate_poison_content(s_poison_section);
    }
    // Keep all three prev values in sync so switching tabs is instant
    for (int i = 0; i < 3; i++) s_poison_prev[i] = counts[i];
}

// ============================================================
// status_timer_cb
// ============================================================

static void status_timer_cb(lv_timer_t *t) {
    (void)t;
    if (s_state == ETH_STATE_DASHBOARD)
        refresh_dashboard_labels();
}

// ============================================================
// build_dashboard
// ============================================================

static void build_dashboard(void) {
    // Compact status card (natural height, first item in the flex column)
    lv_obj_t *status_card = create_card(s_content, 100);
    lv_obj_set_width(status_card, LV_HOR_RES - 2 * GUI_OPTIONS_LIST_PAD_HOR);
    lv_obj_set_flex_align(s_content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    s_lbl_status = add_stat_row(status_card, "Status");
    s_lbl_ip     = add_stat_row(status_card, "IP");
    s_lbl_mask   = add_stat_row(status_card, "Mask");
    s_lbl_gw     = add_stat_row(status_card, "Gateway");
    s_lbl_mac    = add_stat_row(status_card, "MAC");
    s_lbl_link   = add_stat_row(status_card, "Link");
    refresh_dashboard_labels();

    // Actions list - fills all remaining vertical space via flex-grow
    s_action_ov = options_view_create(s_content, NULL);
    lv_obj_t *list = options_view_get_list(s_action_ov);
    if (list && lv_obj_is_valid(list)) {
        // Clear the absolute alignment set by options_view_create and let
        // the parent flex-column drive position; flex-grow fills leftover height
        lv_obj_set_align(list, LV_ALIGN_DEFAULT);
        lv_obj_set_flex_grow(list, 1);
        lv_obj_set_style_pad_top(list, 0, 0);
        lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(list, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    }

#ifdef ETH_HAS_LOCAL
    bool poison_running = eth_arp_poison_is_running();
#else
    bool poison_running = s_poison.running;
#endif
    options_view_add_item(s_action_ov, "Fingerprint Scan",       on_fp_scan,        NULL);
    options_view_add_item(s_action_ov, "ARP Host Scan",          on_arp_scan,       NULL);
    options_view_add_item(s_action_ov, "Port Scan (GW)",         on_port_local,     NULL);
    options_view_add_item(s_action_ov, "Port Scan (All)",        on_port_all,       NULL);
    options_view_add_item(s_action_ov, "Ping Sweep",             on_ping,           NULL);
    if (poison_running) {
        options_view_add_item(s_action_ov, "Stop ARP Poison",   on_poison_stop,    NULL);
        options_view_add_item(s_action_ov, "Poison Monitor",    on_poison_monitor, NULL);
    } else {
        options_view_add_item(s_action_ov, "ARP Poison",        on_poison_start,   NULL);
    }
    options_view_add_back_row(s_action_ov, on_back, NULL);
    options_view_set_selected(s_action_ov, 0);
}

// ============================================================
// build_scanning
// ============================================================

static void build_scanning(void) {
    s_peer_ack     = false;
    s_no_ack_ticks = 0;
    s_scan_status = scan_status_create("Scanning...");
    scan_status_set_subtext(s_scan_status, "Initializing...");
    s_poll_timer = lv_timer_create(scan_poll_cb, 200, NULL);
}

static void build_fp_list(void) {
    char title[40];
    snprintf(title, sizeof(title), "Fingerprint (%d host%s)",
             s_fp.count, s_fp.count != 1 ? "s" : "");
    // options_view_create(s_root, title) positions the list at y=STATUS_BAR below s_root
    s_action_ov = options_view_create(s_root, title);

    eth_disp_fp_host_t *fp_hosts_list = workspace_fp_hosts();
    for (int i = 0; i < s_fp.count; i++) {
        eth_disp_fp_host_t *h = fp_hosts_list ? &fp_hosts_list[i] : NULL;
        if (!h) break;
        char label[96];
        if (h->name[0])
            snprintf(label, sizeof(label), "%s  %s", h->ip_str, h->name);
        else if (h->device_type[0])
            snprintf(label, sizeof(label), "%s  [%s]", h->ip_str, h->device_type);
        else
            snprintf(label, sizeof(label), "%s", h->ip_str);
        options_view_add_item(s_action_ov, label, on_fp_host_selected, (void *)(intptr_t)i);
    }
    if (s_fp.count == 0)
        options_view_add_item(s_action_ov, "No hosts discovered", NULL, NULL);
    options_view_add_back_row(s_action_ov, on_back_to_dashboard, NULL);
    // Restore cursor to last viewed host (if returning from detail page)
    int sel = (s_selected_fp_host >= 0 && s_selected_fp_host < s_fp.count)
              ? s_selected_fp_host : 0;
    options_view_set_selected(s_action_ov, sel);
}

static void build_arp_list(void) {
    char title[40];
    snprintf(title, sizeof(title), "ARP Scan (%d host%s)",
             s_scan.arp_count, s_scan.arp_count != 1 ? "s" : "");
    s_action_ov = options_view_create(s_root, title);

    eth_disp_host_t *arp_hosts_list = workspace_arp_hosts();
    for (int i = 0; i < s_scan.arp_count; i++) {
        eth_disp_host_t *h = arp_hosts_list ? &arp_hosts_list[i] : NULL;
        if (!h) break;
        char label[80];
        if (h->hostname[0])
            snprintf(label, sizeof(label), "%s  %s", h->ip_str, h->hostname);
        else
            snprintf(label, sizeof(label), "%s  %02X:%02X:%02X:%02X:%02X:%02X",
                     h->ip_str,
                     h->mac[0], h->mac[1], h->mac[2],
                     h->mac[3], h->mac[4], h->mac[5]);
        options_view_add_item(s_action_ov, label, on_arp_host_selected, (void *)(intptr_t)i);
    }
    if (s_scan.arp_count == 0)
        options_view_add_item(s_action_ov, "No hosts found", NULL, NULL);
    options_view_add_back_row(s_action_ov, on_back_to_dashboard, NULL);
    int sel = (s_selected_arp_host >= 0 && s_selected_arp_host < s_scan.arp_count)
              ? s_selected_arp_host : 0;
    options_view_set_selected(s_action_ov, sel);
}

// ============================================================
// build_fp_results - reads from s_fp display struct
// ============================================================

static void build_fp_results(void) {
    if (s_selected_fp_host < 0 || s_selected_fp_host >= s_fp.count) {
        rebuild_content(ETH_STATE_FP_LIST);
        return;
    }
    eth_disp_fp_host_t *fp_ws = workspace_fp_hosts();
    if (!fp_ws) { rebuild_content(ETH_STATE_FP_LIST); return; }
    eth_disp_fp_host_t *h = &fp_ws[s_selected_fp_host];
    // detail_view anchors to parent at y=STATUS_BAR; pass s_root (at y=0) so offset is correct
    s_result_dv = detail_view_create(s_root, h->ip_str);
    if (!s_result_dv) return;

    detail_view_add_info(s_result_dv, "IP", h->ip_str);
    if (h->name[0])         detail_view_add_info(s_result_dv, "Name",     h->name);
    if (h->device_type[0])  detail_view_add_info(s_result_dv, "Type",     h->device_type);
    if (h->protocol[0])     detail_view_add_info(s_result_dv, "Protocol", h->protocol);
    if (h->service_type[0]) detail_view_add_info(s_result_dv, "Service",  h->service_type);
    if (h->os_info[0])      detail_view_add_info(s_result_dv, "OS",       h->os_info);
    detail_view_add_back(s_result_dv, on_back_to_fp_list, NULL);
    detail_view_set_selected(s_result_dv, 0);
}

// ============================================================
// build_arp_results - reads from s_scan display struct
// ============================================================

static void build_arp_results(void) {
    if (s_selected_arp_host < 0 || s_selected_arp_host >= s_scan.arp_count) {
        rebuild_content(ETH_STATE_ARP_LIST);
        return;
    }
    eth_disp_host_t *arp_ws = workspace_arp_hosts();
    if (!arp_ws) { rebuild_content(ETH_STATE_ARP_LIST); return; }
    eth_disp_host_t *h = &arp_ws[s_selected_arp_host];
    s_result_dv = detail_view_create(s_root, h->ip_str);
    if (!s_result_dv) return;

    char mac_str[24];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             h->mac[0], h->mac[1], h->mac[2], h->mac[3], h->mac[4], h->mac[5]);
    detail_view_add_info(s_result_dv, "IP",  h->ip_str);
    detail_view_add_info(s_result_dv, "MAC", mac_str);
    if (h->hostname[0]) detail_view_add_info(s_result_dv, "Host", h->hostname);
    detail_view_add_back(s_result_dv, on_back_to_arp_list, NULL);
    detail_view_set_selected(s_result_dv, 0);
}

// ============================================================
// build_port_results - reads from s_scan display struct
// ============================================================

static void build_port_results(void) {
    s_result_dv = detail_view_create(s_root, "Port Scan");
    if (!s_result_dv) return;

    detail_view_add_info(s_result_dv, "Target", s_scan.target_ip[0] ? s_scan.target_ip : "?");
    detail_view_add_infof(s_result_dv, "Open Ports", "%d", s_scan.port_count);

    if (s_scan.port_count == 0) {
        detail_view_add_info(s_result_dv, "Result", "No open ports");
    } else {
        eth_disp_port_t *port_ws = workspace_port_results();
        for (int i = 0; i < s_scan.port_count && port_ws; i++) {
            char port_str[8];
            snprintf(port_str, sizeof(port_str), "%d", port_ws[i].port);
            detail_view_add_info(s_result_dv, port_str, port_ws[i].service);
        }
    }
    detail_view_add_back(s_result_dv, on_back_to_dashboard, NULL);
    detail_view_set_selected(s_result_dv, 0);
}

static void build_ping_results(void) {
    s_result_dv = detail_view_create(s_root, "Ping Sweep");
    if (!s_result_dv) return;

    detail_view_add_infof(s_result_dv, "Alive", "%d", s_scan.ping_alive);
    detail_view_add_infof(s_result_dv, "Scanned", "%d", s_scan.ping_total);
    detail_view_add_back(s_result_dv, on_back_to_dashboard, NULL);
    detail_view_set_selected(s_result_dv, 0);
}

// ============================================================
// build_poison_monitor - reads from s_poison display struct
// ============================================================

static void build_poison_monitor(void) {
    // Tab names local to this function (switch_poison_tab only touches colors)
    static const char *tab_names[4] = {"Domains", "Cookies", "Creds", "Stop"};

    s_poison_section = 0;  // always open on Domains tab
    s_poison_prev[0] = s_poison_prev[1] = s_poison_prev[2] = 0;
    // s_poison_tab_labels / s_poison_content_list / s_poison_stats_lbl
    // are already NULL'd by rebuild_content before this is called.

    // ── Header card ──────────────────────────────────────────────────────────
    lv_obj_t *hdr = create_card(s_content, 100);

    lv_obj_t *hdr_lbl = lv_label_create(hdr);
    lv_label_set_text(hdr_lbl, "ARP POISON ACTIVE");
    lv_obj_set_style_text_color(hdr_lbl, lv_color_hex(accent_color), 0);

    s_poison_stats_lbl = lv_label_create(hdr);
    char stats[96];
    snprintf(stats, sizeof(stats), "%d hosts  |  %d domains  |  %d cookies  |  %d creds",
             s_poison.host_count, s_poison.domain_count, s_poison.cookie_count, s_poison.cred_count);
    lv_label_set_text(s_poison_stats_lbl, stats);
    lv_obj_set_style_text_color(s_poison_stats_lbl, lv_color_hex(muted_color), 0);
    lv_obj_set_style_text_font(s_poison_stats_lbl, accessibility_get_font_small(), 0);

    // ── Tab bar ──────────────────────────────────────────────────────────────
    lv_obj_t *tab_bar = lv_obj_create(s_content);
    lv_obj_set_size(tab_bar, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(tab_bar, lv_color_hex(card_color), 0);
    lv_obj_set_style_bg_opa(tab_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(tab_bar, 0, 0);
    lv_obj_set_style_radius(tab_bar, settings_get_menu_rounded(&G_Settings) ? GUI_RADIUS_SM : 0, 0);
    lv_obj_set_style_pad_all(tab_bar, 4, 0);
    lv_obj_clear_flag(tab_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(tab_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tab_bar, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    for (int i = 0; i < 4; i++) {
        lv_obj_t *lbl = lv_label_create(tab_bar);
        lv_label_set_text(lbl, tab_names[i]);
        lv_obj_set_style_text_font(lbl, accessibility_get_font_small(), 0);
        // i==0 active, i==3 Stop (dimmed red inactive)
        uint32_t col = (i == 0) ? accent_color
                     : (i == 3) ? 0x883333
                     :            muted_color;
        lv_obj_set_style_text_color(lbl, lv_color_hex(col), 0);
        s_poison_tab_labels[i] = lbl;
    }

    // ── Content list - fills all remaining vertical space ────────────────────
    s_poison_content_list = lv_list_create(s_content);
    lv_obj_set_size(s_poison_content_list, LV_PCT(100), 0);
    lv_obj_set_flex_grow(s_poison_content_list, 1);
    lv_obj_set_style_bg_color(s_poison_content_list, lv_color_hex(bg_color), 0);
    lv_obj_set_style_bg_opa(s_poison_content_list, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_poison_content_list, 0, 0);
    lv_obj_set_style_pad_all(s_poison_content_list, 2, 0);

    populate_poison_content(0);

    lv_obj_set_scrollbar_mode(s_poison_content_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_snap_x(s_poison_content_list, LV_SCROLL_SNAP_NONE);
    lv_obj_set_scroll_snap_y(s_poison_content_list, LV_SCROLL_SNAP_NONE);

    s_poll_timer = lv_timer_create(poison_monitor_cb, 1000, NULL);
}

// ============================================================
// rebuild_content - core state machine transition
// ============================================================

static void rebuild_content(eth_screen_state_t new_state) {
    // 1. Delete poll timer
    if (s_poll_timer) {
        lv_timer_del(s_poll_timer);
        s_poll_timer = NULL;
    }

    // 2. Close scan status overlay
    if (s_scan_status && scan_status_is_active(s_scan_status)) {
        scan_status_close(s_scan_status);
        s_scan_status = NULL;
    }

    // 3. Destroy result detail view (anchored to s_root)
    if (s_result_dv) {
        detail_view_destroy(s_result_dv);
        s_result_dv = NULL;
    }

    // 4. Destroy action/host list options view
    if (s_action_ov) {
        options_view_destroy(s_action_ov);
        s_action_ov = NULL;
    }

    // 5. Destroy content container (used by DASHBOARD / SCANNING / POISON_MONITOR)
    if (s_content && lv_obj_is_valid(s_content)) {
        lv_obj_del(s_content);
        s_content = NULL;
    }

    // 6. Null out dangling pointers
    s_lbl_status = s_lbl_ip = s_lbl_mask = s_lbl_gw = s_lbl_mac = s_lbl_link = NULL;
    s_poison_tab_labels[0] = s_poison_tab_labels[1] =
    s_poison_tab_labels[2] = s_poison_tab_labels[3] = NULL;
    s_poison_content_list = NULL;
    s_poison_stats_lbl    = NULL;

    // 7. Update state
    s_state = new_state;
    s_touch_started = false;
    touch_drag_reset(&s_eth_touch_drag);

    // 8. States that anchor directly to s_root need no s_content wrapper.
    //    options_view_create(s_root) and detail_view_create(s_root) both
    //    handle their own positioning at y = STATUS_BAR_HEIGHT below s_root.
    switch (new_state) {
        case ETH_STATE_FP_LIST:      build_fp_list();      return;
        case ETH_STATE_ARP_LIST:     build_arp_list();     return;
        case ETH_STATE_FP_RESULTS:   build_fp_results();   return;
        case ETH_STATE_ARP_RESULTS:  build_arp_results();  return;
        case ETH_STATE_PORT_RESULTS: build_port_results(); return;
        case ETH_STATE_PING_RESULTS: build_ping_results(); return;
        default: break;
    }

    // 9. Remaining states (DASHBOARD / SCANNING / POISON_MONITOR) use s_content,
    //    a full-height flex-column container sitting below the status bar.
    s_content = lv_obj_create(s_root);
    lv_obj_set_style_bg_color(s_content, lv_color_hex(bg_color), 0);
    lv_obj_set_style_bg_opa(s_content, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_content, 0, 0);
    lv_obj_set_style_pad_all(s_content, new_state == ETH_STATE_DASHBOARD ? 0 : 4, 0);
    lv_obj_set_style_pad_row(s_content, 4, 0);
    lv_obj_set_size(s_content, LV_PCT(100), LV_VER_RES - GUI_STATUS_BAR_HEIGHT);
    lv_obj_set_pos(s_content, 0, GUI_STATUS_BAR_HEIGHT);
    lv_obj_set_align(s_content, LV_ALIGN_TOP_LEFT);
    lv_obj_set_flex_flow(s_content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    // SCANNING: outer container scrollable in case status widget overflows.
    // DASHBOARD / POISON_MONITOR: each handles scrolling internally.
    if (new_state == ETH_STATE_SCANNING) {
        lv_obj_add_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(s_content, LV_DIR_VER);
    } else {
        lv_obj_clear_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);
    }

    // 10. Build sub-screen
    switch (new_state) {
        case ETH_STATE_DASHBOARD:      build_dashboard();      break;
        case ETH_STATE_SCANNING:       build_scanning();       break;
        case ETH_STATE_POISON_MONITOR: build_poison_monitor(); break;
        default: break;
    }
}

// ============================================================
// Touch helpers
// ============================================================

static bool point_in_obj(lv_obj_t *obj, const lv_point_t *p) {
    if (!obj || !p || !lv_obj_is_valid(obj)) return false;
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    return p->x >= area.x1 && p->x <= area.x2 && p->y >= area.y1 && p->y <= area.y2;
}

static bool handle_options_touch(options_view_t *ov, const lv_indev_data_t *data) {
    if (!ov || !data) return false;
    lv_obj_t *list = options_view_get_list(ov);
    if (!list || !lv_obj_is_valid(list)) return false;

    if (!point_in_obj(list, &s_touch_start)) return false;

    int dx = data->point.x - s_touch_start.x;
    int dy = data->point.y - s_touch_start.y;
    int thr_y = LV_VER_RES / 20;
    int thr_x = LV_HOR_RES / 20;
    if (thr_y < 6) thr_y = 6;
    if (thr_x < 6) thr_x = 6;

    if (abs(dy) > thr_y) {
        lv_obj_scroll_by_bounded(list, 0, dy, LV_ANIM_OFF);
        return true;
    }
    if (abs(dx) > thr_x || !point_in_obj(list, &data->point)) return true;

    uint32_t child_cnt = lv_obj_get_child_cnt(list);
    for (uint32_t i = 0; i < child_cnt; i++) {
        lv_obj_t *btn = lv_obj_get_child(list, (int32_t)i);
        if (!btn || !lv_obj_is_valid(btn)) continue;
        if (point_in_obj(btn, &data->point)) {
            options_view_set_selected(ov, (int)i);
            lv_event_send(btn, LV_EVENT_CLICKED, NULL);
            return true;
        }
    }
    return true;
}

static bool handle_detail_touch(detail_view_t *dv, const lv_indev_data_t *data) {
    if (!dv || !data) return false;
    lv_obj_t *list = detail_view_get_list(dv);
    if (!list || !lv_obj_is_valid(list)) return false;

    if (!point_in_obj(list, &s_touch_start)) return false;

    int dx = data->point.x - s_touch_start.x;
    int dy = data->point.y - s_touch_start.y;
    int thr_y = LV_VER_RES / 20;
    if (thr_y < 6) thr_y = 6;

    if (abs(dy) > thr_y) {
        lv_obj_scroll_by_bounded(list, 0, dy, LV_ANIM_OFF);
        return true;
    }
    if (abs(dx) > thr_y || !point_in_obj(list, &data->point)) return true;

    uint32_t child_cnt = lv_obj_get_child_cnt(list);
    for (uint32_t i = 0; i < child_cnt; i++) {
        lv_obj_t *child = lv_obj_get_child(list, (int32_t)i);
        if (!child || !lv_obj_is_valid(child)) continue;
        if (point_in_obj(child, &data->point)) {
            lv_event_send(child, LV_EVENT_CLICKED, NULL);
            return true;
        }
    }
    return true;
}

static bool handle_poison_touch(const lv_indev_data_t *data) {
    if (!data) return false;

    for (int i = 0; i < 4; i++) {
        lv_obj_t *tab = s_poison_tab_labels[i];
        if (tab && lv_obj_is_valid(tab) &&
            point_in_obj(tab, &s_touch_start) && point_in_obj(tab, &data->point)) {
            switch_poison_tab(i);
            return true;
        }
    }

    lv_obj_t *list = s_poison_content_list;
    if (!list || !lv_obj_is_valid(list) || !point_in_obj(list, &s_touch_start)) return false;

    int dx = data->point.x - s_touch_start.x;
    int dy = data->point.y - s_touch_start.y;
    int thr_y = LV_VER_RES / 20;
    int thr_x = LV_HOR_RES / 20;
    if (thr_y < 6) thr_y = 6;
    if (thr_x < 6) thr_x = 6;

    if (abs(dy) > thr_y) {
        lv_obj_scroll_by_bounded(list, 0, dy, LV_ANIM_OFF);
        return true;
    }
    if (abs(dx) > thr_x || !point_in_obj(list, &data->point)) return true;

    uint32_t child_cnt = lv_obj_get_child_cnt(list);
    for (uint32_t i = 0; i < child_cnt; i++) {
        lv_obj_t *child = lv_obj_get_child(list, (int32_t)i);
        if (!child || !lv_obj_is_valid(child)) continue;
        if (point_in_obj(child, &data->point)) {
            lv_event_send(child, LV_EVENT_CLICKED, NULL);
            return true;
        }
    }
    return true;
}

static bool handle_ethernet_touch(const lv_indev_data_t *data) {
    if (!data) return false;

    if (data->state == LV_INDEV_STATE_PR) {
        if (!s_eth_touch_drag.started) {
            touch_drag_begin(&s_eth_touch_drag, data);
            s_touch_state = s_state;
            s_touch_started = true;
            s_touch_start = data->point;
            return true;
        }
        // Move event - apply live drag (or remember target) for the current state
        lv_obj_t *scroll_target = NULL;
        switch (s_touch_state) {
            case ETH_STATE_DASHBOARD:
            case ETH_STATE_FP_LIST:
            case ETH_STATE_ARP_LIST:
                if (s_action_ov) scroll_target = options_view_get_list(s_action_ov);
                break;
            case ETH_STATE_FP_RESULTS:
            case ETH_STATE_ARP_RESULTS:
            case ETH_STATE_PORT_RESULTS:
            case ETH_STATE_PING_RESULTS:
                if (s_result_dv) scroll_target = detail_view_get_list(s_result_dv);
                break;
            case ETH_STATE_POISON_MONITOR:
                scroll_target = s_poison_content_list;
                break;
            default:
                break;
        }
        if (scroll_target && lv_obj_is_valid(scroll_target)) {
            touch_drag_update(&s_eth_touch_drag, data, scroll_target);
        }
        s_touch_start = data->point;
        return true;
    }

    if (data->state != LV_INDEV_STATE_REL || !s_touch_started) return true;
    s_touch_started = false;

    // Let the shared helper handle release-on-release
    if (touch_drag_release(&s_eth_touch_drag, data)) return true;

    switch (s_touch_state) {
        case ETH_STATE_DASHBOARD:
        case ETH_STATE_FP_LIST:
        case ETH_STATE_ARP_LIST:
            return handle_options_touch(s_action_ov, data);
        case ETH_STATE_FP_RESULTS:
        case ETH_STATE_ARP_RESULTS:
        case ETH_STATE_PORT_RESULTS:
        case ETH_STATE_PING_RESULTS:
            return handle_detail_touch(s_result_dv, data);
        case ETH_STATE_POISON_MONITOR:
            return handle_poison_touch(data);
        default:
            return true;
    }
}

// ============================================================
// Input callback
// ============================================================

static void ethernet_screen_input_cb(InputEvent *event) {
    if (!event) return;

    bool up = false, down = false, select = false, back = false;
    bool left = false, right = false;

    switch (event->type) {
        case INPUT_TYPE_JOYSTICK:
            if (!event->data.joystick_pressed) return;
            if      (event->data.joystick_index == 2) up    = true;
            else if (event->data.joystick_index == 4) down  = true;
            else if (event->data.joystick_index == 0) back  = true;
            else if (event->data.joystick_index == 3) right = true;
            else if (event->data.joystick_index == 1) select = true;
            break;
        case INPUT_TYPE_KEYBOARD: {
            uint8_t k = event->data.key_value;
            if      (k == LV_KEY_UP)                               up     = true;
            else if (k == LV_KEY_DOWN)                             down   = true;
            else if (k == LV_KEY_LEFT)                             left   = true;
            else if (k == LV_KEY_RIGHT)                            right  = true;
            else if (k == LV_KEY_ENTER || k == '\n' || k == '\r') select = true;
            else if (k == LV_KEY_ESC || k == 27)                   back   = true;
            break;
        }
        case INPUT_TYPE_ENCODER:
            if      (event->data.encoder.direction < 0) up     = true;
            else if (event->data.encoder.direction > 0) down   = true;
            else if (event->data.encoder.button)        select = true;
            break;
        case INPUT_TYPE_EXIT_BUTTON:
            back = true;
            break;
        case INPUT_TYPE_TOUCH:
            handle_ethernet_touch(&event->data.touch_data);
            return;
        default:
            return;
    }

    switch (s_state) {
        // ── Dashboard: card + options_view list ──────────────────────────────
        case ETH_STATE_DASHBOARD:
            if (s_action_ov) {
                if (up)   options_view_move_selection(s_action_ov, -1);
                if (down) options_view_move_selection(s_action_ov, +1);
                if (select) {
                    lv_obj_t *list = options_view_get_list(s_action_ov);
                    int idx = options_view_get_selected(s_action_ov);
                    if (list && lv_obj_is_valid(list) && idx >= 0) {
                        lv_obj_t *btn = lv_obj_get_child(list, idx);
                        if (btn && lv_obj_is_valid(btn))
                            lv_event_send(btn, LV_EVENT_CLICKED, NULL);
                    }
                }
                if (back) display_manager_go_back();
            }
            break;

        // ── Host lists (FP / ARP): Wi-Fi-style selectable rows ───────────────
        case ETH_STATE_FP_LIST:
        case ETH_STATE_ARP_LIST:
            if (s_action_ov) {
                if (up)   options_view_move_selection(s_action_ov, -1);
                if (down) options_view_move_selection(s_action_ov, +1);
                if (select) {
                    lv_obj_t *list = options_view_get_list(s_action_ov);
                    int idx = options_view_get_selected(s_action_ov);
                    if (list && lv_obj_is_valid(list) && idx >= 0) {
                        lv_obj_t *btn = lv_obj_get_child(list, idx);
                        if (btn && lv_obj_is_valid(btn))
                            lv_event_send(btn, LV_EVENT_CLICKED, NULL);
                    }
                }
                if (back || left) rebuild_content(ETH_STATE_DASHBOARD);
            }
            break;

        // ── Scanning: back OR center press cancels ───────────────────────────
        case ETH_STATE_SCANNING:
            if (back || left || select) {
                dispatch_cancel();
                s_fp_active = false;
                rebuild_content(ETH_STATE_DASHBOARD);
            }
            break;

        // ── FP host detail: back returns to FP list ──────────────────────────
        case ETH_STATE_FP_RESULTS:
            if (s_result_dv) {
                if (up)   detail_view_step_up(s_result_dv);
                if (down) detail_view_step_down(s_result_dv);
                if (select) {
                    lv_obj_t *sel = detail_view_get_selected_obj(s_result_dv);
                    if (sel && lv_obj_is_valid(sel))
                        lv_event_send(sel, LV_EVENT_CLICKED, NULL);
                }
                if (back || left) rebuild_content(ETH_STATE_FP_LIST);
            }
            break;

        // ── ARP host detail: back returns to ARP list ────────────────────────
        case ETH_STATE_ARP_RESULTS:
            if (s_result_dv) {
                if (up)   detail_view_step_up(s_result_dv);
                if (down) detail_view_step_down(s_result_dv);
                if (select) {
                    lv_obj_t *sel = detail_view_get_selected_obj(s_result_dv);
                    if (sel && lv_obj_is_valid(sel))
                        lv_event_send(sel, LV_EVENT_CLICKED, NULL);
                }
                if (back || left) rebuild_content(ETH_STATE_ARP_LIST);
            }
            break;

        // ── Port / Ping results: back returns to dashboard ───────────────────
        case ETH_STATE_PORT_RESULTS:
        case ETH_STATE_PING_RESULTS:
            if (s_result_dv) {
                if (up)   detail_view_step_up(s_result_dv);
                if (down) detail_view_step_down(s_result_dv);
                if (select) {
                    lv_obj_t *sel = detail_view_get_selected_obj(s_result_dv);
                    if (sel && lv_obj_is_valid(sel))
                        lv_event_send(sel, LV_EVENT_CLICKED, NULL);
                }
                if (back || left) rebuild_content(ETH_STATE_DASHBOARD);
            }
            break;

        // ── Poison monitor: tabs left/right, scroll up/down, select=stop ─────
        case ETH_STATE_POISON_MONITOR:
            if (left || right) {
                int dir = right ? 1 : -1;
                switch_poison_tab((s_poison_section + dir + 4) % 4);
            }
            if (up || down) {
                if (s_poison_content_list && lv_obj_is_valid(s_poison_content_list))
                    lv_obj_scroll_by(s_poison_content_list, 0,
                                     up ? -20 : 20, LV_ANIM_OFF);
            }
            if (select && s_poison_section == 3) {
                // Confirmed stop from the Stop tab
                dispatch_poison_stop();
                s_poison.running = false;
                rebuild_content(ETH_STATE_DASHBOARD);
            }
            // back/left does NOT exit - user must select Stop tab and press select
            break;
    }
}

// ============================================================
// get_hardwareinput_callback
// ============================================================

static void get_ethernet_screen_cb(void **cb) {
    if (cb) *cb = (void *)ethernet_screen_input_cb;
}

// ============================================================
// ethernet_screen_create / ethernet_screen_destroy
// ============================================================

void ethernet_screen_create(void) {
    load_theme_colors();

    // In remote mode: register stream handler and request current status from peer
    if (eth_is_remote()) {
        esp_comm_manager_register_stream_handler(
            COMM_STREAM_CHANNEL_ETHERNET, eth_stream_rx_cb, NULL);
        esp_comm_manager_send_command("ethernet", "status");
    }

    s_root = gui_screen_create_root(NULL, "Ethernet", lv_color_hex(bg_color), LV_OPA_TRANSP);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    ethernet_screen_view.root = s_root;

    s_status_timer = lv_timer_create(status_timer_cb, 1000, NULL);
    s_state     = ETH_STATE_DASHBOARD;
    s_content   = NULL;
    s_fp_active = false;
    rebuild_content(ETH_STATE_DASHBOARD);
}

void ethernet_screen_destroy(void) {
    if (eth_is_remote()) {
        dispatch_cancel();
        // Deregister stream handler by passing NULL callback
        esp_comm_manager_register_stream_handler(COMM_STREAM_CHANNEL_ETHERNET, NULL, NULL);
    }
    release_workspace();
    if (s_poll_timer) {
        lv_timer_del(s_poll_timer);
        s_poll_timer = NULL;
    }
    if (s_status_timer) {
        lv_timer_del(s_status_timer);
        s_status_timer = NULL;
    }
    if (s_scan_status && scan_status_is_active(s_scan_status)) {
        scan_status_close(s_scan_status);
        s_scan_status = NULL;
    }
    if (s_result_dv) {
        detail_view_destroy(s_result_dv);
        s_result_dv = NULL;
    }
    if (s_action_ov) {
        options_view_destroy(s_action_ov);
        s_action_ov = NULL;
    }
    if (s_root && lv_obj_is_valid(s_root)) {
        lv_obj_del(s_root);
        s_root = NULL;
    }
    s_content    = NULL;
    s_fp_active  = false;
    s_scan_running = false;
    s_lbl_status = s_lbl_ip = s_lbl_mask = s_lbl_gw = s_lbl_mac = s_lbl_link = NULL;
    s_poison_tab_labels[0] = s_poison_tab_labels[1] =
    s_poison_tab_labels[2] = s_poison_tab_labels[3] = NULL;
    s_poison_content_list = NULL;
    s_poison_stats_lbl    = NULL;
    ethernet_screen_view.root = NULL;
}

// ============================================================
// View struct definition
// ============================================================

View ethernet_screen_view = {
    .root                       = NULL,
    .create                     = ethernet_screen_create,
    .destroy                    = ethernet_screen_destroy,
    .name                       = "Ethernet",
    .get_hardwareinput_callback = get_ethernet_screen_cb,
    .input_callback             = ethernet_screen_input_cb,
};
