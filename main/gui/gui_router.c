#include "gui/gui_router.h"

#include "esp_log.h"
#include "managers/display_manager.h"
#include "managers/views/lockscreen.h"
#include "managers/views/main_menu_screen.h"
#include "managers/views/splash_screen.h"
#include <stdlib.h>
#include <string.h>

#define GUI_ROUTE_STACK_MAX 32u

static const char *TAG = "GuiRouter";
static gui_route_t s_routes[GUI_ROUTE_STACK_MAX];
static size_t s_depth;
static View *s_previous_view;

static const char *route_name(const gui_route_t *route) {
    return route && route->view && route->view->name ? route->view->name : "none";
}

static bool route_equals(const gui_route_t *a, const gui_route_t *b) {
    if (!a || !b || a->id != b->id || a->view != b->view ||
        a->selected != b->selected || a->scroll_y != b->scroll_y ||
        a->item_index != b->item_index || a->parent_id != b->parent_id) {
        return false;
    }
    return memcmp(a->state, b->state, sizeof(a->state)) == 0;
}

static bool route_is_transient(const gui_route_t *route) {
    return route && (route->view == &splash_view || route->view == &lockscreen_view);
}

static void render_current(void) {
    if (s_depth > 0 && s_routes[s_depth - 1].view) {
        display_manager_render_view(s_routes[s_depth - 1].view);
    }
}

void gui_router_navigate_immediate(const gui_route_t *route) {
    if (!route || !route->view) return;
    if (s_depth > 0 && route_equals(&s_routes[s_depth - 1], route)) return;

    s_previous_view = s_depth > 0 ? s_routes[s_depth - 1].view : NULL;
    if (s_depth >= GUI_ROUTE_STACK_MAX) {
        memmove(&s_routes[0], &s_routes[1],
                (GUI_ROUTE_STACK_MAX - 1u) * sizeof(s_routes[0]));
        s_depth = GUI_ROUTE_STACK_MAX - 1u;
    }
    s_routes[s_depth++] = *route;
    ESP_LOGD(TAG, "NAV PUSH %s depth=%u", route_name(route), (unsigned)s_depth);
    render_current();
}

void gui_router_replace_immediate(const gui_route_t *route) {
    if (!route || !route->view) return;
    s_previous_view = s_depth > 0 ? s_routes[s_depth - 1].view : NULL;
    if (s_depth == 0) {
        s_routes[s_depth++] = *route;
    } else {
        s_routes[s_depth - 1] = *route;
    }
    ESP_LOGD(TAG, "NAV REPLACE %s depth=%u", route_name(route), (unsigned)s_depth);
    render_current();
}

void gui_router_back_immediate(void) {
    const char *from = s_depth > 0 ? route_name(&s_routes[s_depth - 1]) : "none";
    s_previous_view = s_depth > 0 ? s_routes[s_depth - 1].view : NULL;
    if (s_depth > 1) {
        s_depth--;
    } else {
        gui_route_t root = {.id = GUI_ROUTE_VIEW, .view = &main_menu_view};
        s_routes[0] = root;
        s_depth = 1;
    }
    ESP_LOGD(TAG, "NAV BACK %s -> %s depth=%u", from,
             route_name(&s_routes[s_depth - 1]), (unsigned)s_depth);
    render_current();
}

void gui_router_reset_immediate(const gui_route_t *route) {
    if (!route || !route->view) return;
    s_previous_view = s_depth > 0 ? s_routes[s_depth - 1].view : NULL;
    s_routes[0] = *route;
    s_depth = 1;
    ESP_LOGD(TAG, "NAV RESET %s depth=1", route_name(route));
    render_current();
}

typedef struct {
    gui_route_t route;
    void (*operation)(const gui_route_t *route);
} route_call_t;

static void run_route_call(void *arg) {
    route_call_t *call = arg;
    if (!call) return;
    call->operation(&call->route);
    free(call);
}

static void schedule_route(const gui_route_t *route,
                           void (*operation)(const gui_route_t *route)) {
    if (!route || !operation) return;
    route_call_t *call = malloc(sizeof(*call));
    if (!call) return;
    call->route = *route;
    call->operation = operation;
    if (!display_manager_run_on_lvgl(run_route_call, call)) {
        free(call);
    }
}

void gui_router_navigate(const gui_route_t *route) {
    schedule_route(route, gui_router_navigate_immediate);
}

void gui_router_replace(const gui_route_t *route) {
    schedule_route(route, gui_router_replace_immediate);
}

void gui_router_reset(const gui_route_t *route) {
    schedule_route(route, gui_router_reset_immediate);
}

static void run_back(void *arg) {
    (void)arg;
    gui_router_back_immediate();
}

void gui_router_back(void) {
    display_manager_run_on_lvgl(run_back, NULL);
}

const gui_route_t *gui_router_current(void) {
    return s_depth > 0 ? &s_routes[s_depth - 1] : NULL;
}

View *gui_router_previous_view(void) {
    return s_previous_view;
}

size_t gui_router_depth(void) {
    return s_depth;
}

void gui_router_legacy_switch_immediate(View *view) {
    if (!view) return;
    gui_route_t target = {.id = GUI_ROUTE_VIEW, .view = view};

    if (s_depth == 0 || route_is_transient(&s_routes[s_depth - 1])) {
        gui_router_reset_immediate(&target);
        return;
    }

    /* Legacy code often switches directly to an ancestor instead of saying
     * Back. Keep that compatibility isolated here while migrated code uses
     * explicit operations and never infers direction. */
    for (size_t i = s_depth - 1; i > 0; --i) {
        if (s_routes[i - 1].view == view) {
            s_previous_view = s_routes[s_depth - 1].view;
            s_depth = i;
            ESP_LOGW(TAG, "legacy ancestor switch to %s", view->name);
            render_current();
            return;
        }
    }
    gui_router_navigate_immediate(&target);
}
