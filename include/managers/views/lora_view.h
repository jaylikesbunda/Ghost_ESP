// lora_view.h
#ifndef LORA_VIEW_H
#define LORA_VIEW_H

#include "managers/display_manager.h"

extern View lora_view;

void lora_view_create(void);
void lora_view_destroy(void);
void lora_view_update_remote_state(const char *state);

#endif
