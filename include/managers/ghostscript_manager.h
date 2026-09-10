#pragma once

#include "managers/plugin_manager.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GHOSTSCRIPT_ROOT_DIR "/mnt/ghostesp/scripts"
#define GHOSTSCRIPT_DATA_DIR "/mnt/ghostesp/scriptdata"
#define GHOSTSCRIPT_EXT ".gsb"
#define GHOSTSCRIPT_ID_MAX PLUGIN_APP_ID_MAX
#define GHOSTSCRIPT_NAME_MAX PLUGIN_APP_NAME_MAX
#define GHOSTSCRIPT_PATH_MAX 192
#define GHOSTSCRIPT_DESC_MAX PLUGIN_APP_DESC_MAX
#define GHOSTSCRIPT_CATEGORY_MAX PLUGIN_APP_CATEGORY_MAX
#define GHOSTSCRIPT_VERSION_MAX PLUGIN_APP_VERSION_MAX
#define GHOSTSCRIPT_ERROR_MAX 128
#define GHOSTSCRIPT_DEFAULT_MEMORY_LIMIT 16384u
#define GHOSTSCRIPT_MAX_MEMORY_LIMIT_NO_PSRAM 24576u
#define GHOSTSCRIPT_MAX_MEMORY_LIMIT_PSRAM 196608u
#define GHOSTSCRIPT_FILE_MAX_BYTES 8192u
#define GHOSTSCRIPT_BROWSER_PAGE_SIZE 4

typedef struct {
    char id[GHOSTSCRIPT_ID_MAX];
    char name[GHOSTSCRIPT_NAME_MAX];
    char version[GHOSTSCRIPT_VERSION_MAX];
    char author[PLUGIN_APP_AUTHOR_MAX];
    char description[GHOSTSCRIPT_DESC_MAX];
    char category[GHOSTSCRIPT_CATEGORY_MAX];
    char base_path[GHOSTSCRIPT_PATH_MAX];
    char entry_path[GHOSTSCRIPT_PATH_MAX];
    char data_path[GHOSTSCRIPT_PATH_MAX];
    uint32_t permissions;
    uint32_t memory_limit;
    uint32_t instruction_budget;
    uint32_t timeout_ms;
    uint32_t launch_failure_count;
    bool allow_absolute_storage;
    bool valid;
    char error[GHOSTSCRIPT_ERROR_MAX];
} ghostscript_manifest_t;

typedef struct {
    char name[GHOSTSCRIPT_NAME_MAX];
    char path[GHOSTSCRIPT_PATH_MAX];
    bool is_dir;
    bool is_script;
    bool has_manifest;
} ghostscript_browser_entry_t;

void ghostscript_manager_init(void);
bool ghostscript_manager_sd_begin(bool *display_was_suspended);
void ghostscript_manager_sd_end(bool display_was_suspended);
bool ghostscript_manager_permission_from_string(const char *value, uint32_t *out);
bool ghostscript_manager_load_manifest(const char *path, ghostscript_manifest_t *out);
bool ghostscript_manager_make_single_file_manifest(const char *path, ghostscript_manifest_t *out);
int ghostscript_manager_list(const char *dir, int offset, ghostscript_browser_entry_t *out, int max_entries, bool *has_more);
bool ghostscript_manager_is_script_file(const char *name);
bool ghostscript_manager_is_safe_id(const char *id);
void ghostscript_manager_record_failure(const ghostscript_manifest_t *manifest, const char *error);
void ghostscript_manager_record_clean_exit(const ghostscript_manifest_t *manifest);
const char *ghostscript_manager_last_error(void);

#ifdef __cplusplus
}
#endif
