/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <cormoran/zmk/custom_settings.h>
#define ZMK_RUNTIME_TAPDANCE_SUBSYSTEM_ID "cormoran__runtime_tapdance"
#define ZMK_RUNTIME_TAPDANCE_SLOTS_KEY "slots"
#define ZMK_RUNTIME_TAPDANCE_TIMEOUT_MS_KEY "timeout_ms"
/* Dance actions use a separate position namespace so immediate normal
 * hold-tap/sticky behavior state cannot alias the additional dance binding. */
#define ZMK_RUNTIME_TAPDANCE_ACTION_POSITION(index) (0x40000000u + (index))
struct zmk_runtime_tapdance_config {
    bool enabled;
    uint32_t position;
    bool delay_single;
    struct zmk_custom_setting_behavior_value double_binding;
    struct zmk_custom_setting_behavior_value triple_binding;
};
int zmk_runtime_tapdance_read(uint32_t index, struct zmk_runtime_tapdance_config *config);
int zmk_runtime_tapdance_write(uint32_t index, const struct zmk_runtime_tapdance_config *config,
                               bool persist);
int zmk_runtime_tapdance_delete(uint32_t index, bool persist);
uint32_t zmk_runtime_tapdance_timeout_ms(void);
int zmk_runtime_tapdance_set_timeout_ms(uint32_t value, bool persist);
uint32_t zmk_runtime_tapdance_max_count(void);
