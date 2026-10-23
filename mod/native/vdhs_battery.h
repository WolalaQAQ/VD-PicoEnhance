// PICO controller battery actions, using VD's existing controller action set.
#pragma once
#include <stdint.h>

typedef void (*vdhs_xr_function)(void);
typedef int32_t (*vdhs_xr_get_proc)(uint64_t, const char*, vdhs_xr_function*);

int vdhs_battery_install(void);
void vdhs_battery_begin_instance(uint64_t instance, vdhs_xr_get_proc get_proc);
void vdhs_battery_on_grip_action(uint64_t action_set);
