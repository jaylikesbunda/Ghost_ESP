#include "managers/ghostscript_runtime.h"

#include "core/glog.h"
#include "core/ouis.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "managers/infrared_manager.h"
#include "managers/plugin_api.h"
#include "managers/sd_card_manager.h"
#include "managers/subghz_remote_manager.h"
#include "managers/views/terminal_screen.h"
#include "scans/wifi/station_scan.h"
#include "gui/toast.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include <dirent.h>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define GS_READ_BUF_MAX 1024
#define GS_LOAD_CHUNK 512
#define GS_SD_DMA_RESERVE 1024
#define GS_COMMAND_LOG_LINES 8
#define GS_COMMAND_LOG_LINE_MAX 80
#define GS_EVENT_QUEUE_DEPTH 24
#define GS_EVENT_NAME_MAX 40
#define GS_EVENT_VALUE_MAX 320
#define GS_STORAGE_LIST_MAX 32
#define GS_STORAGE_STREAM_CHUNK_MAX 1024
#define GS_STORAGE_STREAM_MT "ghostscript.storage_stream"

typedef enum {
    GS_QUEUED_EVENT,
    GS_QUEUED_INPUT,
} gs_queued_kind_t;

typedef struct {
    gs_queued_kind_t kind;
} gs_queued_event_t;

typedef struct {
    gs_queued_kind_t kind;
    InputEvent input;
} gs_queued_input_t;

typedef struct {
    gs_queued_kind_t kind;
    char text[];
} gs_queued_text_event_t;

static void *gs_heap_alloc(size_t size) {
    return heap_caps_malloc_prefer(size, 2,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static void *gs_heap_realloc(void *ptr, size_t size) {
    return heap_caps_realloc_prefer(ptr, size, 2,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

struct ghostscript_runtime {
    ghostscript_manifest_t manifest;
    ghostscript_runtime_hooks_t hooks;
    lua_State *L;
    lua_State *thread;
    int thread_ref;
    const ghostesp_api_t *api;
    ghostscript_state_t state;
    size_t memory_used;
    size_t memory_peak;
    size_t memory_limit;
    int64_t slice_start_us;
    bool stop_requested;
    bool api_active;
    bool waiting_for_event;
    bool destroying;
    uint32_t resume_after_ms;
    bool script_done;
    char error[GHOSTSCRIPT_ERROR_MAX];
    char *oui_prefix[4];
    int oui_prefix_count;
    char last_nfc_tag[96];
    /* Dedupe set for ble_device events: small open-addressing table keyed by
     * 6-byte MAC. Reset on each scan start. */
    uint8_t ble_seen_keys[64][6];
    uint8_t ble_seen_used[64];
    /* Last dispatch tick (us) per topic for rate limiting. */
    int64_t last_dispatch_us[16];
    char command_log[GS_COMMAND_LOG_LINES][GS_COMMAND_LOG_LINE_MAX];
    uint32_t command_log_first;
    uint32_t command_log_count;
    uint32_t command_log_total;
    /* Per-runtime task id, used by the task scheduler. */
    int task_id;
};

static ghostscript_runtime_t *s_active_runtime;
static QueueHandle_t s_event_queue;
static portMUX_TYPE s_active_runtime_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_event_senders;

typedef struct {
    char path[GHOSTSCRIPT_PATH_MAX];
    DIR *dir;
} gs_storage_list_ctx_t;

/* A logical stream never keeps an SD file open between Lua calls. This is
 * required on JIT-SD targets, where the card can be unmounted after each call. */
typedef struct {
    ghostscript_runtime_t *runtime;
    char path[GHOSTSCRIPT_PATH_MAX];
    size_t cursor;
    char mode;
    bool closed;
} gs_storage_stream_t;

/* Linked list of background tasks spawned by the script. Each owns its own
 * Lua state but shares the active-runtime pointer for event delivery. */
typedef struct bg_task_s {
    int id;
    lua_State *L;
    char name[32];
    bool done;
    struct bg_task_s *next;
} bg_task_t;
static bg_task_t *s_bg_tasks;

static ghostscript_runtime_t *check_rt(lua_State *L) {
    lua_getfield(L, LUA_REGISTRYINDEX, "ghostscript.rt");
    ghostscript_runtime_t *rt = (ghostscript_runtime_t *)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return rt;
}

static void *gs_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    ghostscript_runtime_t *rt = (ghostscript_runtime_t *)ud;
    if (!rt) return NULL;
    if (nsize == 0) {
        if (ptr) {
            if (rt->memory_used >= osize) rt->memory_used -= osize;
            else rt->memory_used = 0;
            heap_caps_free(ptr);
        }
        return NULL;
    }
    if (!ptr) {
        if (nsize > SIZE_MAX - rt->memory_used) return NULL;
        if (rt->memory_used + nsize > rt->memory_limit && !rt->destroying) return NULL;
        void *allocated = gs_heap_alloc(nsize);
        if (!allocated) return NULL;
        rt->memory_used += nsize;
        if (rt->memory_used > rt->memory_peak) rt->memory_peak = rt->memory_used;
        return allocated;
    }
    if (nsize > osize) {
        size_t growth = nsize - osize;
        if (growth > SIZE_MAX - rt->memory_used) return NULL;
        if (rt->memory_used + growth > rt->memory_limit && !rt->destroying) return NULL;
    }
    void *allocated = gs_heap_realloc(ptr, nsize);
    if (!allocated) return NULL;
    if (nsize >= osize) {
        rt->memory_used += nsize - osize;
    } else {
        size_t reduction = osize - nsize;
        if (rt->memory_used >= reduction) rt->memory_used -= reduction;
        else rt->memory_used = 0;
    }
    if (rt->memory_used > rt->memory_peak) rt->memory_peak = rt->memory_used;
    return allocated;
}

static bool rt_has_perm(ghostscript_runtime_t *rt, uint32_t perm) {
    return rt && ((rt->manifest.permissions & perm) != 0);
}

static int lua_perm_error(lua_State *L, const char *name) {
    return luaL_error(L, "%s permission denied", name ? name : "api");
}

static bool safe_join(const char *base, const char *rel, char *out, size_t out_len) {
    if (!base || !rel || !out || !rel[0] || rel[0] == '/' || strstr(rel, "..")) return false;
    int n = snprintf(out, out_len, "%s/%s", base, rel);
    return n > 0 && (size_t)n < out_len;
}

static bool scoped_storage_path(ghostscript_runtime_t *rt, const char *rel, char *out, size_t out_len) {
    if (!rt || !out || !rel || rel[0] == '/' || strchr(rel, '\\')) return false;
    if (rel[0] == '\0' || strcmp(rel, ".") == 0) {
        int n = snprintf(out, out_len, "%s", rt->manifest.data_path);
        return n > 0 && (size_t)n < out_len;
    }
    for (const char *segment = rel; *segment;) {
        const char *end = strchr(segment, '/');
        size_t len = end ? (size_t)(end - segment) : strlen(segment);
        if (len == 0 || (len == 1 && segment[0] == '.') ||
            (len == 2 && segment[0] == '.' && segment[1] == '.') || segment[0] == '.') {
            return false;
        }
        segment = end ? end + 1 : segment + len;
    }
    return safe_join(rt->manifest.data_path, rel, out, out_len);
}

static void gs_print(ghostscript_runtime_t *rt, const char *text) {
    if (!rt || !text) return;
    if (rt->hooks.print) rt->hooks.print(text, rt->hooks.user);
    else glog("%s", text);
}

static int l_print(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i) {
        size_t len = 0;
        const char *s = luaL_tolstring(L, i, &len);
        if (s) gs_print(rt, s);
        if (i < n) gs_print(rt, "\t");
        lua_pop(L, 1);
    }
    gs_print(rt, "\n");
    return 0;
}

static int l_toast(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_UI)) return lua_perm_error(L, "ui");
    const char *msg = luaL_checkstring(L, 1);
    if (rt->api && rt->api->toast) rt->api->toast(msg);
    return 0;
}

static int l_set_title(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_UI)) return lua_perm_error(L, "ui");
    const char *title = luaL_checkstring(L, 1);
    if (rt->hooks.set_title) rt->hooks.set_title(title, rt->hooks.user);
    if (rt->api && rt->api->ui_set_title) rt->api->ui_set_title(title);
    return 0;
}

static int l_delay(lua_State *L) {
    uint32_t ms = (uint32_t)luaL_checkinteger(L, 1);
    if (ms == 0) ms = 1;
    if (ms > 60000) ms = 60000;
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt || !rt->L) return 0;
    if (lua_isyieldable(L)) {
        rt->resume_after_ms = ms;
        return lua_yield(L, 0);
    }
    if (rt->api && rt->api->delay_ms) rt->api->delay_ms(ms);
    return 0;
}

static const char *const EVENT_KEY = "ghostscript.events";

static void events_table(lua_State *L) {
    lua_getfield(L, LUA_REGISTRYINDEX, EVENT_KEY);
    if (lua_istable(L, -1)) return;
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_setfield(L, LUA_REGISTRYINDEX, EVENT_KEY);
}

static uint32_t event_required_permission(const char *name) {
    if (!name) return 0;
    if (strncmp(name, "wifi_", 5) == 0 || strncmp(name, "capture_", 8) == 0 ||
        strcmp(name, "handshake_captured") == 0 || strcmp(name, "pmkid_exported") == 0) {
        return PLUGIN_PERMISSION_WIFI;
    }
    if (strncmp(name, "ble_", 4) == 0) return PLUGIN_PERMISSION_BLE;
    if (strncmp(name, "nfc_", 4) == 0) return PLUGIN_PERMISSION_NFC;
    if (strncmp(name, "ir_", 3) == 0) return PLUGIN_PERMISSION_IR;
    if (strncmp(name, "subghz_", 7) == 0) return PLUGIN_PERMISSION_SUBGHZ;
    if (strncmp(name, "gps_", 4) == 0) return PLUGIN_PERMISSION_SYSTEM;
    if (strncmp(name, "command.", 8) == 0 || strcmp(name, "comm_command") == 0) return PLUGIN_PERMISSION_COMMANDS;
    if (strncmp(name, "input", 5) == 0) return PLUGIN_PERMISSION_INPUT;
    return 0;
}

static int l_event_on(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    const char *name = luaL_checkstring(L, 1);
    uint32_t required_permission = event_required_permission(name);
    if (required_permission && !rt_has_perm(rt, required_permission)) return lua_perm_error(L, "event topic");
    events_table(L);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (lua_iscfunction(L, 2)) return luaL_error(L, "ghost.on requires a Lua function");
    bool has_filter = !lua_isnoneornil(L, 3);
    if (has_filter && !lua_istable(L, 3)) {
        return luaL_error(L, "ghost.on filter must be a table or nil");
    }
    lua_getfield(L, -1, name);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, -3, name);
    }
    int n = (int)lua_rawlen(L, -1);
    lua_pushvalue(L, 2);
    lua_rawseti(L, -2, ++n);
    if (has_filter) {
        lua_pushvalue(L, 3);
    } else {
        /* Keep listener pairs contiguous; Lua arrays do not retain nil slots. */
        lua_pushboolean(L, false);
    }
    lua_rawseti(L, -2, ++n);
    lua_pop(L, 2);
    lua_pushinteger(L, n / 2);
    return 1;
}

static int l_event_off(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    bool remove_all = lua_isnoneornil(L, 2);
    events_table(L);
    if (remove_all) {
        lua_pushnil(L);
        lua_setfield(L, -2, name);
        lua_pop(L, 1);
        lua_pushinteger(L, 0);
        return 1;
    }
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (lua_iscfunction(L, 2)) return luaL_error(L, "ghost.off requires a Lua function");
    lua_getfield(L, -1, name);
    int removed = 0;
    if (lua_istable(L, -1)) {
        int old_listeners = lua_absindex(L, -1);
        int events = lua_absindex(L, -2);
        int n = (int)lua_rawlen(L, old_listeners);
        int next = 1;
        lua_newtable(L);
        int new_listeners = lua_absindex(L, -1);
        for (int i = 1; i <= n; i += 2) {
            lua_rawgeti(L, old_listeners, i);
            bool is_function = lua_isfunction(L, -1);
            bool remove = is_function && lua_rawequal(L, -1, 2);
            if (!is_function || remove) {
                lua_pop(L, 1);
                if (remove) removed++;
            } else {
                lua_rawgeti(L, old_listeners, i + 1);
                if (lua_isnil(L, -1)) {
                    lua_pop(L, 1);
                    lua_pushboolean(L, false);
                }
                lua_rawseti(L, new_listeners, next + 1);
                lua_rawseti(L, new_listeners, next);
                next += 2;
            }
        }
        lua_pushvalue(L, new_listeners);
        lua_setfield(L, events, name);
        lua_pop(L, 1);
    }
    lua_pop(L, 2);
    lua_pushinteger(L, removed);
    return 1;
}

static bool topic_passes_filter(lua_State *L, const char *topic, const char *value);
static int l_event_emit(lua_State *L);
static int runtime_hook_count(const ghostscript_runtime_t *rt);
static void runtime_begin_slice(ghostscript_runtime_t *rt);
static void runtime_process_input(ghostscript_runtime_t *rt, const InputEvent *event);
static bool queue_text_event(ghostscript_runtime_t *rt, const char *name, const char *value);
static int l_event_emit(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt) return luaL_error(L, "runtime missing");
    const char *name = luaL_checkstring(L, 1);
    const char *value = luaL_optstring(L, 2, "");
    (void)queue_text_event(rt, name, value);
    return 0;
}

static int l_event_wait(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt || !rt->L) return luaL_error(L, "runtime missing");
    if (!lua_isyieldable(L)) return luaL_error(L, "ghost.wait must be called from a coroutine");
    const char *name = luaL_checkstring(L, 1);
    uint32_t required_permission = event_required_permission(name);
    if (required_permission && !rt_has_perm(rt, required_permission)) return lua_perm_error(L, "event topic");
    lua_Integer timeout_ms = luaL_optinteger(L, 2, 0);
    if (timeout_ms < 0) return luaL_error(L, "ghost.wait timeout must not be negative");

    /* entry = { co = <thread>, name = "...", deadline_us = <optional> } */
    lua_createtable(L, 0, 3);
    lua_pushthread(L);
    lua_setfield(L, -2, "co");
    lua_pushstring(L, name);
    lua_setfield(L, -2, "name");
    if (timeout_ms > 0) {
        lua_pushinteger(L, (lua_Integer)esp_timer_get_time() + timeout_ms * 1000);
        lua_setfield(L, -2, "deadline_us");
    }
    lua_getfield(L, LUA_REGISTRYINDEX, EVENT_KEY);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, EVENT_KEY);
    }
    lua_getfield(L, -1, "_waits");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, -3, "_waits");
    }
    int entry_index = lua_absindex(L, -3);
    int waits_index = lua_absindex(L, -1);
    lua_getfield(L, waits_index, "_next_wait_id");
    int high_water = lua_isinteger(L, -1) ? (int)lua_tointeger(L, -1) : 0;
    lua_pop(L, 1);
    int slot = 1;
    for (; slot <= high_water; ++slot) {
        lua_rawgeti(L, waits_index, slot);
        bool available = lua_isnil(L, -1);
        lua_pop(L, 1);
        if (available) break;
    }
    if (slot > high_water) {
        lua_pushinteger(L, slot);
        lua_setfield(L, waits_index, "_next_wait_id");
    }
    lua_pushvalue(L, entry_index);
    lua_rawseti(L, waits_index, slot);
    lua_pop(L, 2);
    if (L == rt->thread) rt->waiting_for_event = true;
    return lua_yield(L, 0);
}

static bool topic_passes_filter(lua_State *L, const char *topic, const char *value) {
    /* Apply C-side fast filters for known topics. If the table has a `match`
     * function, defer to Lua. */
    if (!lua_istable(L, -1)) return true;
    /* Generic match_fn() callback. */
    lua_getfield(L, -1, "match_fn");
    if (lua_isfunction(L, -1)) {
        lua_pushstring(L, value ? value : "");
        if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
            glog("filter error: %s", lua_tostring(L, -1));
            lua_pop(L, 1);
            return true;
        }
        bool ok = lua_toboolean(L, -1);
        lua_pop(L, 1);
        if (!ok) return false;
    } else {
        lua_pop(L, 1);
    }
    /* Topic-specific built-in filters. */
    if (strcmp(topic, "wifi_ap_found") == 0) {
        /* value = bssid|ch|rssi */
        lua_getfield(L, -1, "min_rssi");
        int min_rssi = lua_isnumber(L, -1) ? (int)lua_tointeger(L, -1) : INT32_MIN;
        lua_pop(L, 1);
        if (min_rssi != INT32_MIN) {
            const char *rssi_str = strrchr(value ? value : "", '|');
            int rssi = rssi_str ? atoi(rssi_str + 1) : 0;
            if (rssi < min_rssi) return false;
        }
        lua_getfield(L, -1, "bssid");
        if (lua_isstring(L, -1)) {
            const char *want = lua_tostring(L, -1);
            size_t wlen = strlen(want);
            if (strncmp(value ? value : "", want, wlen) != 0 ||
                (value && value[wlen] != '|' && value[wlen] != '\0')) {
                lua_pop(L, 1);
                return false;
            }
        }
        lua_pop(L, 1);
        lua_getfield(L, -1, "channel");
        if (lua_isnumber(L, -1)) {
            const char *ch_str = strchr(value ? value : "", '|');
            int ch = ch_str ? atoi(ch_str + 1) : -1;
            if (ch != (int)lua_tointeger(L, -1)) { lua_pop(L, 1); return false; }
        }
        lua_pop(L, 1);
    } else if (strcmp(topic, "ble_device") == 0) {
        /* value = mac|rssi */
        lua_getfield(L, -1, "min_rssi");
        if (lua_isnumber(L, -1)) {
            const char *rssi_str = strrchr(value ? value : "", '|');
            int rssi = rssi_str ? atoi(rssi_str + 1) : 0;
            if (rssi < (int)lua_tointeger(L, -1)) { lua_pop(L, 1); return false; }
        }
        lua_pop(L, 1);
        lua_getfield(L, -1, "mac");
        if (lua_isstring(L, -1)) {
            const char *want = lua_tostring(L, -1);
            size_t wlen = strlen(want);
            if (strncmp(value ? value : "", want, wlen) != 0 ||
                (value && value[wlen] != '|' && value[wlen] != '\0')) {
                lua_pop(L, 1);
                return false;
            }
        }
        lua_pop(L, 1);
    } else if (strcmp(topic, "command.output") == 0) {
        lua_getfield(L, -1, "match");
        if (lua_isstring(L, -1)) {
            const char *needle = lua_tostring(L, -1);
            if (!value || !strstr(value, needle)) { lua_pop(L, 1); return false; }
        }
        lua_pop(L, 1);
    } else if (strcmp(topic, "gps_update") == 0 || strcmp(topic, "gps_fix") == 0) {
        /* value = yes/no|lat|lon|alt|sats */
        lua_getfield(L, -1, "want_fix");
        if (lua_isboolean(L, -1)) {
            bool want = lua_toboolean(L, -1);
            bool have = value && strncmp(value, "yes|", 4) == 0;
            if (want != have) { lua_pop(L, 1); return false; }
        }
        lua_pop(L, 1);
    }
    return true;
}

static int64_t last_dispatch_for(ghostscript_runtime_t *rt, const char *name) {
    /* Small fixed bucket index. We don't need exact; just any stable hash. */
    uint32_t h = 5381u;
    for (const char *p = name; *p; ++p) h = h * 33u + (uint8_t)*p;
    return rt->last_dispatch_us[h % 16];
}

static void note_dispatch(ghostscript_runtime_t *rt, const char *name, int64_t now) {
    uint32_t h = 5381u;
    for (const char *p = name; *p; ++p) h = h * 33u + (uint8_t)*p;
    rt->last_dispatch_us[h % 16] = now;
}

/* ghost.exit() raises a Lua error to unwind the current callback immediately.
 * Convert that private control flow into a normal completed script state. */
static bool runtime_finish_requested_exit(ghostscript_runtime_t *rt, lua_State *L) {
    if (!rt || !rt->stop_requested) return false;
    if (L && lua_gettop(L) > 0) lua_pop(L, 1);
    rt->script_done = true;
    rt->state = GHOSTSCRIPT_STATE_DONE;
    return true;
}

static void dispatch_event(ghostscript_runtime_t *rt, const char *name, const char *value) {
    if (!rt || !rt->L || !name) return;
    if (strcmp(name, "nfc_tag") == 0 && value) {
        snprintf(rt->last_nfc_tag, sizeof(rt->last_nfc_tag), "%s", value);
    }
    /* Rate limit: 10 ms minimum interval per topic. Handlers in _waits are
     * exempt because they need to wake up on first match. */
    int64_t now = esp_timer_get_time();
    if (last_dispatch_for(rt, name) != 0 &&
        (now - last_dispatch_for(rt, name)) < 10000 &&
        strcmp(name, "command.output") == 0) {
        /* High-rate topic: drop. */
        return;
    }
    note_dispatch(rt, name, now);

    lua_getfield(rt->L, LUA_REGISTRYINDEX, EVENT_KEY);
    if (!lua_istable(rt->L, -1)) { lua_pop(rt->L, 1); return; }
    lua_getfield(rt->L, -1, name);
    if (lua_istable(rt->L, -1)) {
        int n = (int)lua_rawlen(rt->L, -1);
        /* Pairs are stored as [fn, filter, fn, filter, ...]. */
        for (int i = 1; i <= n; i += 2) {
            lua_rawgeti(rt->L, -1, i);
            if (!lua_isfunction(rt->L, -1)) { lua_pop(rt->L, 1); continue; }
            /* Filter is at i+1 in the listener table, which is at -2: the
             * handler function pushed above is at -1. */
            if (i + 1 <= n) {
                lua_rawgeti(rt->L, -2, i + 1);
                if (lua_isnil(rt->L, -1) || lua_isboolean(rt->L, -1)) {
                    lua_pop(rt->L, 1);
                } else {
                    bool pass = topic_passes_filter(rt->L, name, value);
                    if (!pass) { lua_pop(rt->L, 2); continue; }
                    lua_pop(rt->L, 1);
                }
            }
            lua_pushstring(rt->L, value ? value : "");
            runtime_begin_slice(rt);
            if (lua_pcall(rt->L, 1, 0, 0) != LUA_OK) {
                if (runtime_finish_requested_exit(rt, rt->L)) break;
                glog("event handler error: %s", lua_tostring(rt->L, -1));
                lua_pop(rt->L, 1);
            }
        }
    }
    if (rt->state != GHOSTSCRIPT_STATE_RUNNING) {
        lua_pop(rt->L, 2);
        return;
    }
    lua_pop(rt->L, 1);
    lua_getfield(rt->L, -1, "_waits");
    if (!lua_istable(rt->L, -1)) { lua_pop(rt->L, 2); return; }
    lua_getfield(rt->L, -1, "_next_wait_id");
    int n = lua_isinteger(rt->L, -1) ? (int)lua_tointeger(rt->L, -1) : 0;
    lua_pop(rt->L, 1);
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(rt->L, -1, i);
        if (!lua_istable(rt->L, -1)) {
            lua_pop(rt->L, 1);
            continue;
        }
        lua_getfield(rt->L, -1, "name");
        bool match = lua_isstring(rt->L, -1) && strcmp(lua_tostring(rt->L, -1), name) == 0;
        lua_pop(rt->L, 1);
        if (!match) {
            lua_pop(rt->L, 1);
            continue;
        }
        lua_pushnil(rt->L);
        lua_rawseti(rt->L, -3, i);
        lua_getfield(rt->L, -1, "co");
        lua_State *co = lua_tothread(rt->L, -1);
        lua_pop(rt->L, 1);
        if (co) {
            if (co == rt->thread) rt->waiting_for_event = false;
            lua_newtable(co);
            lua_pushstring(co, value ? value : "");
            lua_setfield(co, -2, "value");
            runtime_begin_slice(rt);
            int nres = 0;
            int rc = lua_resume(co, NULL, 1, &nres);
            if (rc != LUA_OK && rc != LUA_YIELD) {
                if (runtime_finish_requested_exit(rt, co)) {
                    lua_pop(rt->L, 1);
                    break;
                }
                glog("wait resume error: %s", lua_tostring(co, -1));
                lua_pop(co, 1);
            } else if (nres > 0) {
                lua_pop(co, nres);
            }
        }
        lua_pop(rt->L, 1);
    }
    lua_pop(rt->L, 2);
}

static void runtime_expire_waits(ghostscript_runtime_t *rt) {
    if (!rt || !rt->L) return;
    lua_getfield(rt->L, LUA_REGISTRYINDEX, EVENT_KEY);
    if (!lua_istable(rt->L, -1)) { lua_pop(rt->L, 1); return; }
    lua_getfield(rt->L, -1, "_waits");
    if (!lua_istable(rt->L, -1)) { lua_pop(rt->L, 2); return; }
    lua_getfield(rt->L, -1, "_next_wait_id");
    int n = lua_isinteger(rt->L, -1) ? (int)lua_tointeger(rt->L, -1) : 0;
    lua_pop(rt->L, 1);
    int64_t now = esp_timer_get_time();
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(rt->L, -1, i);
        if (!lua_istable(rt->L, -1)) { lua_pop(rt->L, 1); continue; }
        lua_getfield(rt->L, -1, "deadline_us");
        bool expired = lua_isinteger(rt->L, -1) && (int64_t)lua_tointeger(rt->L, -1) <= now;
        lua_pop(rt->L, 1);
        if (!expired) { lua_pop(rt->L, 1); continue; }
        lua_pushnil(rt->L);
        lua_rawseti(rt->L, -3, i);
        lua_getfield(rt->L, -1, "co");
        lua_State *co = lua_tothread(rt->L, -1);
        lua_pop(rt->L, 1);
        if (co) {
            if (co == rt->thread) rt->waiting_for_event = false;
            lua_pushnil(co);
            int nres = 0;
            int rc = lua_resume(co, NULL, 1, &nres);
            if (rc != LUA_OK && rc != LUA_YIELD) {
                if (runtime_finish_requested_exit(rt, co)) {
                    lua_pop(rt->L, 1);
                    break;
                }
                glog("wait timeout error: %s", lua_tostring(co, -1));
                lua_pop(co, 1);
            } else if (nres > 0) {
                lua_pop(co, nres);
            }
        }
        lua_pop(rt->L, 1);
        if (rt->state != GHOSTSCRIPT_STATE_RUNNING) break;
    }
    lua_pop(rt->L, 2);
}

static int l_exit(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (rt) rt->stop_requested = true;
    return luaL_error(L, "script exit requested");
}

static int l_free_heap(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    lua_pushinteger(L, rt && rt->api && rt->api->system_free_heap ? (lua_Integer)rt->api->system_free_heap() : 0);
    return 1;
}

static int l_free_internal_heap(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    lua_pushinteger(L, rt && rt->api && rt->api->system_free_internal_heap ? (lua_Integer)rt->api->system_free_internal_heap() : 0);
    return 1;
}

static int l_uptime_ms(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    lua_pushinteger(L, rt && rt->api && rt->api->system_uptime_ms ? (lua_Integer)rt->api->system_uptime_ms() : 0);
    return 1;
}

static int l_memory_used(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    lua_pushinteger(L, rt ? (lua_Integer)rt->memory_used : 0);
    return 1;
}

static int l_memory_limit(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    lua_pushinteger(L, rt ? (lua_Integer)rt->memory_limit : 0);
    return 1;
}

static int l_storage_read(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_STORAGE)) return lua_perm_error(L, "storage");
    const char *rel = luaL_checkstring(L, 1);
    char path[GHOSTSCRIPT_PATH_MAX];
    if (!scoped_storage_path(rt, rel, path, sizeof(path))) return luaL_error(L, "invalid scoped path");
    bool display_was_suspended = false;
    if (!ghostscript_manager_sd_begin(&display_was_suspended)) { lua_pushnil(L); return 1; }
    FILE *f = fopen(path, "rb");
    if (!f) { ghostscript_manager_sd_end(display_was_suspended); lua_pushnil(L); return 1; }
    char *buf = malloc(GS_READ_BUF_MAX);
    if (!buf) { fclose(f); ghostscript_manager_sd_end(display_was_suspended); return luaL_error(L, "out of host memory"); }
    size_t n = fread(buf, 1, GS_READ_BUF_MAX, f);
    fclose(f);
    ghostscript_manager_sd_end(display_was_suspended);
    lua_pushlstring(L, buf, n);
    free(buf);
    return 1;
}

static int l_storage_write(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_STORAGE)) return lua_perm_error(L, "storage");
    const char *rel = luaL_checkstring(L, 1);
    size_t len = 0;
    const char *data = luaL_checklstring(L, 2, &len);
    char path[GHOSTSCRIPT_PATH_MAX];
    if (!scoped_storage_path(rt, rel, path, sizeof(path))) return luaL_error(L, "invalid scoped path");
    bool display_was_suspended = false;
    if (!ghostscript_manager_sd_begin(&display_was_suspended)) { lua_pushboolean(L, false); return 1; }
    sd_card_create_directory(GHOSTSCRIPT_DATA_DIR);
    sd_card_create_directory(rt->manifest.data_path);
    FILE *f = fopen(path, "wb");
    if (!f) { ghostscript_manager_sd_end(display_was_suspended); lua_pushboolean(L, false); return 1; }
    bool ok = fwrite(data, 1, len, f) == len;
    fclose(f);
    ghostscript_manager_sd_end(display_was_suspended);
    lua_pushboolean(L, ok);
    return 1;
}

static int l_storage_exists(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_STORAGE)) return lua_perm_error(L, "storage");
    const char *rel = luaL_checkstring(L, 1);
    char path[GHOSTSCRIPT_PATH_MAX];
    if (!scoped_storage_path(rt, rel, path, sizeof(path))) { lua_pushboolean(L, false); return 1; }
    bool display_was_suspended = false;
    if (!ghostscript_manager_sd_begin(&display_was_suspended)) { lua_pushboolean(L, false); return 1; }
    struct stat st;
    bool exists = stat(path, &st) == 0;
    ghostscript_manager_sd_end(display_was_suspended);
    lua_pushboolean(L, exists);
    return 1;
}

static int l_wifi_scan_start(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_WIFI)) return lua_perm_error(L, "wifi");
    if (rt->api && rt->api->wifi_start_scan_async) {
        lua_pushboolean(L, rt->api->wifi_start_scan_async());
        return 1;
    }
    lua_pushboolean(L, rt->api && rt->api->wifi_start_scan ? rt->api->wifi_start_scan() : false);
    return 1;
}

static int l_wifi_scan_done(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_WIFI)) return lua_perm_error(L, "wifi");
    lua_pushboolean(L, rt->api && rt->api->wifi_scan_check_done ? rt->api->wifi_scan_check_done() : true);
    return 1;
}

static int l_wifi_scan_finish(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_WIFI)) return lua_perm_error(L, "wifi");
    if (rt->api && rt->api->wifi_finish_scan) rt->api->wifi_finish_scan();
    lua_pushinteger(L, rt->api && rt->api->wifi_ap_count ? rt->api->wifi_ap_count() : 0);
    return 1;
}

static int l_wifi_count(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_WIFI)) return lua_perm_error(L, "wifi");
    lua_pushinteger(L, rt->api && rt->api->wifi_ap_count ? rt->api->wifi_ap_count() : 0);
    return 1;
}

static int l_wifi_ap(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_WIFI)) return lua_perm_error(L, "wifi");
    int idx = (int)luaL_checkinteger(L, 1);
    ghostesp_wifi_ap_info_t ap;
    if (!rt->api || !rt->api->wifi_scan_get_ap || !rt->api->wifi_scan_get_ap((uint16_t)idx, &ap)) { lua_pushnil(L); return 1; }
    lua_newtable(L);
    lua_pushstring(L, ap.ssid); lua_setfield(L, -2, "ssid");
    lua_pushinteger(L, ap.channel); lua_setfield(L, -2, "channel");
    lua_pushinteger(L, ap.rssi); lua_setfield(L, -2, "rssi");
    lua_pushinteger(L, ap.auth_mode); lua_setfield(L, -2, "auth");
    char bssid[18];
    snprintf(bssid, sizeof(bssid), "%02x:%02x:%02x:%02x:%02x:%02x",
        ap.bssid[0], ap.bssid[1], ap.bssid[2], ap.bssid[3], ap.bssid[4], ap.bssid[5]);
    lua_pushstring(L, bssid); lua_setfield(L, -2, "bssid");
    return 1;
}

static int l_bool0(lua_State *L, uint32_t perm, bool (*fn)(void), const char *name) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, perm)) return lua_perm_error(L, name);
    lua_pushboolean(L, fn ? fn() : false);
    return 1;
}

static int l_ble_start(lua_State *L) { ghostscript_runtime_t *rt = check_rt(L); return l_bool0(L, PLUGIN_PERMISSION_BLE, rt && rt->api ? rt->api->ble_start_scan : NULL, "ble"); }
static int l_ble_stop(lua_State *L) { ghostscript_runtime_t *rt = check_rt(L); return l_bool0(L, PLUGIN_PERMISSION_BLE, rt && rt->api ? rt->api->ble_stop_scan : NULL, "ble"); }
static int l_rgb_set(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_RGB)) return lua_perm_error(L, "rgb");
    int r = luaL_checkinteger(L, 1), g = luaL_checkinteger(L, 2), b = luaL_checkinteger(L, 3);
    lua_pushboolean(L, rt->api && rt->api->rgb_set_all ? rt->api->rgb_set_all((uint8_t)r, (uint8_t)g, (uint8_t)b) : false);
    return 1;
}

static int l_badusb_run(lua_State *L) { ghostscript_runtime_t *rt = check_rt(L); if (!rt_has_perm(rt, PLUGIN_PERMISSION_BADUSB)) return lua_perm_error(L, "badusb"); lua_pushboolean(L, rt->api && rt->api->badusb_run_script ? rt->api->badusb_run_script(luaL_checkstring(L, 1)) : false); return 1; }
static int l_ir_send(lua_State *L) { ghostscript_runtime_t *rt = check_rt(L); if (!rt_has_perm(rt, PLUGIN_PERMISSION_IR)) return lua_perm_error(L, "ir"); lua_pushboolean(L, rt->api && rt->api->ir_send_file ? rt->api->ir_send_file(luaL_checkstring(L, 1)) : false); return 1; }

static int l_ir_listen(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_IR)) return lua_perm_error(L, "ir");
    int timeout_ms = (int)luaL_optinteger(L, 1, 5000);
    if (timeout_ms < 0) timeout_ms = 0;
    if (timeout_ms > 120000) timeout_ms = 120000;
    if (!infrared_manager_rx_init()) {
        lua_pushnil(L);
        return 1;
    }
    infrared_signal_t sig;
    memset(&sig, 0, sizeof(sig));
    bool got = infrared_manager_rx_receive(&sig, timeout_ms);
    if (!got) {
        infrared_manager_rx_deinit();
        lua_pushnil(L);
        return 1;
    }
    /* No Lua allocation should leave the receiver running after an OOM error. */
    infrared_manager_rx_deinit();
    lua_newtable(L);
    if (sig.is_raw) {
        lua_pushboolean(L, true);
        lua_setfield(L, -2, "raw");
        lua_pushinteger(L, (lua_Integer)sig.payload.raw.timings_size);
        lua_setfield(L, -2, "count");
        lua_pushinteger(L, (lua_Integer)sig.payload.raw.frequency);
        lua_setfield(L, -2, "frequency");
        lua_createtable(L, (int)sig.payload.raw.timings_size, 0);
        for (size_t i = 0; i < sig.payload.raw.timings_size; i++) {
            lua_pushinteger(L, (lua_Integer)sig.payload.raw.timings[i]);
            lua_rawseti(L, -2, (int)i + 1);
        }
        lua_setfield(L, -2, "timings");
    } else {
        lua_pushstring(L, sig.payload.message.protocol);
        lua_setfield(L, -2, "protocol");
        lua_pushinteger(L, (lua_Integer)sig.payload.message.address);
        lua_setfield(L, -2, "address");
        lua_pushinteger(L, (lua_Integer)sig.payload.message.command);
        lua_setfield(L, -2, "command");
    }
    infrared_manager_free_signal(&sig);
    return 1;
}
static int l_subghz_load(lua_State *L) { ghostscript_runtime_t *rt = check_rt(L); if (!rt_has_perm(rt, PLUGIN_PERMISSION_SUBGHZ)) return lua_perm_error(L, "subghz"); lua_pushboolean(L, rt->api && rt->api->subghz_load_snapshot ? rt->api->subghz_load_snapshot(luaL_checkstring(L, 1)) : false); return 1; }
static int l_subghz_tx(lua_State *L) { ghostscript_runtime_t *rt = check_rt(L); return l_bool0(L, PLUGIN_PERMISSION_SUBGHZ, rt && rt->api ? rt->api->subghz_transmit_loaded : NULL, "subghz"); }

static int l_subghz_receive(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_SUBGHZ)) return lua_perm_error(L, "subghz");
    int timeout_ms = (int)luaL_optinteger(L, 1, 5000);
    uint32_t freq = (uint32_t)luaL_optinteger(L, 2, 0);
    if (timeout_ms < 0) timeout_ms = 0;
    if (timeout_ms > 120000) timeout_ms = 120000;
    if (freq == 0) freq = subghz_remote_manager_get_frequency_hz();
    if (!subghz_remote_manager_begin_capture(true, freq, false, (uint32_t)timeout_ms)) {
        lua_pushnil(L);
        return 1;
    }
    subghz_decoded_signal_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    if (subghz_remote_manager_take_decode_result(&decoded) && decoded.decoded) {
        lua_newtable(L);
        lua_pushstring(L, decoded.protocol);
        lua_setfield(L, -2, "protocol");
        lua_pushstring(L, decoded.info);
        lua_setfield(L, -2, "info");
        lua_pushinteger(L, (lua_Integer)decoded.code);
        lua_setfield(L, -2, "code");
        lua_pushinteger(L, decoded.bits);
        lua_setfield(L, -2, "bits");
        lua_pushinteger(L, decoded.frequency_hz);
        lua_setfield(L, -2, "frequency");
        lua_pushinteger(L, decoded.te);
        lua_setfield(L, -2, "te");
        return 1;
    }
    int32_t durations[256];
    size_t count = 0;
    if (subghz_remote_manager_take_raw_capture(durations, 256, &count) && count > 0) {
        lua_newtable(L);
        lua_pushboolean(L, true);
        lua_setfield(L, -2, "raw");
        lua_pushinteger(L, (lua_Integer)count);
        lua_setfield(L, -2, "count");
        lua_pushinteger(L, (lua_Integer)freq);
        lua_setfield(L, -2, "frequency");
        lua_createtable(L, (int)count, 0);
        for (size_t i = 0; i < count; i++) {
            lua_pushinteger(L, (lua_Integer)durations[i]);
            lua_rawseti(L, -2, (int)i + 1);
        }
        lua_setfield(L, -2, "durations");
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

static int l_subghz_read_raw(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_SUBGHZ)) return lua_perm_error(L, "subghz");
    int32_t durations[256];
    size_t count = 0;
    if (!subghz_remote_manager_take_raw_capture(durations, 256, &count) || count == 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)count);
    lua_setfield(L, -2, "count");
    lua_pushinteger(L, (lua_Integer)subghz_remote_manager_get_frequency_hz());
    lua_setfield(L, -2, "frequency");
    lua_createtable(L, (int)count, 0);
    for (size_t i = 0; i < count; i++) {
        lua_pushinteger(L, (lua_Integer)durations[i]);
        lua_rawseti(L, -2, (int)i + 1);
    }
    lua_setfield(L, -2, "durations");
    return 1;
}

static int l_http_get(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_NETWORK)) return lua_perm_error(L, "network");
    const char *url = luaL_checkstring(L, 1);
    char *buf = malloc(GS_READ_BUF_MAX);
    if (!buf) return luaL_error(L, "out of host memory");
    int n = rt->api && rt->api->http_get ? rt->api->http_get(url, buf, GS_READ_BUF_MAX, 10000) : -1;
    if (n < 0) lua_pushnil(L); else lua_pushlstring(L, buf, (size_t)n);
    free(buf);
    return 1;
}

static int l_http_post(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_NETWORK)) return lua_perm_error(L, "network");
    const char *url = luaL_checkstring(L, 1);
    size_t body_len = 0;
    const char *body = luaL_checklstring(L, 2, &body_len);
    char *buf = malloc(GS_READ_BUF_MAX);
    if (!buf) return luaL_error(L, "out of host memory");
    int n = rt->api && rt->api->http_post ? rt->api->http_post(url, body, body_len, buf, GS_READ_BUF_MAX, 10000) : -1;
    if (n < 0) lua_pushnil(L); else lua_pushlstring(L, buf, (size_t)n);
    free(buf);
    return 1;
}

static int l_storage_append(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_STORAGE)) return lua_perm_error(L, "storage");
    const char *rel = luaL_checkstring(L, 1);
    size_t len = 0;
    const char *data = luaL_checklstring(L, 2, &len);
    char path[GHOSTSCRIPT_PATH_MAX];
    if (!scoped_storage_path(rt, rel, path, sizeof(path))) return luaL_error(L, "invalid scoped path");
    bool display_was_suspended = false;
    if (!ghostscript_manager_sd_begin(&display_was_suspended)) { lua_pushboolean(L, false); return 1; }
    sd_card_create_directory(GHOSTSCRIPT_DATA_DIR);
    sd_card_create_directory(rt->manifest.data_path);
    FILE *f = fopen(path, "ab");
    bool ok = false;
    if (f) {
        ok = fwrite(data, 1, len, f) == len;
        fclose(f);
    }
    lua_pushboolean(L, ok);
    ghostscript_manager_sd_end(display_was_suspended);
    return 1;
}

static gs_storage_stream_t *check_storage_stream(lua_State *L, ghostscript_runtime_t *rt) {
    gs_storage_stream_t *stream = (gs_storage_stream_t *)luaL_checkudata(L, 1, GS_STORAGE_STREAM_MT);
    if (!stream || stream->runtime != rt || stream->closed) {
        luaL_error(L, "storage stream is closed or belongs to another runtime");
    }
    return stream;
}

static int l_storage_stream_gc(lua_State *L) {
    gs_storage_stream_t *stream = (gs_storage_stream_t *)luaL_testudata(L, 1, GS_STORAGE_STREAM_MT);
    if (stream) stream->closed = true;
    return 0;
}

static int storage_stream_error(lua_State *L, const char *message) {
    lua_pushnil(L);
    lua_pushstring(L, message);
    return 2;
}

static int l_storage_open(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_STORAGE)) return lua_perm_error(L, "storage");
    const char *rel = luaL_checkstring(L, 1);
    const char *mode = luaL_checkstring(L, 2);
    if (mode[0] == '\0' || mode[1] != '\0' || (mode[0] != 'r' && mode[0] != 'w' && mode[0] != 'a')) {
        return luaL_error(L, "storage mode must be r, w, or a");
    }

    char path[GHOSTSCRIPT_PATH_MAX];
    if (!scoped_storage_path(rt, rel, path, sizeof(path))) return storage_stream_error(L, "invalid scoped path");
    bool display_was_suspended = false;
    if (!ghostscript_manager_sd_begin(&display_was_suspended)) return storage_stream_error(L, "SD card unavailable");
    if (mode[0] != 'r') {
        sd_card_create_directory(GHOSTSCRIPT_DATA_DIR);
        sd_card_create_directory(rt->manifest.data_path);
    }
    FILE *file = fopen(path, mode[0] == 'r' ? "rb" : (mode[0] == 'w' ? "wb" : "ab"));
    size_t cursor = 0;
    bool ok = file != NULL;
    if (file && mode[0] == 'a') {
        if (fseek(file, 0, SEEK_END) != 0) ok = false;
        long offset = ok ? ftell(file) : -1;
        if (offset < 0) ok = false;
        else cursor = (size_t)offset;
    }
    if (file) fclose(file);
    ghostscript_manager_sd_end(display_was_suspended);
    if (!ok) return storage_stream_error(L, "unable to open storage stream");

    gs_storage_stream_t *stream = (gs_storage_stream_t *)lua_newuserdatauv(L, sizeof(*stream), 0);
    memset(stream, 0, sizeof(*stream));
    stream->runtime = rt;
    stream->cursor = cursor;
    stream->mode = mode[0];
    snprintf(stream->path, sizeof(stream->path), "%s", path);
    luaL_getmetatable(L, GS_STORAGE_STREAM_MT);
    lua_setmetatable(L, -2);
    return 1;
}

static int l_storage_stream_read(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_STORAGE)) return lua_perm_error(L, "storage");
    gs_storage_stream_t *stream = check_storage_stream(L, rt);
    if (stream->mode != 'r') return storage_stream_error(L, "storage stream is not readable");
    lua_Integer requested = luaL_optinteger(L, 2, GS_STORAGE_STREAM_CHUNK_MAX);
    if (requested <= 0 || requested > GS_STORAGE_STREAM_CHUNK_MAX) return luaL_error(L, "read size must be between 1 and %d", GS_STORAGE_STREAM_CHUNK_MAX);

    char buffer[GS_STORAGE_STREAM_CHUNK_MAX];
    bool display_was_suspended = false;
    if (!ghostscript_manager_sd_begin(&display_was_suspended)) return storage_stream_error(L, "SD card unavailable");
    FILE *file = fopen(stream->path, "rb");
    size_t count = 0;
    bool ok = file != NULL && fseek(file, (long)stream->cursor, SEEK_SET) == 0;
    if (ok) {
        count = fread(buffer, 1, (size_t)requested, file);
        ok = !ferror(file);
    }
    if (file) fclose(file);
    ghostscript_manager_sd_end(display_was_suspended);
    if (!ok) return storage_stream_error(L, "storage stream read failed");
    if (count == 0) { lua_pushnil(L); return 1; }
    stream->cursor += count;
    lua_pushlstring(L, buffer, count);
    return 1;
}

static int l_storage_stream_write(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_STORAGE)) return lua_perm_error(L, "storage");
    gs_storage_stream_t *stream = check_storage_stream(L, rt);
    if (stream->mode == 'r') return storage_stream_error(L, "storage stream is not writable");
    size_t len = 0;
    const char *data = luaL_checklstring(L, 2, &len);
    if (len > GS_STORAGE_STREAM_CHUNK_MAX) return luaL_error(L, "write size exceeds %d bytes", GS_STORAGE_STREAM_CHUNK_MAX);
    if (len == 0) { lua_pushinteger(L, 0); return 1; }

    bool display_was_suspended = false;
    if (!ghostscript_manager_sd_begin(&display_was_suspended)) return storage_stream_error(L, "SD card unavailable");
    FILE *file = fopen(stream->path, "r+b");
    bool ok = file != NULL && fseek(file, (long)stream->cursor, SEEK_SET) == 0;
    if (ok) ok = fwrite(data, 1, len, file) == len && fflush(file) == 0;
    if (file) fclose(file);
    ghostscript_manager_sd_end(display_was_suspended);
    if (!ok) return storage_stream_error(L, "storage stream write failed");
    stream->cursor += len;
    lua_pushinteger(L, (lua_Integer)len);
    return 1;
}

static int l_storage_close(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_STORAGE)) return lua_perm_error(L, "storage");
    gs_storage_stream_t *stream = (gs_storage_stream_t *)luaL_checkudata(L, 1, GS_STORAGE_STREAM_MT);
    if (stream->runtime != rt) return luaL_error(L, "storage stream belongs to another runtime");
    stream->closed = true;
    lua_pushboolean(L, true);
    return 1;
}

static int l_storage_delete(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_STORAGE)) return lua_perm_error(L, "storage");
    const char *rel = luaL_checkstring(L, 1);
    char path[GHOSTSCRIPT_PATH_MAX];
    if (!scoped_storage_path(rt, rel, path, sizeof(path))) { lua_pushboolean(L, false); return 1; }
    bool display_was_suspended = false;
    if (!ghostscript_manager_sd_begin(&display_was_suspended)) { lua_pushboolean(L, false); return 1; }
    lua_pushboolean(L, unlink(path) == 0);
    ghostscript_manager_sd_end(display_was_suspended);
    return 1;
}

static int l_storage_mkdir(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_STORAGE)) return lua_perm_error(L, "storage");
    const char *rel = luaL_checkstring(L, 1);
    char path[GHOSTSCRIPT_PATH_MAX];
    if (!scoped_storage_path(rt, rel, path, sizeof(path))) { lua_pushboolean(L, false); return 1; }
    bool display_was_suspended = false;
    if (!ghostscript_manager_sd_begin(&display_was_suspended)) { lua_pushboolean(L, false); return 1; }
    lua_pushboolean(L, sd_card_create_directory(path) == ESP_OK);
    ghostscript_manager_sd_end(display_was_suspended);
    return 1;
}

static int l_storage_list_build(lua_State *L) {
    gs_storage_list_ctx_t *ctx = (gs_storage_list_ctx_t *)lua_touserdata(L, lua_upvalueindex(1));
    if (!ctx) { lua_newtable(L); return 1; }
    ctx->dir = opendir(ctx->path);
    int count = 0;
    struct dirent *entry;
    lua_newtable(L);
    while (ctx->dir && (entry = readdir(ctx->dir)) != NULL && count < GS_STORAGE_LIST_MAX) {
        if (entry->d_name[0] == '.') continue;
        char child[GHOSTSCRIPT_PATH_MAX];
        if (!safe_join(ctx->path, entry->d_name, child, sizeof(child))) continue;
        struct stat st;
        bool is_dir = stat(child, &st) == 0 && S_ISDIR(st.st_mode);
        lua_newtable(L);
        lua_pushstring(L, entry->d_name); lua_setfield(L, -2, "name");
        lua_pushboolean(L, is_dir); lua_setfield(L, -2, "is_dir");
        lua_rawseti(L, -2, ++count);
    }
    if (ctx->dir) {
        closedir(ctx->dir);
        ctx->dir = NULL;
    }
    return 1;
}

static int l_storage_list(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_STORAGE)) return lua_perm_error(L, "storage");
    const char *rel = luaL_checkstring(L, 1);
    gs_storage_list_ctx_t ctx = { 0 };
    if (!scoped_storage_path(rt, rel, ctx.path, sizeof(ctx.path))) { lua_newtable(L); return 1; }
    bool display_was_suspended = false;
    if (!ghostscript_manager_sd_begin(&display_was_suspended)) { lua_newtable(L); return 1; }
    lua_pushlightuserdata(L, &ctx);
    lua_pushcclosure(L, l_storage_list_build, 1);
    int rc = lua_pcall(L, 0, 1, 0);
    if (ctx.dir) closedir(ctx.dir);
    ghostscript_manager_sd_end(display_was_suspended);
    if (rc != LUA_OK) {
        lua_pop(L, 1);
        lua_newtable(L);
    }
    return 1;
}

static int l_storage_stat(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_STORAGE)) return lua_perm_error(L, "storage");
    const char *rel = luaL_checkstring(L, 1);
    char path[GHOSTSCRIPT_PATH_MAX];
    if (!scoped_storage_path(rt, rel, path, sizeof(path))) { lua_pushnil(L); return 1; }
    bool display_was_suspended = false;
    if (!ghostscript_manager_sd_begin(&display_was_suspended)) { lua_pushnil(L); return 1; }
    struct stat st;
    bool ok = stat(path, &st) == 0;
    ghostscript_manager_sd_end(display_was_suspended);
    if (!ok) { lua_pushnil(L); return 1; }
    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)st.st_size); lua_setfield(L, -2, "size");
    lua_pushboolean(L, S_ISDIR(st.st_mode)); lua_setfield(L, -2, "is_dir");
    return 1;
}

static int l_storage_rename(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_STORAGE)) return lua_perm_error(L, "storage");
    const char *from = luaL_checkstring(L, 1);
    const char *to = luaL_checkstring(L, 2);
    char from_path[GHOSTSCRIPT_PATH_MAX];
    char to_path[GHOSTSCRIPT_PATH_MAX];
    if (!scoped_storage_path(rt, from, from_path, sizeof(from_path)) || !scoped_storage_path(rt, to, to_path, sizeof(to_path))) { lua_pushboolean(L, false); return 1; }
    bool display_was_suspended = false;
    if (!ghostscript_manager_sd_begin(&display_was_suspended)) { lua_pushboolean(L, false); return 1; }
    lua_pushboolean(L, rename(from_path, to_path) == 0);
    ghostscript_manager_sd_end(display_was_suspended);
    return 1;
}

static int l_wifi_stop_scan(lua_State *L) { ghostscript_runtime_t *rt = check_rt(L); return l_bool0(L, PLUGIN_PERMISSION_WIFI, rt && rt->api ? rt->api->wifi_stop_scan : NULL, "wifi"); }

static int l_wifi_connect(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_WIFI_CONTROL)) return lua_perm_error(L, "wifi.control");
    const char *ssid = luaL_checkstring(L, 1);
    const char *pass = luaL_optstring(L, 2, "");
    uint32_t timeout = (uint32_t)luaL_optinteger(L, 3, 10000);
    lua_pushboolean(L, rt->api && rt->api->wifi_connect ? rt->api->wifi_connect(ssid, pass, timeout) : false);
    return 1;
}

static int l_wifi_disconnect(lua_State *L) { ghostscript_runtime_t *rt = check_rt(L); return l_bool0(L, PLUGIN_PERMISSION_WIFI_CONTROL, rt && rt->api ? rt->api->wifi_disconnect : NULL, "wifi.control"); }
static int l_wifi_is_connected(lua_State *L) { ghostscript_runtime_t *rt = check_rt(L); return l_bool0(L, PLUGIN_PERMISSION_WIFI, rt && rt->api ? rt->api->wifi_is_connected : NULL, "wifi"); }

static int l_wifi_rssi(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_WIFI)) return lua_perm_error(L, "wifi");
    lua_pushinteger(L, rt->api && rt->api->wifi_rssi ? rt->api->wifi_rssi() : 0);
    return 1;
}

static int l_wifi_ip(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_WIFI)) return lua_perm_error(L, "wifi");
    char ip[32] = {0};
    bool ok = rt->api && rt->api->wifi_ip && rt->api->wifi_ip(ip, sizeof(ip));
    if (ok && ip[0]) lua_pushstring(L, ip); else lua_pushnil(L);
    return 1;
}

static int l_wifi_set_channel(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_WIFI_CONTROL)) return lua_perm_error(L, "wifi.control");
    int ch = luaL_checkinteger(L, 1);
    lua_pushboolean(L, rt->api && rt->api->wifi_set_channel ? rt->api->wifi_set_channel((uint8_t)ch) : false);
    return 1;
}

static int l_wifi_deauth(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_WIFI_CONTROL)) return lua_perm_error(L, "wifi.control");
    size_t bssid_len = 0;
    const char *bssid = luaL_checklstring(L, 1, &bssid_len);
    uint8_t b[6] = {0};
    if (bssid_len > 0) {
        int bi = 0;
        for (size_t i = 0; i < bssid_len && bi < 6; ++i) {
            char c = bssid[i];
            if (c == ':' || c == '-' || c == ' ') continue;
            unsigned int v = 0;
            char tmp[3] = { c, (i + 1 < bssid_len) ? bssid[i + 1] : 0, 0 };
            if (sscanf(tmp, "%x", &v) == 1) {
                b[bi++] = (uint8_t)v;
                i++;
            }
        }
    }
    int reason = (int)luaL_optinteger(L, 2, 1);
    lua_pushboolean(L, rt->api && rt->api->wifi_deauth ? rt->api->wifi_deauth(b, NULL, (uint8_t)reason) : false);
    return 1;
}

static int l_wifi_station_scan_start(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_WIFI)) return lua_perm_error(L, "wifi");
    station_scan_start();
    lua_pushboolean(L, true);
    return 1;
}

static int l_wifi_station_scan_stop(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_WIFI)) return lua_perm_error(L, "wifi");
    station_scan_stop();
    lua_pushboolean(L, true);
    return 1;
}

static int l_wifi_station_count(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_WIFI)) return lua_perm_error(L, "wifi");
    lua_pushinteger(L, station_scan_get_count());
    return 1;
}

static int l_wifi_station(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_WIFI)) return lua_perm_error(L, "wifi");
    int idx = (int)luaL_checkinteger(L, 1);
    if (idx < 0 || idx >= station_scan_get_count()) { lua_pushnil(L); return 1; }
    station_ap_pair_t *p = &station_ap_list[idx];
    lua_newtable(L);
    char mac[18];
    snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
        p->station_mac[0], p->station_mac[1], p->station_mac[2],
        p->station_mac[3], p->station_mac[4], p->station_mac[5]);
    lua_pushstring(L, mac);
    lua_setfield(L, -2, "mac");
    char bssid[18];
    snprintf(bssid, sizeof(bssid), "%02x:%02x:%02x:%02x:%02x:%02x",
        p->ap_bssid[0], p->ap_bssid[1], p->ap_bssid[2],
        p->ap_bssid[3], p->ap_bssid[4], p->ap_bssid[5]);
    lua_pushstring(L, bssid);
    lua_setfield(L, -2, "bssid");
    return 1;
}

static bool result_wifi_ap_count(ghostscript_runtime_t *rt, int *out) {
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_WIFI) || !out) return false;
    *out = rt->api && rt->api->wifi_ap_count ? rt->api->wifi_ap_count() : 0;
    return true;
}

static bool result_wifi_ap_field(ghostscript_runtime_t *rt, int idx, const char *field,
                                 char *str, size_t str_len, lua_Integer *num, bool *is_num) {
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_WIFI) || !field) return false;
    ghostesp_wifi_ap_info_t ap;
    if (!rt->api || !rt->api->wifi_scan_get_ap || !rt->api->wifi_scan_get_ap((uint16_t)idx, &ap)) return false;
    if (strcmp(field, "bssid") == 0) {
        snprintf(str, str_len, "%02x:%02x:%02x:%02x:%02x:%02x",
                 ap.bssid[0], ap.bssid[1], ap.bssid[2], ap.bssid[3], ap.bssid[4], ap.bssid[5]);
        if (is_num) *is_num = false;
        return true;
    }
    if (strcmp(field, "ssid") == 0) {
        snprintf(str, str_len, "%s", ap.ssid);
        if (is_num) *is_num = false;
        return true;
    }
    if (strcmp(field, "channel") == 0) { if (num) *num = ap.channel; if (is_num) *is_num = true; return true; }
    if (strcmp(field, "rssi") == 0) { if (num) *num = ap.rssi; if (is_num) *is_num = true; return true; }
    if (strcmp(field, "auth") == 0) { if (num) *num = ap.auth_mode; if (is_num) *is_num = true; return true; }
    return false;
}

static bool result_wifi_ap_write_csv(ghostscript_runtime_t *rt, FILE *f) {
    int count = 0;
    if (!result_wifi_ap_count(rt, &count)) return false;
    if (fprintf(f, "bssid,ssid,channel,rssi,auth\n") <= 0) return false;
    for (int i = 0; i < count; ++i) {
        ghostesp_wifi_ap_info_t ap;
        if (!rt->api || !rt->api->wifi_scan_get_ap || !rt->api->wifi_scan_get_ap((uint16_t)i, &ap)) continue;
        char bssid[18];
        snprintf(bssid, sizeof(bssid), "%02x:%02x:%02x:%02x:%02x:%02x",
                 ap.bssid[0], ap.bssid[1], ap.bssid[2], ap.bssid[3], ap.bssid[4], ap.bssid[5]);
        if (fprintf(f, "%s,\"%s\",%u,%d,%u\n", bssid, ap.ssid, (unsigned)ap.channel, (int)ap.rssi, (unsigned)ap.auth_mode) <= 0) return false;
    }
    return true;
}

static bool result_ble_device_count(ghostscript_runtime_t *rt, int *out) {
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_BLE) || !out) return false;
    *out = rt->api && rt->api->ble_device_count ? rt->api->ble_device_count() : 0;
    return true;
}

static bool result_ble_device_field(ghostscript_runtime_t *rt, int idx, const char *field,
                                    char *str, size_t str_len, lua_Integer *num, bool *is_num) {
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_BLE) || !field) return false;
    ghostesp_ble_device_info_t dev;
    if (!rt->api || !rt->api->ble_get_device || !rt->api->ble_get_device(idx, &dev)) return false;
    if (strcmp(field, "mac") == 0) {
        snprintf(str, str_len, "%02X:%02X:%02X:%02X:%02X:%02X", dev.mac[0], dev.mac[1], dev.mac[2], dev.mac[3], dev.mac[4], dev.mac[5]);
        if (is_num) *is_num = false;
        return true;
    }
    if (strcmp(field, "name") == 0) {
        snprintf(str, str_len, "%s", dev.name);
        if (is_num) *is_num = false;
        return true;
    }
    if (strcmp(field, "rssi") == 0) { if (num) *num = dev.rssi; if (is_num) *is_num = true; return true; }
    return false;
}

static bool result_ble_device_write_csv(ghostscript_runtime_t *rt, FILE *f) {
    int count = 0;
    if (!result_ble_device_count(rt, &count)) return false;
    if (fprintf(f, "mac,name,rssi\n") <= 0) return false;
    for (int i = 0; i < count; ++i) {
        ghostesp_ble_device_info_t dev;
        if (!rt->api || !rt->api->ble_get_device || !rt->api->ble_get_device(i, &dev)) continue;
        char mac[18];
        snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", dev.mac[0], dev.mac[1], dev.mac[2], dev.mac[3], dev.mac[4], dev.mac[5]);
        if (fprintf(f, "%s,\"%s\",%d\n", mac, dev.name, (int)dev.rssi) <= 0) return false;
    }
    return true;
}

static bool result_ble_detect_count(ghostscript_runtime_t *rt, int *out) {
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_BLE) || !out) return false;
    *out = rt->api && rt->api->ble_detect_count ? rt->api->ble_detect_count() : 0;
    return true;
}

static bool result_ble_detect_field(ghostscript_runtime_t *rt, int idx, const char *field,
                                    char *str, size_t str_len, lua_Integer *num, bool *is_num) {
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_BLE) || !field) return false;
    ghostesp_ble_detect_info_t dev;
    if (!rt->api || !rt->api->ble_detect_get_device || !rt->api->ble_detect_get_device(idx, &dev)) return false;
    if (strcmp(field, "mac") == 0) {
        snprintf(str, str_len, "%02X:%02X:%02X:%02X:%02X:%02X", dev.mac[0], dev.mac[1], dev.mac[2], dev.mac[3], dev.mac[4], dev.mac[5]);
        if (is_num) *is_num = false;
        return true;
    }
    if (strcmp(field, "name") == 0) { snprintf(str, str_len, "%s", dev.name); if (is_num) *is_num = false; return true; }
    if (strcmp(field, "subtype") == 0) { snprintf(str, str_len, "%s", dev.subtype); if (is_num) *is_num = false; return true; }
    if (strcmp(field, "type") == 0) { if (num) *num = dev.type; if (is_num) *is_num = true; return true; }
    if (strcmp(field, "rssi") == 0) { if (num) *num = dev.rssi; if (is_num) *is_num = true; return true; }
    if (strcmp(field, "tracking") == 0) { if (num) *num = dev.tracking ? 1 : 0; if (is_num) *is_num = true; return true; }
    return false;
}

static bool result_ble_detect_write_csv(ghostscript_runtime_t *rt, FILE *f) {
    int count = 0;
    if (!result_ble_detect_count(rt, &count)) return false;
    if (fprintf(f, "mac,name,subtype,type,rssi,tracking\n") <= 0) return false;
    for (int i = 0; i < count; ++i) {
        ghostesp_ble_detect_info_t dev;
        if (!rt->api || !rt->api->ble_detect_get_device || !rt->api->ble_detect_get_device(i, &dev)) continue;
        char mac[18];
        snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", dev.mac[0], dev.mac[1], dev.mac[2], dev.mac[3], dev.mac[4], dev.mac[5]);
        if (fprintf(f, "%s,\"%s\",\"%s\",%u,%d,%u\n", mac, dev.name, dev.subtype, (unsigned)dev.type, (int)dev.rssi, dev.tracking ? 1u : 0u) <= 0) return false;
    }
    return true;
}

static void command_log_clear(ghostscript_runtime_t *rt) {
    if (!rt) return;
    rt->command_log_first = 0;
    rt->command_log_count = 0;
    rt->command_log_total = 0;
    memset(rt->command_log, 0, sizeof(rt->command_log));
}

static void command_log_append(ghostscript_runtime_t *rt, const char *line) {
    if (!rt || !line) return;
    uint32_t slot;
    if (rt->command_log_count < GS_COMMAND_LOG_LINES) {
        slot = (rt->command_log_first + rt->command_log_count) % GS_COMMAND_LOG_LINES;
        rt->command_log_count++;
    } else {
        slot = rt->command_log_first;
        rt->command_log_first = (rt->command_log_first + 1) % GS_COMMAND_LOG_LINES;
    }
    snprintf(rt->command_log[slot], sizeof(rt->command_log[slot]), "%s", line);
    size_t n = strlen(rt->command_log[slot]);
    while (n > 0 && (rt->command_log[slot][n - 1] == '\n' || rt->command_log[slot][n - 1] == '\r')) {
        rt->command_log[slot][--n] = '\0';
    }
    rt->command_log_total++;
}

static const char *command_log_line_at(ghostscript_runtime_t *rt, int idx, uint32_t *line_no) {
    if (!rt || idx < 0 || (uint32_t)idx >= rt->command_log_count) return NULL;
    uint32_t slot = (rt->command_log_first + (uint32_t)idx) % GS_COMMAND_LOG_LINES;
    if (line_no) *line_no = rt->command_log_total - rt->command_log_count + (uint32_t)idx + 1;
    return rt->command_log[slot];
}

static bool result_command_log_count(ghostscript_runtime_t *rt, int *out) {
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_COMMANDS) || !out) return false;
    *out = rt ? (int)rt->command_log_count : 0;
    return true;
}

static bool result_command_log_field(ghostscript_runtime_t *rt, int idx, const char *field,
                                     char *str, size_t str_len, lua_Integer *num, bool *is_num) {
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_COMMANDS) || !field) return false;
    uint32_t line_no = 0;
    const char *line = command_log_line_at(rt, idx, &line_no);
    if (!line) return false;
    if (strcmp(field, "line") == 0) { snprintf(str, str_len, "%s", line); if (is_num) *is_num = false; return true; }
    if (strcmp(field, "number") == 0) { if (num) *num = line_no; if (is_num) *is_num = true; return true; }
    return false;
}

static bool result_command_log_write_csv(ghostscript_runtime_t *rt, FILE *f) {
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_COMMANDS)) return false;
    if (fprintf(f, "number,line\n") <= 0) return false;
    for (uint32_t i = 0; i < rt->command_log_count; ++i) {
        uint32_t line_no = 0;
        const char *line = command_log_line_at(rt, (int)i, &line_no);
        if (!line) continue;
        if (fprintf(f, "%lu,\"%s\"\n", (unsigned long)line_no, line) <= 0) return false;
    }
    return true;
}

static bool result_serial_log_count(ghostscript_runtime_t *rt, int *out) {
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_COMMANDS) || !out) return false;
    *out = (int)terminal_view_log_count();
    return true;
}

static bool result_serial_log_field(ghostscript_runtime_t *rt, int idx, const char *field,
                                    char *str, size_t str_len, lua_Integer *num, bool *is_num) {
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_COMMANDS) || !field) return false;
    if (idx < 0) return false;
    if (strcmp(field, "line") == 0) {
        if (!terminal_view_log_get((size_t)idx, str, str_len)) return false;
        if (is_num) *is_num = false;
        return true;
    }
    if (strcmp(field, "number") == 0) { if (num) *num = idx + 1; if (is_num) *is_num = true; return true; }
    return false;
}

static bool result_serial_log_write_csv(ghostscript_runtime_t *rt, FILE *f) {
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_COMMANDS)) return false;
    size_t count = terminal_view_log_count();
    if (fprintf(f, "number,line\n") <= 0) return false;
    char line[GS_COMMAND_LOG_LINE_MAX];
    for (size_t i = 0; i < count; ++i) {
        if (!terminal_view_log_get(i, line, sizeof(line))) continue;
        if (fprintf(f, "%lu,\"%s\"\n", (unsigned long)(i + 1), line) <= 0) return false;
    }
    return true;
}

typedef bool (*result_count_fn_t)(ghostscript_runtime_t *rt, int *out);
typedef bool (*result_field_fn_t)(ghostscript_runtime_t *rt, int idx, const char *field, char *str, size_t str_len, lua_Integer *num, bool *is_num);
typedef bool (*result_csv_fn_t)(ghostscript_runtime_t *rt, FILE *f);

typedef struct {
    const char *kind;
    const char *alias;
    result_count_fn_t count;
    result_field_fn_t field;
    result_csv_fn_t write_csv;
} result_provider_t;

static const result_provider_t s_result_providers[] = {
    { "wifi.ap", "wifi", result_wifi_ap_count, result_wifi_ap_field, result_wifi_ap_write_csv },
    { "ble.device", "ble", result_ble_device_count, result_ble_device_field, result_ble_device_write_csv },
    { "ble.detect", NULL, result_ble_detect_count, result_ble_detect_field, result_ble_detect_write_csv },
    { "command.log", "command", result_command_log_count, result_command_log_field, result_command_log_write_csv },
    { "log.serial", "serial", result_serial_log_count, result_serial_log_field, result_serial_log_write_csv },
    { NULL, NULL, NULL, NULL, NULL },
};

static const result_provider_t *result_provider_for(const char *kind) {
    if (!kind) return NULL;
    for (const result_provider_t *p = s_result_providers; p->kind; ++p) {
        if (strcmp(kind, p->kind) == 0 || (p->alias && strcmp(kind, p->alias) == 0)) return p;
    }
    return NULL;
}

static int l_results_count(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    const char *kind = luaL_checkstring(L, 1);
    const result_provider_t *provider = result_provider_for(kind);
    int count = 0;
    if (!provider || !provider->count || !provider->count(rt, &count)) { lua_pushnil(L); return 1; }
    lua_pushinteger(L, count);
    return 1;
}

static int l_results_field(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    const char *kind = luaL_checkstring(L, 1);
    int idx = (int)luaL_checkinteger(L, 2);
    const char *field = luaL_checkstring(L, 3);
    const result_provider_t *provider = result_provider_for(kind);
    char str[96];
    lua_Integer num = 0;
    bool is_num = false;
    if (!provider || !provider->field || !provider->field(rt, idx, field, str, sizeof(str), &num, &is_num)) { lua_pushnil(L); return 1; }
    if (is_num) lua_pushinteger(L, num);
    else lua_pushstring(L, str);
    return 1;
}

static int l_results_save_csv(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_STORAGE)) return lua_perm_error(L, "storage");
    const char *kind = luaL_checkstring(L, 1);
    const char *rel = luaL_checkstring(L, 2);
    const result_provider_t *provider = result_provider_for(kind);
    if (!provider || !provider->write_csv) { lua_pushboolean(L, false); return 1; }
    char path[GHOSTSCRIPT_PATH_MAX];
    if (!scoped_storage_path(rt, rel, path, sizeof(path))) return luaL_error(L, "invalid scoped path");
    bool display_was_suspended = false;
    if (!ghostscript_manager_sd_begin(&display_was_suspended)) { lua_pushboolean(L, false); return 1; }
    sd_card_create_directory(GHOSTSCRIPT_DATA_DIR);
    sd_card_create_directory(rt->manifest.data_path);
    FILE *f = fopen(path, "wb");
    if (!f) { ghostscript_manager_sd_end(display_was_suspended); lua_pushboolean(L, false); return 1; }
    bool ok = provider->write_csv(rt, f);
    fclose(f);
    ghostscript_manager_sd_end(display_was_suspended);
    lua_pushboolean(L, ok);
    return 1;
}

static int l_wifi_get_channel(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_WIFI)) return lua_perm_error(L, "wifi");
    lua_pushinteger(L, rt->api && rt->api->wifi_get_channel ? rt->api->wifi_get_channel() : 0);
    return 1;
}

static int l_ble_device_count(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_BLE)) return lua_perm_error(L, "ble");
    lua_pushinteger(L, rt->api && rt->api->ble_device_count ? rt->api->ble_device_count() : 0);
    return 1;
}

static int l_ble_get_device(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_BLE)) return lua_perm_error(L, "ble");
    int idx = (int)luaL_checkinteger(L, 1);
    ghostesp_ble_device_info_t dev;
    if (!rt->api || !rt->api->ble_get_device || !rt->api->ble_get_device(idx, &dev)) { lua_pushnil(L); return 1; }
    lua_newtable(L);
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X", dev.mac[0], dev.mac[1], dev.mac[2], dev.mac[3], dev.mac[4], dev.mac[5]);
    lua_pushstring(L, mac_str); lua_setfield(L, -2, "mac");
    lua_pushinteger(L, dev.rssi); lua_setfield(L, -2, "rssi");
    lua_pushstring(L, dev.name); lua_setfield(L, -2, "name");
    return 1;
}

static int l_gps_is_available(lua_State *L) { ghostscript_runtime_t *rt = check_rt(L); return l_bool0(L, PLUGIN_PERMISSION_SYSTEM, rt && rt->api ? rt->api->gps_is_available : NULL, "system"); }
static int l_gps_has_fix(lua_State *L) { ghostscript_runtime_t *rt = check_rt(L); return l_bool0(L, PLUGIN_PERMISSION_SYSTEM, rt && rt->api ? rt->api->gps_has_fix : NULL, "system"); }

static int l_gps_get_latitude(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_SYSTEM)) return lua_perm_error(L, "system");
    lua_pushnumber(L, rt->api && rt->api->gps_get_latitude ? rt->api->gps_get_latitude() : 0.0);
    return 1;
}

static int l_gps_get_longitude(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_SYSTEM)) return lua_perm_error(L, "system");
    lua_pushnumber(L, rt->api && rt->api->gps_get_longitude ? rt->api->gps_get_longitude() : 0.0);
    return 1;
}

static int l_gps_get_altitude(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_SYSTEM)) return lua_perm_error(L, "system");
    lua_pushnumber(L, rt->api && rt->api->gps_get_altitude ? rt->api->gps_get_altitude() : 0.0);
    return 1;
}

static int l_gps_get_satellites(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_SYSTEM)) return lua_perm_error(L, "system");
    lua_pushinteger(L, rt->api && rt->api->gps_get_satellites ? rt->api->gps_get_satellites() : 0);
    return 1;
}

static int l_battery_percent(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_POWER)) return lua_perm_error(L, "power");
    lua_pushinteger(L, rt->api && rt->api->battery_percent ? rt->api->battery_percent() : -1);
    return 1;
}

static int l_battery_voltage(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_POWER)) return lua_perm_error(L, "power");
    lua_pushinteger(L, rt->api && rt->api->battery_voltage_mv ? rt->api->battery_voltage_mv() : 0);
    return 1;
}

static int l_battery_is_charging(lua_State *L) { ghostscript_runtime_t *rt = check_rt(L); return l_bool0(L, PLUGIN_PERMISSION_POWER, rt && rt->api ? rt->api->battery_is_charging : NULL, "power"); }

static int l_brightness_get(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_DISPLAY)) return lua_perm_error(L, "display");
    lua_pushinteger(L, rt->api && rt->api->display_get_brightness ? rt->api->display_get_brightness() : 100);
    return 1;
}

static int l_brightness_set(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_DISPLAY)) return lua_perm_error(L, "display");
    int pct = luaL_checkinteger(L, 1);
    lua_pushboolean(L, rt->api && rt->api->display_set_brightness ? rt->api->display_set_brightness((uint8_t)pct) : false);
    return 1;
}

static int l_random_u32(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_RANDOM)) return lua_perm_error(L, "random");
    lua_pushinteger(L, rt->api && rt->api->random_u32 ? (lua_Integer)rt->api->random_u32() : 0);
    return 1;
}

static int l_system_reboot(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_SYSTEM)) return lua_perm_error(L, "system");
    if (rt->api && rt->api->system_reboot) rt->api->system_reboot();
    return 0;
}

static int l_firmware_version(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    const char *ver = rt && rt->api && rt->api->system_firmware_version ? rt->api->system_firmware_version() : NULL;
    lua_pushstring(L, ver ? ver : "unknown");
    return 1;
}

static int l_system_target(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    const char *tgt = rt && rt->api && rt->api->system_target ? rt->api->system_target() : NULL;
    lua_pushstring(L, tgt ? tgt : "unknown");
    return 1;
}

static int l_time_unix(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_TIME)) return lua_perm_error(L, "time");
    lua_pushinteger(L, rt->api && rt->api->time_unix ? (lua_Integer)rt->api->time_unix() : 0);
    return 1;
}

static int l_time_set(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_TIME)) return lua_perm_error(L, "time");
    int64_t ts = (int64_t)luaL_checkinteger(L, 1);
    lua_pushboolean(L, rt->api && rt->api->time_set_unix ? rt->api->time_set_unix(ts) : false);
    return 1;
}

static int l_screen_width(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    lua_pushinteger(L, rt && rt->api && rt->api->ui_screen_get_width ? rt->api->ui_screen_get_width() : 0);
    return 1;
}

static int l_screen_height(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    lua_pushinteger(L, rt && rt->api && rt->api->ui_screen_get_height ? rt->api->ui_screen_get_height() : 0);
    return 1;
}

static int l_nfc_is_available(lua_State *L) { ghostscript_runtime_t *rt = check_rt(L); return l_bool0(L, PLUGIN_PERMISSION_NFC, rt && rt->api ? rt->api->nfc_is_available : NULL, "nfc"); }
static int l_nfc_read_start(lua_State *L) { ghostscript_runtime_t *rt = check_rt(L); return l_bool0(L, PLUGIN_PERMISSION_NFC, rt && rt->api ? rt->api->nfc_read_start : NULL, "nfc"); }
static int l_nfc_stop(lua_State *L) { ghostscript_runtime_t *rt = check_rt(L); return l_bool0(L, PLUGIN_PERMISSION_NFC, rt && rt->api ? rt->api->nfc_t2_scan_stop : NULL, "nfc"); }

static int l_nfc_scan_start(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    return l_bool0(L, PLUGIN_PERMISSION_NFC, rt && rt->api ? rt->api->nfc_t2_scan_start : NULL, "nfc");
}

static int l_nfc_scan_active(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    return l_bool0(L, PLUGIN_PERMISSION_NFC, rt && rt->api ? rt->api->nfc_t2_scan_active : NULL, "nfc");
}

static int l_nfc_read(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_NFC)) return lua_perm_error(L, "nfc");
    lua_Integer requested = luaL_optinteger(L, 1, GHOSTESP_NFC_T2_NDEF_MAX);
    if (requested < 0) return luaL_error(L, "max_bytes must be non-negative");
    size_t max_bytes = (size_t)requested;
    if (max_bytes > GHOSTESP_NFC_T2_NDEF_MAX) max_bytes = GHOSTESP_NFC_T2_NDEF_MAX;

    ghostesp_nfc_t2_info_t info = {0};
    uint8_t ndef[GHOSTESP_NFC_T2_NDEF_MAX];
    size_t ndef_len = 0;
    if (!rt || !rt->api || !rt->api->nfc_t2_read ||
        !rt->api->nfc_t2_read(&info, ndef, max_bytes, &ndef_len)) {
        lua_pushnil(L);
        return 1;
    }

    char uid_hex[sizeof(info.uid) * 2 + 1];
    size_t uid_pos = 0;
    for (uint8_t i = 0; i < info.uid_len && i < sizeof(info.uid); i++) {
        uid_pos += (size_t)snprintf(uid_hex + uid_pos, sizeof(uid_hex) - uid_pos, "%02X", info.uid[i]);
    }
    lua_newtable(L);
    lua_pushstring(L, uid_hex); lua_setfield(L, -2, "uid");
    lua_pushstring(L, info.model); lua_setfield(L, -2, "model");
    lua_pushinteger(L, info.user_bytes); lua_setfield(L, -2, "user_bytes");
    lua_pushinteger(L, info.ndef_length); lua_setfield(L, -2, "ndef_length");
    lua_pushboolean(L, info.ndef_present); lua_setfield(L, -2, "ndef_present");
    lua_pushboolean(L, info.read_only); lua_setfield(L, -2, "read_only");
    lua_pushboolean(L, info.password_protected); lua_setfield(L, -2, "password_protected");
    lua_pushboolean(L, info.static_locked); lua_setfield(L, -2, "static_locked");
    lua_pushboolean(L, info.dynamic_locked); lua_setfield(L, -2, "dynamic_locked");
    if (info.ndef_present) {
        lua_pushlstring(L, (const char *)ndef, ndef_len);
        lua_setfield(L, -2, "ndef");
    }
    return 1;
}

static int l_nfc_write_ndef(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_NFC)) return lua_perm_error(L, "nfc");
    size_t len = 0;
    const char *message = luaL_checklstring(L, 1, &len);
    bool ok = rt && rt->api && rt->api->nfc_t2_write_ndef &&
              rt->api->nfc_t2_write_ndef((const uint8_t *)message, len);
    lua_pushboolean(L, ok);
    return 1;
}

static int l_nfc_last_tag(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_NFC)) return lua_perm_error(L, "nfc");
    const char *last = rt ? rt->last_nfc_tag : NULL;
    if (!last || !last[0]) { lua_pushnil(L); return 1; }
    lua_newtable(L);
    lua_pushstring(L, last);
    lua_setfield(L, -2, "raw");
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", last);
    char *sep = strchr(buf, '|');
    if (sep) {
        *sep = '\0';
        lua_pushstring(L, buf);
        lua_setfield(L, -2, "type");
        lua_pushstring(L, sep + 1);
        lua_setfield(L, -2, "uid");
    }
    return 1;
}

static int l_ir_stop(lua_State *L) { ghostscript_runtime_t *rt = check_rt(L); return l_bool0(L, PLUGIN_PERMISSION_IR, rt && rt->api ? rt->api->ir_stop : NULL, "ir"); }
static int l_subghz_stop(lua_State *L) { ghostscript_runtime_t *rt = check_rt(L); return l_bool0(L, PLUGIN_PERMISSION_SUBGHZ, rt && rt->api ? rt->api->subghz_stop : NULL, "subghz"); }
static int l_badusb_stop(lua_State *L) { ghostscript_runtime_t *rt = check_rt(L); return l_bool0(L, PLUGIN_PERMISSION_BADUSB, rt && rt->api ? rt->api->badusb_stop : NULL, "badusb"); }

static int l_settings_get_u8(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_SETTINGS)) return lua_perm_error(L, "settings");
    const char *key = luaL_checkstring(L, 1);
    uint8_t val = 0;
    bool ok = rt->api && rt->api->settings_get_u8 && rt->api->settings_get_u8(key, &val);
    if (!ok) { lua_pushnil(L); return 1; }
    lua_pushinteger(L, val);
    return 1;
}

static int l_settings_set_u8(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_SETTINGS)) return lua_perm_error(L, "settings");
    const char *key = luaL_checkstring(L, 1);
    int val = luaL_checkinteger(L, 2);
    lua_pushboolean(L, rt->api && rt->api->settings_set_u8 ? rt->api->settings_set_u8(key, (uint8_t)val) : false);
    return 1;
}

static int l_settings_get_string(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_SETTINGS)) return lua_perm_error(L, "settings");
    const char *key = luaL_checkstring(L, 1);
    char buf[128] = {0};
    bool ok = rt->api && rt->api->settings_get_string && rt->api->settings_get_string(key, buf, sizeof(buf));
    if (!ok) { lua_pushnil(L); return 1; }
    lua_pushstring(L, buf);
    return 1;
}

static int l_settings_set_string(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_SETTINGS)) return lua_perm_error(L, "settings");
    const char *key = luaL_checkstring(L, 1);
    const char *val = luaL_checkstring(L, 2);
    lua_pushboolean(L, rt->api && rt->api->settings_set_string ? rt->api->settings_set_string(key, val) : false);
    return 1;
}

static int l_settings_save(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_SETTINGS)) return lua_perm_error(L, "settings");
    lua_pushboolean(L, rt->api && rt->api->settings_save ? rt->api->settings_save() : false);
    return 1;
}

static int l_nvs_get_u32(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_SETTINGS)) return lua_perm_error(L, "settings");
    const char *key = luaL_checkstring(L, 1);
    uint32_t val = 0;
    bool ok = rt->api && rt->api->nvs_get_u32 && rt->api->nvs_get_u32(key, &val);
    if (!ok) { lua_pushnil(L); return 1; }
    lua_pushinteger(L, (lua_Integer)val);
    return 1;
}

static int l_nvs_set_u32(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_SETTINGS)) return lua_perm_error(L, "settings");
    const char *key = luaL_checkstring(L, 1);
    uint32_t val = (uint32_t)luaL_checkinteger(L, 2);
    lua_pushboolean(L, rt->api && rt->api->nvs_set_u32 ? rt->api->nvs_set_u32(key, val) : false);
    return 1;
}

static int l_nvs_delete(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_SETTINGS)) return lua_perm_error(L, "settings");
    const char *key = luaL_checkstring(L, 1);
    lua_pushboolean(L, rt->api && rt->api->nvs_delete ? rt->api->nvs_delete(key) : false);
    return 1;
}

static int l_command_exec(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_COMMANDS)) return lua_perm_error(L, "commands");
    const char *cmd = luaL_checkstring(L, 1);
    lua_pushboolean(L, rt->api && rt->api->command_exec ? rt->api->command_exec(cmd) : false);
    return 1;
}

static int l_parser_summary(lua_State *L, bool (*fn)(const char *, char *, size_t), uint32_t perm, const char *name) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, perm)) return lua_perm_error(L, name);
    const char *path = luaL_checkstring(L, 1);
    char summary[512];
    bool ok = fn ? fn(path, summary, sizeof(summary)) : false;
    if (!ok) { lua_pushnil(L); return 1; }
    lua_pushstring(L, summary);
    return 1;
}

static int l_parser_nfc_summary(lua_State *L) { ghostscript_runtime_t *rt = check_rt(L); return l_parser_summary(L, rt && rt->api ? rt->api->parser_nfc_summary : NULL, PLUGIN_PERMISSION_NFC, "nfc"); }
static int l_parser_ir_summary(lua_State *L) { ghostscript_runtime_t *rt = check_rt(L); return l_parser_summary(L, rt && rt->api ? rt->api->parser_ir_summary : NULL, PLUGIN_PERMISSION_IR, "ir"); }
static int l_parser_subghz_summary(lua_State *L) { ghostscript_runtime_t *rt = check_rt(L); return l_parser_summary(L, rt && rt->api ? rt->api->parser_subghz_summary : NULL, PLUGIN_PERMISSION_SUBGHZ, "subghz"); }

void ghostscript_runtime_set_active(ghostscript_runtime_t *rt) {
    taskENTER_CRITICAL(&s_active_runtime_mux);
    s_active_runtime = rt;
    taskEXIT_CRITICAL(&s_active_runtime_mux);
}

ghostscript_runtime_t *ghostscript_runtime_get_active(void) {
    taskENTER_CRITICAL(&s_active_runtime_mux);
    ghostscript_runtime_t *rt = s_active_runtime;
    taskEXIT_CRITICAL(&s_active_runtime_mux);
    return rt;
}

static bool queue_event(ghostscript_runtime_t *rt, gs_queued_event_t *event) {
    if (!rt || !event) return false;

    taskENTER_CRITICAL(&s_active_runtime_mux);
    QueueHandle_t queue = (rt == s_active_runtime) ? s_event_queue : NULL;
    if (queue) s_event_senders++;
    taskEXIT_CRITICAL(&s_active_runtime_mux);
    if (!queue) {
        heap_caps_free(event);
        return false;
    }

    bool queued = event && xQueueSend(queue, &event, 0) == pdPASS;
    if (!queued) heap_caps_free(event);
    taskENTER_CRITICAL(&s_active_runtime_mux);
    s_event_senders--;
    taskEXIT_CRITICAL(&s_active_runtime_mux);
    return queued;
}

static bool queue_text_event(ghostscript_runtime_t *rt, const char *name, const char *value) {
    if (!name) return false;
    size_t name_len = strnlen(name, GS_EVENT_NAME_MAX - 1);
    size_t value_len = strnlen(value ? value : "", GS_EVENT_VALUE_MAX - 1);
    gs_queued_text_event_t *event = gs_heap_alloc(sizeof(*event) + name_len + value_len + 2);
    if (!event) return false;
    event->kind = GS_QUEUED_EVENT;
    memcpy(event->text, name, name_len);
    event->text[name_len] = '\0';
    memcpy(event->text + name_len + 1, value ? value : "", value_len);
    event->text[name_len + value_len + 1] = '\0';
    return queue_event(rt, (gs_queued_event_t *)event);
}

static void runtime_capture_line(const char *line, void *user) {
    (void)user;
    ghostscript_runtime_t *rt = ghostscript_runtime_get_active();
    if (!rt || !line) return;
    char buf[GS_COMMAND_LOG_LINE_MAX];
    size_t n = strnlen(line, sizeof(buf) - 1);
    memcpy(buf, line, n);
    buf[n] = '\0';
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) { buf[--n] = '\0'; }
    command_log_append(rt, buf);
    (void)queue_text_event(rt, "command.output", buf);
}

static int l_command_start(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt_has_perm(rt, PLUGIN_PERMISSION_COMMANDS)) return lua_perm_error(L, "commands");
    const char *cmd = luaL_checkstring(L, 1);
    command_log_clear(rt);
    glog_set_capture(runtime_capture_line, NULL);
    bool ok = rt->api && rt->api->command_exec ? rt->api->command_exec(cmd) : false;
    glog_set_capture(NULL, NULL);
    lua_pushboolean(L, ok);
    return 1;
}

static int l_oui_lookup(lua_State *L) {
    const char *mac = luaL_checkstring(L, 1);
    char vendor[64];
    if (ouis_lookup_vendor(mac, vendor, sizeof(vendor))) {
        lua_pushstring(L, vendor);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

static bool mac_starts_with_prefix(const char *mac, const char *prefix) {
    if (!mac || !prefix) return false;
    int mac_i = 0, pre_i = 0;
    int pre_target = 0;
    while (prefix[pre_i]) {
        char c = prefix[pre_i++];
        if (c == ':' || c == '-' || c == ' ') continue;
        pre_target++;
    }
    pre_i = 0;
    int pre_consumed = 0;
    while (mac[mac_i] && pre_consumed < pre_target) {
        char c = mac[mac_i++];
        if (c == ':' || c == '-' || c == ' ') continue;
        char p = prefix[pre_i++];
        if (p == ':' || p == '-' || p == ' ') { pre_i--; continue; }
        if (tolower((unsigned char)c) != tolower((unsigned char)p)) return false;
        pre_consumed++;
    }
    return pre_consumed == pre_target;
}

static int l_oui_prefix_match(lua_State *L) {
    const char *mac = luaL_checkstring(L, 1);
    const char *prefix = luaL_checkstring(L, 2);
    lua_pushboolean(L, mac_starts_with_prefix(mac, prefix));
    return 1;
}

static int l_oui_prefix_set(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt) return luaL_error(L, "runtime missing");
    for (int i = 0; i < rt->oui_prefix_count; ++i) {
        free(rt->oui_prefix[i]);
        rt->oui_prefix[i] = NULL;
    }
    rt->oui_prefix_count = 0;
    int n = lua_gettop(L);
    for (int i = 1; i <= n && rt->oui_prefix_count < 4; ++i) {
        if (lua_isstring(L, i)) {
            const char *p = lua_tostring(L, i);
            if (p && p[0]) {
                rt->oui_prefix[rt->oui_prefix_count++] = strdup(p);
            }
        }
    }
    lua_pushinteger(L, rt->oui_prefix_count);
    return 1;
}

bool ghostscript_runtime_match_oui_prefix(const ghostscript_runtime_t *rt, const char *mac) {
    if (!rt || !mac) return false;
    for (int i = 0; i < rt->oui_prefix_count; ++i) {
        if (mac_starts_with_prefix(mac, rt->oui_prefix[i])) return true;
    }
    return false;
}

static int l_wifi_on_ap(lua_State *L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushstring(L, "wifi_ap_found");
    lua_insert(L, 1);
    return l_event_on(L);
}

static int l_ble_on_device(lua_State *L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushstring(L, "ble_device");
    lua_insert(L, 1);
    return l_event_on(L);
}

static int l_gps_on_fix(lua_State *L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushstring(L, "gps_fix");
    lua_insert(L, 1);
    return l_event_on(L);
}

static int l_capabilities_has_permission(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    const char *name = luaL_checkstring(L, 1);
    bool allowed = rt && rt->api && rt->api->has_permission && rt->api->has_permission(name);
    lua_pushboolean(L, allowed);
    return 1;
}

static int l_capabilities_has_feature(lua_State *L) {
    ghostscript_runtime_t *rt = check_rt(L);
    const char *name = luaL_checkstring(L, 1);
    bool available = strcmp(name, "storage_stream") == 0 ||
                     (rt && rt->api && rt->api->has_feature && rt->api->has_feature(name));
    lua_pushboolean(L, available);
    return 1;
}

static void add_funcs(lua_State *L, const luaL_Reg *funcs) {
    for (const luaL_Reg *r = funcs; r && r->name; ++r) {
        lua_pushcfunction(L, r->func);
        lua_setfield(L, -2, r->name);
    }
}

typedef struct {
    const char *name;
    const luaL_Reg *funcs;
} gs_lazy_sub_t;

static const gs_lazy_sub_t s_lazy_subs[] = {
    {"ui",       (const luaL_Reg[]){{"toast", l_toast}, {"set_title", l_set_title}, {"screen_width", l_screen_width}, {"screen_height", l_screen_height}, {NULL, NULL}}},
    {"event",    (const luaL_Reg[]){{"on", l_event_on}, {"off", l_event_off}, {"emit", l_event_emit}, {"wait", l_event_wait}, {NULL, NULL}}},
    {"input",    (const luaL_Reg[]){{"subscribe", l_event_on}, {"unsubscribe", l_event_off}, {NULL, NULL}}},
    {"system",   (const luaL_Reg[]){{"free_heap", l_free_heap}, {"free_internal_heap", l_free_internal_heap}, {"uptime_ms", l_uptime_ms}, {"memory_used", l_memory_used}, {"memory_limit", l_memory_limit}, {"firmware_version", l_firmware_version}, {"target", l_system_target}, {"reboot", l_system_reboot}, {"random", l_random_u32}, {NULL, NULL}}},
    {"storage",  (const luaL_Reg[]){{"read", l_storage_read}, {"write", l_storage_write}, {"exists", l_storage_exists}, {"append", l_storage_append}, {"delete", l_storage_delete}, {"mkdir", l_storage_mkdir}, {"list", l_storage_list}, {"stat", l_storage_stat}, {"rename", l_storage_rename}, {"open", l_storage_open}, {"stream_read", l_storage_stream_read}, {"stream_write", l_storage_stream_write}, {"close", l_storage_close}, {NULL, NULL}}},
    {"capabilities", (const luaL_Reg[]){{"has_permission", l_capabilities_has_permission}, {"has_feature", l_capabilities_has_feature}, {NULL, NULL}}},
    {"wifi",     (const luaL_Reg[]){{"scan_start", l_wifi_scan_start}, {"scan_done", l_wifi_scan_done}, {"scan_finish", l_wifi_scan_finish}, {"scan_stop", l_wifi_stop_scan}, {"ap_count", l_wifi_count}, {"ap", l_wifi_ap}, {"connect", l_wifi_connect}, {"disconnect", l_wifi_disconnect}, {"is_connected", l_wifi_is_connected}, {"rssi", l_wifi_rssi}, {"ip", l_wifi_ip}, {"set_channel", l_wifi_set_channel}, {"get_channel", l_wifi_get_channel}, {"on_ap", l_wifi_on_ap}, {"deauth", l_wifi_deauth}, {"station_scan_start", l_wifi_station_scan_start}, {"station_scan_stop", l_wifi_station_scan_stop}, {"station_count", l_wifi_station_count}, {"station", l_wifi_station}, {NULL, NULL}}},
    {"ble",      (const luaL_Reg[]){{"scan_start", l_ble_start}, {"scan_stop", l_ble_stop}, {"device_count", l_ble_device_count}, {"get_device", l_ble_get_device}, {"on_device", l_ble_on_device}, {NULL, NULL}}},
    {"gps",      (const luaL_Reg[]){{"is_available", l_gps_is_available}, {"has_fix", l_gps_has_fix}, {"latitude", l_gps_get_latitude}, {"longitude", l_gps_get_longitude}, {"altitude", l_gps_get_altitude}, {"satellites", l_gps_get_satellites}, {"on_fix", l_gps_on_fix}, {NULL, NULL}}},
    {"oui",      (const luaL_Reg[]){{"lookup", l_oui_lookup}, {"prefix_match", l_oui_prefix_match}, {"prefix_set", l_oui_prefix_set}, {NULL, NULL}}},
    {"power",    (const luaL_Reg[]){{"percent", l_battery_percent}, {"voltage_mv", l_battery_voltage}, {"is_charging", l_battery_is_charging}, {"get_brightness", l_brightness_get}, {"set_brightness", l_brightness_set}, {NULL, NULL}}},
    {"nfc",      (const luaL_Reg[]){{"is_available", l_nfc_is_available}, {"read_start", l_nfc_read_start}, {"scan_start", l_nfc_scan_start}, {"stop", l_nfc_stop}, {"scan_active", l_nfc_scan_active}, {"read", l_nfc_read}, {"write_ndef", l_nfc_write_ndef}, {"last_tag", l_nfc_last_tag}, {NULL, NULL}}},
    {"time",     (const luaL_Reg[]){{"unix", l_time_unix}, {"set_unix", l_time_set}, {NULL, NULL}}},
    {"rgb",      (const luaL_Reg[]){{"set", l_rgb_set}, {NULL, NULL}}},
    {"badusb",   (const luaL_Reg[]){{"run", l_badusb_run}, {"stop", l_badusb_stop}, {NULL, NULL}}},
    {"ir",       (const luaL_Reg[]){{"send_file", l_ir_send}, {"listen", l_ir_listen}, {"stop", l_ir_stop}, {NULL, NULL}}},
    {"subghz",   (const luaL_Reg[]){{"load", l_subghz_load}, {"transmit", l_subghz_tx}, {"receive", l_subghz_receive}, {"read_raw", l_subghz_read_raw}, {"stop", l_subghz_stop}, {NULL, NULL}}},
    {"net",      (const luaL_Reg[]){{"http_get", l_http_get}, {"http_post", l_http_post}, {NULL, NULL}}},
    {"settings", (const luaL_Reg[]){{"get_u8", l_settings_get_u8}, {"set_u8", l_settings_set_u8}, {"get_string", l_settings_get_string}, {"set_string", l_settings_set_string}, {"save", l_settings_save}, {"nvs_get_u32", l_nvs_get_u32}, {"nvs_set_u32", l_nvs_set_u32}, {"nvs_delete", l_nvs_delete}, {NULL, NULL}}},
    {"commands", (const luaL_Reg[]){{"exec", l_command_exec}, {"start", l_command_start}, {NULL, NULL}}},
    {"parser",   (const luaL_Reg[]){{"nfc_summary", l_parser_nfc_summary}, {"ir_summary", l_parser_ir_summary}, {"subghz_summary", l_parser_subghz_summary}, {NULL, NULL}}},
    {"results",  (const luaL_Reg[]){{"count", l_results_count}, {"field", l_results_field}, {"save_csv", l_results_save_csv}, {NULL, NULL}}},
    {NULL, NULL},
};

static int l_ghost_index(lua_State *L) {
    const char *key = lua_tostring(L, 2);
    if (!key) { lua_pushnil(L); return 1; }
    /* Check cache without invoking this __index metamethod again. */
    lua_getfield(L, LUA_REGISTRYINDEX, "ghostscript.cache");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, key);
        if (!lua_isnil(L, -1)) return 1;
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    /* look up subtable definition */
    for (const gs_lazy_sub_t *s = s_lazy_subs; s->name; ++s) {
        if (strcmp(s->name, key) == 0) {
            lua_newtable(L);
            add_funcs(L, s->funcs);
            lua_getfield(L, LUA_REGISTRYINDEX, "ghostscript.cache");
            if (lua_isnil(L, -1)) {
                lua_pop(L, 1);
                lua_newtable(L);
                lua_pushvalue(L, -1);
                lua_setfield(L, LUA_REGISTRYINDEX, "ghostscript.cache");
            }
            lua_pushvalue(L, -2);
            lua_setfield(L, -2, key);
            lua_pop(L, 1);
            return 1;
        }
    }
    lua_pushnil(L);
    return 1;
}

static void register_api(lua_State *L, ghostscript_runtime_t *rt) {
    lua_pushlightuserdata(L, rt);
    lua_setfield(L, LUA_REGISTRYINDEX, "ghostscript.rt");
    lua_newtable(L);
    const luaL_Reg root[] = { {"print", l_print}, {"delay", l_delay}, {"exit", l_exit}, {NULL, NULL} };
    add_funcs(L, root);
    /* set metatable with __index for lazy subtable creation */
    lua_newtable(L);
    lua_pushcfunction(L, l_ghost_index);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    lua_setglobal(L, "ghost");
    lua_pushcfunction(L, l_print);
    lua_setglobal(L, "print");

    luaL_newmetatable(L, GS_STORAGE_STREAM_MT);
    lua_pushcfunction(L, l_storage_stream_gc);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);
}

static void hook(lua_State *L, lua_Debug *ar) {
    (void)ar;
    ghostscript_runtime_t *rt = check_rt(L);
    if (!rt) luaL_error(L, "runtime missing");
    if (rt->stop_requested) luaL_error(L, "script stopped");
    if (rt->manifest.timeout_ms > 0) {
        if (rt->slice_start_us <= 0) rt->slice_start_us = esp_timer_get_time();
        int64_t elapsed = (esp_timer_get_time() - rt->slice_start_us) / 1000;
        if (elapsed > rt->manifest.timeout_ms) luaL_error(L, "script timeout");
    }
}

static int runtime_hook_count(const ghostscript_runtime_t *rt) {
    return (int)(rt && rt->manifest.instruction_budget ? rt->manifest.instruction_budget : 10000);
}

static void runtime_begin_slice(ghostscript_runtime_t *rt) {
    if (rt) rt->slice_start_us = esp_timer_get_time();
}

typedef struct {
    FILE *file;
    char *buf; /* sector-sized, DMA-capable, cache-aligned read buffer */
    void *dma_reserve;
    bool io_failed;
} gs_reader_t;

static bool script_reader_reserve_dma(gs_reader_t *reader) {
    reader->dma_reserve = heap_caps_aligned_alloc(32, GS_SD_DMA_RESERVE,
                                                  MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return reader->dma_reserve != NULL;
}

static const char *script_reader(lua_State *L, void *data, size_t *size) {
    (void)L;
    gs_reader_t *reader = (gs_reader_t *)data;
    if (!reader || !reader->file || !reader->buf || reader->io_failed) {
        *size = 0;
        return NULL;
    }
    /* Full-sector script data reads land directly in this DMA buffer. FatFs
     * metadata is not DMA-aligned, though, so ESP-IDF needs a temporary DMA
     * buffer for those reads. Release our reservation immediately before the
     * read, then recreate it to keep the next transaction fragmentation-safe. */
    heap_caps_free(reader->dma_reserve);
    reader->dma_reserve = NULL;
    size_t n = fread(reader->buf, 1, GS_LOAD_CHUNK, reader->file);
    if (!script_reader_reserve_dma(reader)) reader->io_failed = true;
    if (ferror(reader->file)) reader->io_failed = true;
    *size = n;
    return n > 0 ? reader->buf : NULL;
}

static FILE *open_script_for_streaming(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz <= 0 || (size_t)sz > GHOSTSCRIPT_FILE_MAX_BYTES) { fclose(f); return NULL; }
    rewind(f);
    return f;
}

ghostscript_runtime_t *ghostscript_runtime_create(const ghostscript_manifest_t *manifest, const ghostscript_runtime_hooks_t *hooks) {
    if (!manifest || !manifest->valid) return NULL;
    ghostscript_runtime_t *rt = heap_caps_calloc_prefer(1, sizeof(*rt), 2,
                                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!rt) return NULL;
    rt->manifest = *manifest;
    if (hooks) rt->hooks = *hooks;
    rt->memory_limit = manifest->memory_limit;
    rt->thread_ref = LUA_NOREF;
    rt->state = GHOSTSCRIPT_STATE_LOADED;
    return rt;
}

#if 0 /* FreeRTOS Lua tasks cannot safely share script closures or runtime state. */
static int bg_task_thread_fn(void *arg) {
    bg_task_t *t = (bg_task_t *)arg;
    /* The bg runtime + Lua state were pre-created by l_tasks_spawn so the
     * function could be moved across. */
    if (!t->L) {
        t->done = true;
        vTaskDelete(NULL);
        return 0;
    }
    int rc = lua_pcall(t->L, 0, 0, 0);
    if (rc != LUA_OK) {
        glog("bg task %s error: %s", t->name, lua_tostring(t->L, -1));
    }
    ghostscript_runtime_t *bg_rt = (ghostscript_runtime_t *)lua_touserdata(t->L, lua_upvalueindex(0));
    /* Better: pull bg_rt from registry. */
    lua_getfield(t->L, LUA_REGISTRYINDEX, "ghostscript.bg_rt");
    bg_rt = (ghostscript_runtime_t *)lua_touserdata(t->L, -1);
    lua_pop(t->L, 1);
    ghostscript_runtime_set_active(NULL);
    if (bg_rt) {
        lua_close(bg_rt->L);
        free(bg_rt);
    }
    t->done = true;
    vTaskDelete(NULL);
    return 0;
}

static int next_task_id(void) {
    static int s_id = 1;
    return s_id++;
}

static int l_tasks_spawn(lua_State *L) {
    ghostscript_runtime_t *parent_rt = check_rt(L);
    if (!parent_rt) return luaL_error(L, "runtime missing");
    const char *name = luaL_optstring(L, 1, "bg");
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (lua_iscfunction(L, 2)) return luaL_error(L, "ghost.tasks.spawn needs a Lua function");
    size_t stack = 8192;
    if (!lua_isnoneornil(L, 3) && lua_istable(L, 3)) {
        lua_getfield(L, 3, "stack");
        if (lua_isnumber(L, -1)) stack = (size_t)lua_tointeger(L, -1);
        lua_pop(L, 1);
    }
    if (stack < 2048) stack = 2048;
    if (stack > 32768) stack = 32768;

    bg_task_t *t = calloc(1, sizeof(*t));
    if (!t) return luaL_error(L, "out of memory");
    t->id = next_task_id();
    snprintf(t->name, sizeof(t->name), "%.24s", name);

    /* Build the bg runtime and Lua state up front so we can copy the
     * function into it. */
    ghostscript_manifest_t bg_manifest = {0};
    snprintf(bg_manifest.id, sizeof(bg_manifest.id), "bg:%.24s", t->name);
    snprintf(bg_manifest.name, sizeof(bg_manifest.name), "bg:%s", t->name);
    bg_manifest.memory_limit = 16 * 1024;
    bg_manifest.instruction_budget = 50000;
    bg_manifest.valid = true;
    ghostscript_runtime_t *bg_rt = ghostscript_runtime_create(&bg_manifest, NULL);
    if (!bg_rt) { free(t); return luaL_error(L, "out of memory"); }
    bg_rt->L = lua_newstate(gs_alloc, bg_rt);
    if (!bg_rt->L) { free(bg_rt); free(t); return luaL_error(L, "out of memory"); }
    /* Open a safe subset of stdlib. */
    luaL_requiref(bg_rt->L, LUA_GNAME, luaopen_base, 1); lua_pop(bg_rt->L, 1);
    lua_pushnil(bg_rt->L); lua_setglobal(bg_rt->L, "dofile");
    lua_pushnil(bg_rt->L); lua_setglobal(bg_rt->L, "loadfile");
    lua_pushnil(bg_rt->L); lua_setglobal(bg_rt->L, "load");
    luaL_requiref(bg_rt->L, "string", luaopen_string, 1); lua_pop(bg_rt->L, 1);
    luaL_requiref(bg_rt->L, "table", luaopen_table, 1); lua_pop(bg_rt->L, 1);
    /* coroutine library is not linked; lua_yield/lua_resume still work. */
    /* Minimal ghost.* for the bg task. */
    lua_pushcfunction(bg_rt->L, l_print); lua_setglobal(bg_rt->L, "print");
    /* Build ghost = { event = {...}, system = {...} } and ghost.delay. */
    lua_newtable(bg_rt->L);  /* ghost */
    lua_newtable(bg_rt->L);  /* ghost.event */
    lua_pushcfunction(bg_rt->L, l_event_on);   lua_setfield(bg_rt->L, -2, "on");
    lua_pushcfunction(bg_rt->L, l_event_off);  lua_setfield(bg_rt->L, -2, "off");
    lua_pushcfunction(bg_rt->L, l_event_emit); lua_setfield(bg_rt->L, -2, "emit");
    lua_pushcfunction(bg_rt->L, l_event_wait); lua_setfield(bg_rt->L, -2, "wait");
    lua_setfield(bg_rt->L, -2, "event");
    lua_pushcfunction(bg_rt->L, l_free_heap);          lua_setfield(bg_rt->L, -2, "free_heap");
    lua_pushcfunction(bg_rt->L, l_free_internal_heap); lua_setfield(bg_rt->L, -2, "free_internal_heap");
    lua_pushcfunction(bg_rt->L, l_uptime_ms);          lua_setfield(bg_rt->L, -2, "uptime_ms");
    lua_pushcfunction(bg_rt->L, l_random_u32);         lua_setfield(bg_rt->L, -2, "random");
    /* ghost.system is the ghost table itself for backwards compat. */
    lua_setglobal(bg_rt->L, "ghost");
    /* ghost.delay as a function on the ghost table. */
    lua_getglobal(bg_rt->L, "ghost");
    lua_pushcfunction(bg_rt->L, l_delay);
    lua_setfield(bg_rt->L, -2, "delay");
    lua_pop(bg_rt->L, 1);
    /* Install rt pointer so event helpers find the runtime. */
    lua_pushlightuserdata(bg_rt->L, bg_rt);
    lua_setfield(bg_rt->L, LUA_REGISTRYINDEX, "ghostscript.rt");
    lua_pushlightuserdata(bg_rt->L, bg_rt);
    lua_setfield(bg_rt->L, LUA_REGISTRYINDEX, "ghostscript.bg_rt");
    /* Move the user's function from parent stack to bg stack. */
    lua_pushvalue(L, 2);
    lua_xmove(L, bg_rt->L, 1);
    t->L = bg_rt->L;

    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(
        bg_task_thread_fn, t->name, stack, t, 4, NULL, 0,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        lua_close(bg_rt->L);
        free(bg_rt);
        free(t);
        return luaL_error(L, "failed to spawn task");
    }
    t->next = s_bg_tasks;
    s_bg_tasks = t;
    lua_pushinteger(L, t->id);
    return 1;
}

static int l_tasks_list(lua_State *L) {
    lua_newtable(L);
    int idx = 0;
    for (bg_task_t *t = s_bg_tasks; t; t = t->next) {
        if (t->done) continue;
        idx++;
        lua_newtable(L);
        lua_pushinteger(L, t->id); lua_setfield(L, -2, "id");
        lua_pushstring(L, t->name); lua_setfield(L, -2, "name");
        lua_pushboolean(L, false); lua_setfield(L, -2, "done");
        lua_rawseti(L, -2, idx);
    }
    return 1;
}
#endif

static int l_load_chunk(lua_State *L) {
    gs_reader_t *reader = (gs_reader_t *)lua_touserdata(L, 1);
    ghostscript_runtime_t *rt = check_rt(L);
    const char *path = rt ? rt->manifest.entry_path : "script";
    int err = lua_load(L, script_reader, reader, path, "b");
    if (reader && reader->io_failed) return luaL_error(L, "SD read could not reserve DMA memory");
    if (err) return lua_error(L);
    return 1;
}

static int l_math_abs(lua_State *L) {
    if (lua_isinteger(L, 1)) {
        lua_Integer n = lua_tointeger(L, 1);
        if (n < 0) n = (lua_Integer)(0u - (lua_Unsigned)n);
        lua_pushinteger(L, n);
    } else {
        lua_Number v = luaL_checknumber(L, 1);
        lua_pushnumber(L, v < 0 ? -v : v);
    }
    return 1;
}

static int l_math_floor(lua_State *L) {
    if (lua_isinteger(L, 1)) { lua_settop(L, 1); return 1; }
    lua_Number d = luaL_checknumber(L, 1);
    lua_pushinteger(L, (lua_Integer)d);
    return 1;
}

static int l_math_ceil(lua_State *L) {
    if (lua_isinteger(L, 1)) { lua_settop(L, 1); return 1; }
    lua_Number d = luaL_checknumber(L, 1);
    lua_Integer i = (lua_Integer)d;
    if (d > i) i++;
    lua_pushinteger(L, i);
    return 1;
}

static int l_math_max(lua_State *L) {
    int n = lua_gettop(L);
    luaL_argcheck(L, n >= 1, 1, "value expected");
    int imax = 1;
    for (int i = 2; i <= n; i++)
        if (lua_compare(L, imax, i, LUA_OPLT)) imax = i;
    lua_pushvalue(L, imax);
    return 1;
}

static int l_math_min(lua_State *L) {
    int n = lua_gettop(L);
    luaL_argcheck(L, n >= 1, 1, "value expected");
    int imin = 1;
    for (int i = 2; i <= n; i++)
        if (lua_compare(L, i, imin, LUA_OPLT)) imin = i;
    lua_pushvalue(L, imin);
    return 1;
}

static uint32_t xorshift32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static int l_math_random(lua_State *L) {
    uint32_t *state = (uint32_t *)lua_touserdata(L, lua_upvalueindex(1));
    switch (lua_gettop(L)) {
        case 0:
            lua_pushnumber(L, (lua_Number)xorshift32(state) / (lua_Number)0xFFFFFFFFu);
            return 1;
        case 1: {
            lua_Integer up = luaL_checkinteger(L, 1);
            if (up == 0) { lua_pushinteger(L, (lua_Integer)xorshift32(state)); return 1; }
            luaL_argcheck(L, up >= 1, 1, "interval is empty");
            lua_pushinteger(L, (lua_Integer)(xorshift32(state) % (lua_Unsigned)up) + 1);
            return 1;
        }
        case 2: {
            lua_Integer low = luaL_checkinteger(L, 1);
            lua_Integer up = luaL_checkinteger(L, 2);
            luaL_argcheck(L, low <= up, 1, "interval is empty");
            lua_Unsigned range = (lua_Unsigned)up - (lua_Unsigned)low;
            lua_pushinteger(L, (lua_Integer)(xorshift32(state) % (range + 1)) + low);
            return 1;
        }
        default: return luaL_error(L, "wrong number of arguments");
    }
}

static int l_math_randomseed(lua_State *L) {
    uint32_t *state = (uint32_t *)lua_touserdata(L, lua_upvalueindex(1));
    if (lua_isnone(L, 1)) {
        *state = (uint32_t)esp_timer_get_time();
    } else {
        *state = (uint32_t)luaL_checkinteger(L, 1);
    }
    xorshift32(state);
    return 0;
}

static void open_math_lite(lua_State *L) {
    static const luaL_Reg math_funcs[] = {
        {"abs", l_math_abs}, {"floor", l_math_floor}, {"ceil", l_math_ceil},
        {"max", l_math_max}, {"min", l_math_min}, {NULL, NULL}
    };
    luaL_newlib(L, math_funcs);
    lua_newuserdatauv(L, sizeof(uint32_t), 0);
    *(uint32_t *)lua_touserdata(L, -1) = (uint32_t)esp_timer_get_time();
    xorshift32((uint32_t *)lua_touserdata(L, -1));
    lua_pushvalue(L, -1);
    lua_pushcclosure(L, l_math_random, 1); lua_setfield(L, -3, "random");
    lua_pushcclosure(L, l_math_randomseed, 1); lua_setfield(L, -2, "randomseed");
    lua_setglobal(L, "math");
}

static int l_initialize_runtime(lua_State *L) {
    ghostscript_runtime_t *rt = (ghostscript_runtime_t *)lua_touserdata(L, 1);
    if (!rt) return luaL_error(L, "runtime missing");
    luaL_requiref(L, LUA_GNAME, luaopen_base, 1); lua_pop(L, 1);
    lua_pushnil(L); lua_setglobal(L, "dofile");
    lua_pushnil(L); lua_setglobal(L, "loadfile");
    lua_pushnil(L); lua_setglobal(L, "load");
    luaL_requiref(L, "string", luaopen_string, 1); lua_pop(L, 1);
    luaL_requiref(L, "table", luaopen_table, 1); lua_pop(L, 1);
    open_math_lite(L);
    register_api(L, rt);
    lua_gc(L, LUA_GCSETPAUSE, 150);
    lua_gc(L, LUA_GCCOLLECT);
    lua_newthread(L);
    return 1;
}

static int l_probe_on_tick(lua_State *L) {
    lua_getglobal(L, "on_tick");
    lua_pushboolean(L, lua_isfunction(L, -1));
    return 1;
}

static bool runtime_finish_script_if_needed(ghostscript_runtime_t *rt) {
    /* Probe for on_tick inside a protected call: lua_getglobal interns the
     * string and allocates, and a script that finishes with its heap at the
     * memory_limit would otherwise luaD_throw with no error handler on the C
     * stack -> abort() -> reboot. lua_pushcfunction pushes a light C function
     * (no allocation), so this stays safe even when the heap is full. */
    bool has_tick = false;
    lua_pushcfunction(rt->L, l_probe_on_tick);
    if (lua_pcall(rt->L, 0, 1, 0) == LUA_OK) {
        has_tick = lua_toboolean(rt->L, -1);
    }
    lua_pop(rt->L, 1);
    if (!has_tick && !rt->stop_requested) rt->state = GHOSTSCRIPT_STATE_DONE;
    return has_tick;
}

static const char *lua_rc_name(int rc) {
    switch (rc) {
        case LUA_OK: return "ok";
        case LUA_YIELD: return "yield";
        case LUA_ERRRUN: return "runtime";
        case LUA_ERRMEM: return "memory";
        case LUA_ERRERR: return "error handler";
        case LUA_ERRSYNTAX: return "syntax";
        default: return "unknown";
    }
}

static void runtime_set_lua_error(ghostscript_runtime_t *rt, lua_State *L, int rc) {
    if (!rt) return;
    if (rc == LUA_ERRMEM) {
        snprintf(rt->error, sizeof(rt->error), "%s error: memory limit exceeded (%lu/%lu bytes)",
                 lua_rc_name(rc), (unsigned long)rt->memory_used, (unsigned long)rt->memory_limit);
        return;
    }
    if (!L) {
        snprintf(rt->error, sizeof(rt->error), "%s error, heap %lu/%lu bytes",
                 lua_rc_name(rc), (unsigned long)rt->memory_used, (unsigned long)rt->memory_limit);
        return;
    }
    const char *msg = lua_isstring(L, -1) ? lua_tostring(L, -1) : NULL;
    if (msg && msg[0]) {
        snprintf(rt->error, sizeof(rt->error), "%s error: %.100s", lua_rc_name(rc), msg);
    } else {
        int ltype = lua_type(L, -1);
        const char *tname = lua_typename(L, ltype);
        const char *val = luaL_tolstring(L, -1, NULL);
        snprintf(rt->error, sizeof(rt->error), "%s err %s: %.50s h %lu/%lu",
                 lua_rc_name(rc), tname ? tname : "?", val ? val : "?",
                 (unsigned long)rt->memory_used, (unsigned long)rt->memory_limit);
        lua_pop(L, 1);
    }
}

static bool runtime_resume_script(ghostscript_runtime_t *rt) {
    if (!rt || !rt->thread || rt->script_done || rt->state != GHOSTSCRIPT_STATE_RUNNING) return true;
    rt->resume_after_ms = 0;
    int nres = 0;
    runtime_begin_slice(rt);
    int rc = lua_resume(rt->thread, NULL, 0, &nres);
    if (rc == LUA_OK) {
        if (nres > 0) lua_pop(rt->thread, nres);
        rt->script_done = true;
        runtime_finish_script_if_needed(rt);
        return true;
    }
    if (rc == LUA_YIELD) {
        if (nres > 0) lua_pop(rt->thread, nres);
        return true;
    }
    if (runtime_finish_requested_exit(rt, rt->thread)) return true;
    runtime_set_lua_error(rt, rt->thread, rc);
    rt->state = GHOSTSCRIPT_STATE_FAILED;
    return false;
}

bool ghostscript_runtime_start(ghostscript_runtime_t *rt) {
    if (!rt || rt->state != GHOSTSCRIPT_STATE_LOADED) return false;
    bool display_was_suspended = false;
    if (!ghostscript_manager_sd_begin(&display_was_suspended)) {
        snprintf(rt->error, sizeof(rt->error), "SD mount failed for script: %.90s", rt->manifest.entry_path);
        rt->state = GHOSTSCRIPT_STATE_FAILED;
        return false;
    }
    FILE *script = open_script_for_streaming(rt->manifest.entry_path);
    if (!script) {
        snprintf(rt->error, sizeof(rt->error), "failed to read script: %.90s", rt->manifest.entry_path);
        rt->state = GHOSTSCRIPT_STATE_FAILED;
        ghostscript_manager_sd_end(display_was_suspended);
        return false;
    }
    rt->api = plugin_api_get(rt->manifest.id, rt->manifest.base_path, rt->manifest.permissions,
                             rt->manifest.memory_limit, rt->manifest.allow_absolute_storage);
    rt->api_active = rt->api != NULL;
    if (!rt->api) {
        snprintf(rt->error, sizeof(rt->error), "app API busy for script: %.90s", rt->manifest.id);
        fclose(script);
        ghostscript_manager_sd_end(display_was_suspended);
        rt->state = GHOSTSCRIPT_STATE_FAILED;
        return false;
    }
    rt->L = lua_newstate(gs_alloc, rt);
    if (!rt->L) {
        snprintf(rt->error, sizeof(rt->error), "not enough memory for Lua state (%lu bytes limit)", (unsigned long)rt->memory_limit);
        fclose(script);
        ghostscript_manager_sd_end(display_was_suspended);
        rt->state = GHOSTSCRIPT_STATE_FAILED;
        return false;
    }
    gs_reader_t reader = { .file = script, .buf = NULL };
    /* Sector-sized, DMA-capable, word-aligned buffer. Full-sector reads land
     * directly in it, so the SD-SPI driver never needs to allocate a DMA bounce
     * buffer -- that alloc fails under memory pressure on no-PSRAM targets and
     * the IDF driver NULL-derefs in memcpy (Cardputer crash). */
    reader.buf = heap_caps_aligned_alloc(32, GS_LOAD_CHUNK, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!reader.buf) {
        snprintf(rt->error, sizeof(rt->error), "not enough DMA memory for script load buffer");
        fclose(script);
        ghostscript_manager_sd_end(display_was_suspended);
        rt->state = GHOSTSCRIPT_STATE_FAILED;
        return false;
    }
    if (!script_reader_reserve_dma(&reader)) {
        snprintf(rt->error, sizeof(rt->error), "not enough DMA memory reserved for SD reads");
        heap_caps_free(reader.buf);
        fclose(script);
        ghostscript_manager_sd_end(display_was_suspended);
        rt->state = GHOSTSCRIPT_STATE_FAILED;
        return false;
    }
    /* Wrap load in a protected call so luaD_throw always finds an errorJmp.
     * Execution happens in a coroutine so ghost.delay() can yield top-level
     * scripts and keep the runner alive. */
    lua_pushcfunction(rt->L, l_load_chunk);
    lua_pushlightuserdata(rt->L, &reader);
    runtime_begin_slice(rt);
    int err = lua_pcall(rt->L, 1, 1, 0);
    heap_caps_free(reader.dma_reserve);
    heap_caps_free(reader.buf);
    reader.buf = NULL;
    fclose(script);
    ghostscript_manager_sd_end(display_was_suspended);
    if (err) {
        runtime_set_lua_error(rt, rt->L, err);
        rt->state = rt->stop_requested ? GHOSTSCRIPT_STATE_STOPPED : GHOSTSCRIPT_STATE_FAILED;
        return false;
    }
    /* Compile while SD is mounted before opening the libraries and creating a
     * coroutine. This keeps the internal DMA heap available to the SD driver. */
    lua_pushcfunction(rt->L, l_initialize_runtime);
    lua_pushlightuserdata(rt->L, rt);
    int init_err = lua_pcall(rt->L, 1, 1, 0);
    if (init_err != LUA_OK) {
        runtime_set_lua_error(rt, rt->L, init_err);
        rt->state = GHOSTSCRIPT_STATE_FAILED;
        return false;
    }
    rt->thread = lua_tothread(rt->L, -1);
    rt->thread_ref = luaL_ref(rt->L, LUA_REGISTRYINDEX);
    if (!rt->thread) {
        snprintf(rt->error, sizeof(rt->error), "failed to create Lua coroutine");
        rt->state = GHOSTSCRIPT_STATE_FAILED;
        return false;
    }
    runtime_begin_slice(rt);
    rt->state = GHOSTSCRIPT_STATE_RUNNING;
    lua_xmove(rt->L, rt->thread, 1);
    lua_sethook(rt->L, hook, LUA_MASKCOUNT, runtime_hook_count(rt));
    lua_sethook(rt->thread, hook, LUA_MASKCOUNT, runtime_hook_count(rt));
    QueueHandle_t queue = xQueueCreate(GS_EVENT_QUEUE_DEPTH, sizeof(gs_queued_event_t *));
    if (!queue) {
        snprintf(rt->error, sizeof(rt->error), "not enough memory for event queue");
        rt->state = GHOSTSCRIPT_STATE_FAILED;
        return false;
    }
    taskENTER_CRITICAL(&s_active_runtime_mux);
    if (!s_event_queue) s_event_queue = queue;
    bool queue_ready = s_event_queue == queue;
    taskEXIT_CRITICAL(&s_active_runtime_mux);
    if (!queue_ready) vQueueDelete(queue);
    if (!queue_ready) {
        snprintf(rt->error, sizeof(rt->error), "not enough memory for event queue");
        rt->state = GHOSTSCRIPT_STATE_FAILED;
        return false;
    }
    ghostscript_runtime_set_active(rt);
    return runtime_resume_script(rt);
}

static void runtime_drain_events(ghostscript_runtime_t *rt) {
    if (!rt) return;
    gs_queued_event_t *event = NULL;
    while (s_event_queue && xQueueReceive(s_event_queue, &event, 0) == pdPASS) {
        if (event->kind == GS_QUEUED_EVENT) {
            gs_queued_text_event_t *text_event = (gs_queued_text_event_t *)event;
            const char *value = text_event->text + strlen(text_event->text) + 1;
            dispatch_event(rt, text_event->text, value);
        } else {
            runtime_process_input(rt, &((gs_queued_input_t *)event)->input);
        }
        heap_caps_free(event);
        if (rt->state != GHOSTSCRIPT_STATE_RUNNING) return;
    }
}

void ghostscript_runtime_tick(ghostscript_runtime_t *rt, uint32_t elapsed_ms) {
    if (!rt || !rt->L || rt->state != GHOSTSCRIPT_STATE_RUNNING) return;
    runtime_drain_events(rt);
    if (rt->state != GHOSTSCRIPT_STATE_RUNNING) return;
    runtime_expire_waits(rt);
    if (rt->state != GHOSTSCRIPT_STATE_RUNNING) return;
    if (!rt->script_done) {
        if (rt->waiting_for_event) return;
        if (rt->resume_after_ms > elapsed_ms) {
            rt->resume_after_ms -= elapsed_ms;
            return;
        }
        rt->resume_after_ms = 0;
        runtime_resume_script(rt);
        if (!rt || rt->state != GHOSTSCRIPT_STATE_RUNNING || !rt->script_done) return;
    }
    lua_getglobal(rt->L, "on_tick");
    if (!lua_isfunction(rt->L, -1)) { lua_pop(rt->L, 1); return; }
    lua_pushinteger(rt->L, elapsed_ms);
    runtime_begin_slice(rt);
    int rc = lua_pcall(rt->L, 1, 0, 0);
    if (rc != LUA_OK) {
        if (runtime_finish_requested_exit(rt, rt->L)) return;
        runtime_set_lua_error(rt, rt->L, rc);
        rt->state = GHOSTSCRIPT_STATE_FAILED;
    }
}

static void runtime_process_input(ghostscript_runtime_t *rt, const InputEvent *event) {
    if (!rt || !rt->L || !event) return;
    lua_newtable(rt->L);
    const char *type = "unknown";
    switch (event->type) {
        case INPUT_TYPE_TOUCH: type = "touch"; break;
        case INPUT_TYPE_JOYSTICK: type = "joystick"; break;
        case INPUT_TYPE_KEYBOARD: type = "keyboard"; break;
        case INPUT_TYPE_ENCODER: type = "encoder"; break;
        case INPUT_TYPE_EXIT_BUTTON: type = "back"; break;
        case INPUT_TYPE_HOME_BUTTON: type = "home"; break;
    }
    lua_pushstring(rt->L, type); lua_setfield(rt->L, -2, "type");
    if (event->type == INPUT_TYPE_KEYBOARD) { lua_pushinteger(rt->L, event->data.key_value); lua_setfield(rt->L, -2, "key"); }
    if (event->type == INPUT_TYPE_JOYSTICK) { lua_pushinteger(rt->L, event->data.joystick_index); lua_setfield(rt->L, -2, "index"); lua_pushboolean(rt->L, event->data.joystick_pressed); lua_setfield(rt->L, -2, "pressed"); }
    if (event->type == INPUT_TYPE_ENCODER) { lua_pushinteger(rt->L, event->data.encoder.direction); lua_setfield(rt->L, -2, "direction"); lua_pushboolean(rt->L, event->data.encoder.button); lua_setfield(rt->L, -2, "button"); }
    if (event->type == INPUT_TYPE_TOUCH) {
        lua_pushinteger(rt->L, event->data.touch_data.point.x); lua_setfield(rt->L, -2, "x");
        lua_pushinteger(rt->L, event->data.touch_data.point.y); lua_setfield(rt->L, -2, "y");
    }
    const char *topic = "input";
    char topic_buf[32];
    if (event->type == INPUT_TYPE_JOYSTICK) {
        snprintf(topic_buf, sizeof(topic_buf), "input.joy.%d", event->data.joystick_index);
        topic = topic_buf;
    } else if (event->type == INPUT_TYPE_ENCODER) {
        topic = event->data.encoder.button ? "input.encoder.click" : "input.encoder";
    } else if (event->type == INPUT_TYPE_TOUCH) {
        topic = "input.touch";
    } else if (event->type == INPUT_TYPE_EXIT_BUTTON) {
        topic = "input.back";
    } else if (event->type == INPUT_TYPE_KEYBOARD) {
        snprintf(topic_buf, sizeof(topic_buf), "input.key.%d", event->data.key_value);
        topic = topic_buf;
    }
    char serialized[160];
    snprintf(serialized, sizeof(serialized), "%s", type);
    dispatch_event(rt, topic, serialized);
    if (rt->state != GHOSTSCRIPT_STATE_RUNNING) {
        lua_pop(rt->L, 1);
        return;
    }
    lua_pushvalue(rt->L, -1);
    dispatch_event(rt, "input", serialized);
    if (rt->state != GHOSTSCRIPT_STATE_RUNNING) {
        lua_pop(rt->L, 2);
        return;
    }
    lua_pop(rt->L, 1);
    lua_getglobal(rt->L, "on_input");
    if (!lua_isfunction(rt->L, -1)) { lua_pop(rt->L, 2); return; }
    lua_insert(rt->L, -2);
    runtime_begin_slice(rt);
    int rc = lua_pcall(rt->L, 1, 0, 0);
    if (rc != LUA_OK) {
        if (runtime_finish_requested_exit(rt, rt->L)) return;
        runtime_set_lua_error(rt, rt->L, rc);
        rt->state = GHOSTSCRIPT_STATE_FAILED;
    }
}

void ghostscript_runtime_input(ghostscript_runtime_t *rt, const InputEvent *event) {
    if (!rt || !event) return;
    gs_queued_input_t *queued = gs_heap_alloc(sizeof(*queued));
    if (!queued) return;
    queued->kind = GS_QUEUED_INPUT;
    queued->input = *event;
    (void)queue_event(rt, (gs_queued_event_t *)queued);
}

void ghostscript_runtime_stop(ghostscript_runtime_t *rt) {
    if (!rt) return;
    rt->stop_requested = true;
    if (rt->state == GHOSTSCRIPT_STATE_RUNNING || rt->state == GHOSTSCRIPT_STATE_LOADED) rt->state = GHOSTSCRIPT_STATE_STOPPED;
}

void ghostscript_runtime_destroy(ghostscript_runtime_t *rt) {
    if (!rt) return;
    QueueHandle_t queue = NULL;
    taskENTER_CRITICAL(&s_active_runtime_mux);
    if (s_active_runtime == rt) {
        s_active_runtime = NULL;
        queue = s_event_queue;
        s_event_queue = NULL;
    }
    taskEXIT_CRITICAL(&s_active_runtime_mux);
    while (queue) {
        taskENTER_CRITICAL(&s_active_runtime_mux);
        bool busy = s_event_senders != 0;
        taskEXIT_CRITICAL(&s_active_runtime_mux);
        if (!busy) break;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (queue) {
        gs_queued_event_t *event = NULL;
        while (xQueueReceive(queue, &event, 0) == pdPASS) heap_caps_free(event);
        vQueueDelete(queue);
    }
    for (int i = 0; i < rt->oui_prefix_count; ++i) free(rt->oui_prefix[i]);
    rt->destroying = true;
    if (rt->L) lua_close(rt->L);
    if (rt->api_active) plugin_api_release();
    heap_caps_free(rt);
}

ghostscript_state_t ghostscript_runtime_state(const ghostscript_runtime_t *rt) { return rt ? rt->state : GHOSTSCRIPT_STATE_EMPTY; }
const char *ghostscript_runtime_error(const ghostscript_runtime_t *rt) { return rt ? rt->error : ""; }
size_t ghostscript_runtime_memory_used(const ghostscript_runtime_t *rt) { return rt ? rt->memory_used : 0; }
size_t ghostscript_runtime_memory_peak(const ghostscript_runtime_t *rt) { return rt ? rt->memory_peak : 0; }
size_t ghostscript_runtime_memory_limit(const ghostscript_runtime_t *rt) { return rt ? rt->memory_limit : 0; }
const ghostscript_manifest_t *ghostscript_runtime_manifest(const ghostscript_runtime_t *rt) { return rt ? &rt->manifest : NULL; }

void ghostscript_runtime_dispatch_event(ghostscript_runtime_t *rt, const char *name, const char *value) {
    if (!rt) return;
    (void)queue_text_event(rt, name, value);
}

void ghostscript_emit_event(const char *name, const char *value) {
    ghostscript_runtime_t *rt = ghostscript_runtime_get_active();
    if (!rt) return;
    (void)queue_text_event(rt, name, value);
}

void ghostscript_emit_event_escaped(const char *name, const char *value) {
    if (!name) return;
    if (!value) { ghostscript_emit_event(name, ""); return; }
    char buf[256];
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)value; *p && o < sizeof(buf) - 5; ++p) {
        unsigned char c = *p;
        if (c == '|' || c == '\\' || c == '\n' || c == '\r') {
            if (o + 2 >= sizeof(buf)) break;
            buf[o++] = '\\';
            buf[o++] = (c == '|') ? 'p' : (c == '\\') ? 'b' : (c == '\n') ? 'n' : 'r';
        } else if (c < 0x20 || c == 0x7f) {
            if (o + 4 >= sizeof(buf)) break;
            buf[o++] = '\\';
            buf[o++] = 'x';
            static const char hex[] = "0123456789abcdef";
            buf[o++] = hex[(c >> 4) & 0xf];
            buf[o++] = hex[c & 0xf];
        } else {
            buf[o++] = (char)c;
        }
    }
    buf[o] = '\0';
    ghostscript_emit_event(name, buf);
}

static uint8_t seen_hash(const uint8_t mac[6]) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < 6; ++i) h = (h ^ mac[i]) * 16777619u;
    return (uint8_t)(h & 63);
}

bool ghostscript_runtime_mark_ble_seen(const uint8_t mac[6]) {
    if (!mac) return true;
    taskENTER_CRITICAL(&s_active_runtime_mux);
    ghostscript_runtime_t *rt = s_active_runtime;
    if (!rt) {
        taskEXIT_CRITICAL(&s_active_runtime_mux);
        return true;
    }
    uint8_t start = seen_hash(mac);
    for (int probe = 0; probe < 64; ++probe) {
        uint8_t idx = (start + probe) & 63;
        if (rt->ble_seen_used[idx] && memcmp(rt->ble_seen_keys[idx], mac, 6) == 0) {
            taskEXIT_CRITICAL(&s_active_runtime_mux);
            return true; /* already seen */
        }
    }
    uint8_t idx = start;
    for (int probe = 0; probe < 64; ++probe) {
        idx = (start + probe) & 63;
        if (!rt->ble_seen_used[idx]) {
            memcpy(rt->ble_seen_keys[idx], mac, 6);
            rt->ble_seen_used[idx] = 1;
            taskEXIT_CRITICAL(&s_active_runtime_mux);
            return false; /* first time this scan */
        }
    }
    /* full; overwrite oldest by hashing again (rare) */
    memcpy(rt->ble_seen_keys[idx], mac, 6);
    taskEXIT_CRITICAL(&s_active_runtime_mux);
    return false;
}

void ghostscript_runtime_reset_ble_seen(void) {
    taskENTER_CRITICAL(&s_active_runtime_mux);
    if (s_active_runtime) memset(s_active_runtime->ble_seen_used, 0, sizeof(s_active_runtime->ble_seen_used));
    taskEXIT_CRITICAL(&s_active_runtime_mux);
}
