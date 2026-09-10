#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct progress_bar_view_t progress_bar_view_t;

progress_bar_view_t *progress_bar_view_create(const char *title);
progress_bar_view_t *progress_bar_view_create_with_cancel(const char *title, void (*on_cancel)(void *), void *user_data);
void progress_bar_view_update(progress_bar_view_t *view, const char *title);
void progress_bar_view_set_subtext(progress_bar_view_t *view, const char *subtext);
void progress_bar_view_set_progress(progress_bar_view_t *view, size_t current, size_t total);
void progress_bar_view_set_indeterminate(progress_bar_view_t *view);
void progress_bar_view_close(progress_bar_view_t *view);
bool progress_bar_view_is_active(const progress_bar_view_t *view);

#ifdef __cplusplus
}
#endif
