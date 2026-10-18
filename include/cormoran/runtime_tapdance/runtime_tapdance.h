/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zmk/behavior.h>
#include <cormoran/zmk/custom_settings.h>
#define ZMK_RUNTIME_TAPDANCE_SUBSYSTEM_ID "cormoran__runtime_tapdance"
#define ZMK_RUNTIME_TAPDANCE_KEY_PREFIX "tapdance/"
#define ZMK_RUNTIME_TAPDANCE_NEXT_ID_KEY "next_id"
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
int zmk_runtime_tapdance_create(const char *name, bool persist, uint32_t *index);
int zmk_runtime_tapdance_rename(uint32_t index, const char *name, bool persist);
int zmk_runtime_tapdance_name(uint32_t index, char *name, size_t capacity,
                              bool *has_unsaved_changes);
typedef int (*zmk_runtime_tapdance_iter_cb_t)(uint32_t index, const char *name,
                                              bool has_unsaved_changes, void *context);
int zmk_runtime_tapdance_for_each(zmk_runtime_tapdance_iter_cb_t cb, void *context);
uint32_t zmk_runtime_tapdance_used_count(void);
size_t zmk_runtime_tapdance_pool_total(void);
size_t zmk_runtime_tapdance_pool_used(void);
bool zmk_runtime_tapdance_has_unsaved_changes(void);
int zmk_runtime_tapdance_save(uint32_t *affected_count);
int zmk_runtime_tapdance_discard(uint32_t *affected_count);
uint32_t zmk_runtime_tapdance_timeout_ms(void);
int zmk_runtime_tapdance_set_timeout_ms(uint32_t value, bool persist);
uint32_t zmk_runtime_tapdance_max_count(void);
bool zmk_runtime_tapdance_is_behavior(const char *name);
#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
int zmk_runtime_tapdance_pressed(uint32_t slot, struct zmk_behavior_binding_event event);
int zmk_runtime_tapdance_released(struct zmk_behavior_binding_event event);
#endif
