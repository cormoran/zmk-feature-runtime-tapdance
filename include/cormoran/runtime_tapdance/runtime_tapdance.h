/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <zmk/behavior.h>
#include <cormoran/zmk/custom_settings.h>
#define ZMK_RUNTIME_TAPDANCE_SUBSYSTEM_ID "cormoran__runtime_tapdance"
#define ZMK_RUNTIME_TAPDANCE_SLOTS_KEY "slots"
#define ZMK_RUNTIME_TAPDANCE_TIMEOUT_MS_KEY "timeout_ms"
struct zmk_runtime_tapdance_config {
    struct zmk_custom_setting_behavior_value single_binding;
    struct zmk_custom_setting_behavior_value double_binding;
    struct zmk_custom_setting_behavior_value triple_binding;
};
int zmk_runtime_tapdance_read(uint32_t index, struct zmk_runtime_tapdance_config *config);
int zmk_runtime_tapdance_write(uint32_t index, const struct zmk_runtime_tapdance_config *config,
                               bool persist);
int zmk_runtime_tapdance_set_binding(uint32_t index, uint32_t tap_count,
                                     const struct zmk_custom_setting_behavior_value *binding,
                                     bool persist);
int zmk_runtime_tapdance_delete(uint32_t index, bool persist);
uint32_t zmk_runtime_tapdance_timeout_ms(void);
int zmk_runtime_tapdance_set_timeout_ms(uint32_t value, bool persist);
uint32_t zmk_runtime_tapdance_max_count(void);
bool zmk_runtime_tapdance_is_behavior(const char *name);
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
int zmk_runtime_tapdance_pressed(uint32_t slot, struct zmk_behavior_binding_event event);
int zmk_runtime_tapdance_released(struct zmk_behavior_binding_event event);
#endif
