/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <cormoran/runtime_tapdance/runtime_tapdance.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define COUNT CONFIG_ZMK_RUNTIME_TAPDANCE_MAX_TAPDANCES
#define PACKED_SIZE (4 + CONFIG_ZMK_RUNTIME_TAPDANCE_NAME_MAX_LEN + 72 + 8)
#define KEY_SIZE (sizeof(ZMK_RUNTIME_TAPDANCE_KEY_PREFIX) + 10)
#define LEGACY_PACKED_SIZE 37
BUILD_ASSERT(4 + CONFIG_ZMK_RUNTIME_TAPDANCE_NAME_MAX_LEN <=
             CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE);
BUILD_ASSERT(PACKED_SIZE <= CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUE_MAX_SIZE,
             "Set CONFIG_ZMK_CUSTOM_SETTINGS_LARGE_VALUE_MAX_SIZE to at least 108");
/* Keep the old array registered for one-way migration of pre-pool settings. */
#define LEGACY_SLOT_DEFAULT(i, _) {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES, .size = 0}
static const struct zmk_custom_setting_value legacy_defaults[COUNT] = {
    LISTIFY(COUNT, LEGACY_SLOT_DEFAULT, (, ))};
ZMK_CUSTOM_SETTING_ARRAY_DEFINE(runtime_tapdance_legacy_slots, ZMK_RUNTIME_TAPDANCE_SUBSYSTEM_ID,
                                "slots", ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES, COUNT, COUNT,
                                legacy_defaults, ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                                ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                ZMK_CUSTOM_SETTING_PERMISSION_SECURE,
                                ZMK_CUSTOM_SETTING_NO_CONSTRAINT);
ZMK_CUSTOM_SETTING_KEYSPACE_DEFINE_WITH_POOL_SIZE(
    runtime_tapdances, ZMK_RUNTIME_TAPDANCE_SUBSYSTEM_ID, ZMK_RUNTIME_TAPDANCE_KEY_PREFIX,
    ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES, PACKED_SIZE, KEY_SIZE, COUNT,
    CONFIG_ZMK_RUNTIME_TAPDANCE_POOL_BYTES, ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
    ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE, ZMK_CUSTOM_SETTING_PERMISSION_SECURE,
    ZMK_CUSTOM_SETTING_NO_CONSTRAINT);
ZMK_CUSTOM_SETTING_DEFINE(runtime_tapdance_next_id, ZMK_RUNTIME_TAPDANCE_SUBSYSTEM_ID,
                          ZMK_RUNTIME_TAPDANCE_NEXT_ID_KEY, ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
                          ZMK_CUSTOM_SETTING_VALUE_INT32(0),
                          ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                          ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                          ZMK_CUSTOM_SETTING_PERMISSION_SECURE, ZMK_CUSTOM_SETTING_NO_CONSTRAINT);
ZMK_CUSTOM_SETTING_DEFINE_WITH_CONSTRAINTS(
    runtime_tapdance_timeout, ZMK_RUNTIME_TAPDANCE_SUBSYSTEM_ID,
    ZMK_RUNTIME_TAPDANCE_TIMEOUT_MS_KEY, ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32,
    ZMK_CUSTOM_SETTING_VALUE_INT32(200), ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
    ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE, ZMK_CUSTOM_SETTING_PERMISSION_SECURE,
    ZMK_CUSTOM_SETTING_RANGE_INT32(1, 2000));

K_MUTEX_DEFINE(tapdance_lock);

static int binding_from_value(const struct zmk_custom_setting_behavior_value *value,
                              struct zmk_behavior_binding *binding) {
    if (!value->behavior_id) {
        return (value->param1 || value->param2) ? -EINVAL : 0;
    }
    if (value->behavior_id >= UINT16_MAX) {
        return -ENODEV;
    }
    const char *name = zmk_behavior_find_behavior_name_from_local_id(value->behavior_id);
    if (!name) {
        return -ENODEV;
    }
    if (zmk_runtime_tapdance_is_behavior(name)) {
        return -ELOOP;
    }
    *binding = (struct zmk_behavior_binding){
        .behavior_dev = name, .param1 = value->param1, .param2 = value->param2};
    return zmk_behavior_validate_binding(binding);
}

static int validate(const struct zmk_runtime_tapdance_config *config) {
    struct zmk_behavior_binding binding;
    int ret = binding_from_value(&config->single_binding, &binding);
    if (ret < 0) {
        return ret;
    }
    ret = binding_from_value(&config->double_binding, &binding);
    if (ret < 0) {
        return ret;
    }
    ret = binding_from_value(&config->triple_binding, &binding);
    if (ret < 0) {
        return ret;
    }
    for (size_t i = 0; i < 3; i++) {
        ret = binding_from_value(&config->hold_bindings[i], &binding);
        if (ret < 0) {
            return ret;
        }
    }
    if ((!config->single_binding.behavior_id &&
         (config->double_binding.behavior_id || config->triple_binding.behavior_id)) ||
        (config->triple_binding.behavior_id && !config->double_binding.behavior_id)) {
        return -EINVAL;
    }
    const bool taps[3] = {config->single_binding.behavior_id, config->double_binding.behavior_id,
                          config->triple_binding.behavior_id};
    if (config->interval_ms > 2000) {
        return -EINVAL;
    }
    for (size_t i = 0; i < 3; i++) {
        if ((!taps[i] && (config->hold_bindings[i].behavior_id || config->tap_intervals_ms[i])) ||
            config->tap_intervals_ms[i] > 2000) {
            return -EINVAL;
        }
    }
    return 0;
}

static struct zmk_custom_setting_behavior_value *
tap_binding(struct zmk_runtime_tapdance_config *config, size_t index) {
    return index == 0   ? &config->single_binding
           : index == 1 ? &config->double_binding
                        : &config->triple_binding;
}

static void read_packed_binding(const uint8_t *p,
                                struct zmk_custom_setting_behavior_value *binding) {
    *binding = (struct zmk_custom_setting_behavior_value){sys_get_le32(p), sys_get_le32(p + 4),
                                                          sys_get_le32(p + 8)};
}

static void write_packed_binding(uint8_t *p,
                                 const struct zmk_custom_setting_behavior_value *binding) {
    sys_put_le32(binding->behavior_id, p);
    sys_put_le32(binding->param1, p + 4);
    sys_put_le32(binding->param2, p + 8);
}

static int make_key(uint32_t index, char key[KEY_SIZE]) {
    if (index > INT32_MAX) {
        return -ERANGE;
    }
    int n = snprintf(key, KEY_SIZE, "%s%u", ZMK_RUNTIME_TAPDANCE_KEY_PREFIX, index);
    return n > 0 && n < KEY_SIZE ? 0 : -ENAMETOOLONG;
}

static const struct zmk_custom_setting *find_entry(uint32_t index) {
    char key[KEY_SIZE];
    if (make_key(index, key) < 0) {
        return NULL;
    }
    return zmk_custom_setting_keyspace_find(&runtime_tapdances, key);
}

static int decode_entry(const struct zmk_custom_setting *entry,
                        struct zmk_runtime_tapdance_config *config, char *name, size_t name_size) {
    uint8_t packed[PACKED_SIZE];
    size_t size = 0;
    int ret = zmk_custom_setting_read_into(entry, packed, sizeof(packed), &size, NULL);
    if (ret < 0) {
        return ret;
    }
    if (size < 4 || (packed[0] != 4 && packed[0] != 5) || packed[1] == 0 ||
        packed[1] > CONFIG_ZMK_RUNTIME_TAPDANCE_NAME_MAX_LEN ||
        (packed[0] == 4 && size != (size_t)packed[1] + 38)) {
        return -EINVAL;
    }
    if (name) {
        if (name_size <= packed[1]) {
            return -EMSGSIZE;
        }
        memcpy(name, packed + 2, packed[1]);
        name[packed[1]] = '\0';
    }
    if (config) {
        memset(config, 0, sizeof(*config));
        const uint8_t *p = packed + 2 + packed[1];
        if (packed[0] == 4) {
            for (size_t i = 0; i < 3; i++) {
                read_packed_binding(p + i * 12, tap_binding(config, i));
            }
        } else {
            if (size < (size_t)packed[1] + 4) {
                return -EINVAL;
            }
            uint16_t mask = sys_get_le16(p);
            if (mask & ~0x3ff) {
                return -EINVAL;
            }
            p += 2;
            size_t remaining = size - packed[1] - 4;
            for (size_t i = 0; i < 6; i++) {
                if (!(mask & BIT(i))) {
                    continue;
                }
                if (remaining < 12) {
                    return -EINVAL;
                }
                read_packed_binding(p,
                                    i < 3 ? tap_binding(config, i) : &config->hold_bindings[i - 3]);
                p += 12;
                remaining -= 12;
            }
            for (size_t i = 6; i < 10; i++) {
                if (!(mask & BIT(i))) {
                    continue;
                }
                if (remaining < 2) {
                    return -EINVAL;
                }
                uint16_t interval = sys_get_le16(p);
                if (i == 6)
                    config->interval_ms = interval;
                else
                    config->tap_intervals_ms[i - 7] = interval;
                p += 2;
                remaining -= 2;
            }
            if (remaining) {
                return -EINVAL;
            }
        }
        return validate(config);
    }
    if (packed[0] == 5) {
        struct zmk_runtime_tapdance_config checked;
        return decode_entry(entry, &checked, NULL, 0);
    }
    return 0;
}

static int encode_entry(const char *name, const struct zmk_runtime_tapdance_config *config,
                        uint8_t value[PACKED_SIZE], size_t *size) {
    size_t name_len = strlen(name);
    if (name_len == 0 || name_len > CONFIG_ZMK_RUNTIME_TAPDANCE_NAME_MAX_LEN) {
        return -EMSGSIZE;
    }
    uint8_t *p = value;
    p[0] = 5;
    p[1] = name_len;
    memcpy(p + 2, name, name_len);
    p += 2 + name_len;
    uint8_t *mask_at = p;
    p += 2;
    uint16_t mask = 0;
    for (size_t i = 0; i < 6; i++) {
        const struct zmk_custom_setting_behavior_value *binding =
            i < 3 ? tap_binding((struct zmk_runtime_tapdance_config *)config, i)
                  : &config->hold_bindings[i - 3];
        if (binding->behavior_id) {
            mask |= BIT(i);
            write_packed_binding(p, binding);
            p += 12;
        }
    }
    const uint16_t intervals[4] = {config->interval_ms, config->tap_intervals_ms[0],
                                   config->tap_intervals_ms[1], config->tap_intervals_ms[2]};
    for (size_t i = 0; i < 4; i++) {
        if (intervals[i]) {
            mask |= BIT(i + 6);
            sys_put_le16(intervals[i], p);
            p += 2;
        }
    }
    sys_put_le16(mask, mask_at);
    *size = p - value;
    return 0;
}

static int read_raw(uint32_t index, struct zmk_runtime_tapdance_config *config) {
    const struct zmk_custom_setting *entry = find_entry(index);
    return !config ? -EINVAL : !entry ? -ENOENT : decode_entry(entry, config, NULL, 0);
}

int zmk_runtime_tapdance_read(uint32_t index, struct zmk_runtime_tapdance_config *config) {
    k_mutex_lock(&tapdance_lock, K_FOREVER);
    int ret = read_raw(index, config);
    k_mutex_unlock(&tapdance_lock);
    return ret;
}

static int write_raw_locked(uint32_t index, const struct zmk_runtime_tapdance_config *config,
                            bool persist) {
    const struct zmk_custom_setting *entry = find_entry(index);
    if (!entry) {
        return -ENOENT;
    }
    char name[CONFIG_ZMK_RUNTIME_TAPDANCE_NAME_MAX_LEN + 1];
    int ret = decode_entry(entry, NULL, name, sizeof(name));
    if (ret < 0) {
        return ret;
    }
    uint8_t value[PACKED_SIZE];
    size_t size;
    ret = encode_entry(name, config, value, &size);
    if (ret < 0) {
        return ret;
    }
    return zmk_custom_setting_write_bytes(entry, value, size,
                                          persist ? ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST
                                                  : ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
}

int zmk_runtime_tapdance_write(uint32_t index, const struct zmk_runtime_tapdance_config *config,
                               bool persist) {
    if (!config) {
        return -EINVAL;
    }
    int ret = validate(config);
    if (ret < 0) {
        return ret;
    }
    k_mutex_lock(&tapdance_lock, K_FOREVER);
    ret = write_raw_locked(index, config, persist);
    k_mutex_unlock(&tapdance_lock);
    return ret;
}

static int set_binding(uint32_t index, uint32_t tap_count,
                       const struct zmk_custom_setting_behavior_value *binding, bool hold,
                       bool persist) {
    if (!binding || tap_count < 1 || tap_count > 3) {
        return -EINVAL;
    }
    k_mutex_lock(&tapdance_lock, K_FOREVER);
    struct zmk_runtime_tapdance_config config;
    int ret = read_raw(index, &config);
    if (ret == 0) {
        struct zmk_custom_setting_behavior_value *selected =
            hold ? &config.hold_bindings[tap_count - 1] : tap_binding(&config, tap_count - 1);
        *selected = *binding;
        ret = validate(&config);
        if (ret == 0) {
            ret = write_raw_locked(index, &config, persist);
        }
    }
    k_mutex_unlock(&tapdance_lock);
    return ret;
}

int zmk_runtime_tapdance_set_binding(uint32_t index, uint32_t tap_count,
                                     const struct zmk_custom_setting_behavior_value *binding,
                                     bool persist) {
    return set_binding(index, tap_count, binding, false, persist);
}

int zmk_runtime_tapdance_set_hold_binding(uint32_t index, uint32_t tap_count,
                                          const struct zmk_custom_setting_behavior_value *binding,
                                          bool persist) {
    return set_binding(index, tap_count, binding, true, persist);
}

int zmk_runtime_tapdance_set_interval(uint32_t index, uint32_t tap_count, uint32_t interval_ms,
                                      bool persist) {
    if (tap_count > 3 || interval_ms > 2000) {
        return -EINVAL;
    }
    k_mutex_lock(&tapdance_lock, K_FOREVER);
    struct zmk_runtime_tapdance_config config;
    int ret = read_raw(index, &config);
    if (ret == 0) {
        uint16_t *selected =
            tap_count ? &config.tap_intervals_ms[tap_count - 1] : &config.interval_ms;
        *selected = interval_ms;
        ret = validate(&config);
        if (ret == 0) {
            ret = write_raw_locked(index, &config, persist);
        }
    }
    k_mutex_unlock(&tapdance_lock);
    return ret;
}

int zmk_runtime_tapdance_delete(uint32_t index, bool persist) {
    ARG_UNUSED(persist);
    char key[KEY_SIZE];
    int ret = make_key(index, key);
    if (ret < 0) {
        return ret;
    }
    k_mutex_lock(&tapdance_lock, K_FOREVER);
    ret = zmk_custom_setting_keyspace_delete(&runtime_tapdances, key);
    k_mutex_unlock(&tapdance_lock);
    return ret;
}

uint32_t zmk_runtime_tapdance_max_count(void) { return COUNT; }

static int id_from_entry(const struct zmk_custom_setting *entry, uint32_t *id) {
    const char *key = zmk_custom_setting_public_key(entry);
    const size_t prefix_len = sizeof(ZMK_RUNTIME_TAPDANCE_KEY_PREFIX) - 1;
    if (!key || strncmp(key, ZMK_RUNTIME_TAPDANCE_KEY_PREFIX, prefix_len) != 0) {
        return -EINVAL;
    }
    const char *digits = key + prefix_len;
    if (!*digits) {
        return -EINVAL;
    }
    uint32_t result = 0;
    for (const char *p = digits; *p; p++) {
        if (*p < '0' || *p > '9' || result > (INT32_MAX - (*p - '0')) / 10) {
            return -EINVAL;
        }
        result = result * 10 + (*p - '0');
    }
    *id = result;
    return 0;
}

static bool name_exists_locked(const char *name, uint32_t except_id) {
    for (uint32_t i = 0; i < COUNT; i++) {
        if (!runtime_tapdances.slots[i].in_use) {
            continue;
        }
        const struct zmk_custom_setting *entry = &runtime_tapdances.slots[i].setting;
        uint32_t id;
        char current[CONFIG_ZMK_RUNTIME_TAPDANCE_NAME_MAX_LEN + 1];
        if (id_from_entry(entry, &id) == 0 && id != except_id &&
            decode_entry(entry, NULL, current, sizeof(current)) == 0 &&
            strcmp(current, name) == 0) {
            return true;
        }
    }
    return false;
}

int zmk_runtime_tapdance_create(const char *name, bool persist, uint32_t *index) {
    if (!name || !index || !name[0]) {
        return -EINVAL;
    }
    size_t length = strlen(name);
    if (length > CONFIG_ZMK_RUNTIME_TAPDANCE_NAME_MAX_LEN) {
        return -EMSGSIZE;
    }
    k_mutex_lock(&tapdance_lock, K_FOREVER);
    if (name_exists_locked(name, UINT32_MAX)) {
        k_mutex_unlock(&tapdance_lock);
        return -EEXIST;
    }
    bool free_slot = false;
    for (uint32_t i = 0; i < COUNT; i++) {
        free_slot |= !runtime_tapdances.slots[i].in_use;
    }
    if (!free_slot) {
        k_mutex_unlock(&tapdance_lock);
        return -ENOSPC;
    }
    struct zmk_custom_setting_value next;
    int ret = zmk_custom_setting_read_by_key(ZMK_RUNTIME_TAPDANCE_SUBSYSTEM_ID,
                                             ZMK_RUNTIME_TAPDANCE_NEXT_ID_KEY, &next);
    if (ret < 0 || next.type != ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32 || next.int32_value < 0 ||
        next.int32_value == INT32_MAX) {
        k_mutex_unlock(&tapdance_lock);
        return ret < 0 ? ret : -ENOSPC;
    }
    uint32_t id = next.int32_value;
    char key[KEY_SIZE];
    ret = make_key(id, key);
    if (ret == 0) {
        struct zmk_custom_setting_value value;
        const struct zmk_runtime_tapdance_config empty = {0};
        uint8_t packed[PACKED_SIZE];
        size_t size;
        ret = encode_entry(name, &empty, packed, &size);
        if (ret == 0) {
            value = (struct zmk_custom_setting_value){.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES,
                                                      .size = size};
            memcpy(value.bytes_value, packed, size);
        }
        if (ret == 0) {
            /* Persist the high-water mark even for RAM-only entries so a stale
             * keymap reference can never attach to a later entry. */
            next.int32_value++;
            ret = zmk_custom_setting_write_by_key(
                ZMK_RUNTIME_TAPDANCE_SUBSYSTEM_ID, ZMK_RUNTIME_TAPDANCE_NEXT_ID_KEY, &next,
                IS_ENABLED(CONFIG_SETTINGS_NONE) ? ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY
                                                 : ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST);
        }
        if (ret == 0) {
            ret = zmk_custom_setting_keyspace_create(&runtime_tapdances, key, &value,
                                                     persist ? ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST
                                                             : ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY,
                                                     NULL);
        }
    }
    if (ret == 0) {
        *index = id;
    }
    k_mutex_unlock(&tapdance_lock);
    return ret;
}

int zmk_runtime_tapdance_rename(uint32_t index, const char *name, bool persist) {
    if (!name || !name[0]) {
        return -EINVAL;
    }
    size_t length = strlen(name);
    if (length > CONFIG_ZMK_RUNTIME_TAPDANCE_NAME_MAX_LEN) {
        return -EMSGSIZE;
    }
    k_mutex_lock(&tapdance_lock, K_FOREVER);
    const struct zmk_custom_setting *entry = find_entry(index);
    int ret = !entry ? -ENOENT : name_exists_locked(name, index) ? -EEXIST : 0;
    if (ret == 0) {
        struct zmk_runtime_tapdance_config config;
        uint8_t value[PACKED_SIZE];
        size_t size;
        ret = decode_entry(entry, &config, NULL, 0);
        if (ret == 0) {
            ret = encode_entry(name, &config, value, &size);
        }
        if (ret == 0) {
            ret = zmk_custom_setting_write_bytes(entry, value, size,
                                                 persist ? ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST
                                                         : ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
        }
    }
    k_mutex_unlock(&tapdance_lock);
    return ret;
}

int zmk_runtime_tapdance_name(uint32_t index, char *name, size_t capacity,
                              bool *has_unsaved_changes) {
    if (!name) {
        return -EINVAL;
    }
    k_mutex_lock(&tapdance_lock, K_FOREVER);
    const struct zmk_custom_setting *entry = find_entry(index);
    int ret = entry ? decode_entry(entry, NULL, name, capacity) : -ENOENT;
    if (ret == 0 && has_unsaved_changes) {
        *has_unsaved_changes = zmk_custom_setting_has_unsaved_value(entry);
    }
    k_mutex_unlock(&tapdance_lock);
    return ret;
}

int zmk_runtime_tapdance_for_each(zmk_runtime_tapdance_iter_cb_t cb, void *context) {
    if (!cb) {
        return -EINVAL;
    }
    k_mutex_lock(&tapdance_lock, K_FOREVER);
    int ret = 0;
    for (uint32_t i = 0; i < COUNT && ret == 0; i++) {
        if (!runtime_tapdances.slots[i].in_use) {
            continue;
        }
        const struct zmk_custom_setting *entry = &runtime_tapdances.slots[i].setting;
        uint32_t id;
        char name[CONFIG_ZMK_RUNTIME_TAPDANCE_NAME_MAX_LEN + 1];
        ret = id_from_entry(entry, &id);
        if (ret == 0) {
            ret = decode_entry(entry, NULL, name, sizeof(name));
        }
        if (ret == 0) {
            ret = cb(id, name, zmk_custom_setting_has_unsaved_value(entry), context);
        }
    }
    k_mutex_unlock(&tapdance_lock);
    return ret;
}

uint32_t zmk_runtime_tapdance_used_count(void) {
    uint32_t count = 0;
    k_mutex_lock(&tapdance_lock, K_FOREVER);
    for (uint32_t i = 0; i < COUNT; i++) {
        count += runtime_tapdances.slots[i].in_use;
    }
    k_mutex_unlock(&tapdance_lock);
    return count;
}

size_t zmk_runtime_tapdance_pool_total(void) { return CONFIG_ZMK_RUNTIME_TAPDANCE_POOL_BYTES; }
size_t zmk_runtime_tapdance_pool_used(void) {
    return zmk_custom_setting_large_pool_used(runtime_tapdances.large_pool);
}

bool zmk_runtime_tapdance_has_unsaved_changes(void) {
    const struct zmk_custom_setting *interval = zmk_custom_setting_find(
        ZMK_RUNTIME_TAPDANCE_SUBSYSTEM_ID, ZMK_RUNTIME_TAPDANCE_TIMEOUT_MS_KEY);
    if (interval && zmk_custom_setting_has_unsaved_value(interval)) {
        return true;
    }
    bool dirty = false;
    k_mutex_lock(&tapdance_lock, K_FOREVER);
    for (uint32_t i = 0; i < COUNT; i++) {
        if (runtime_tapdances.slots[i].in_use &&
            zmk_custom_setting_has_unsaved_value(&runtime_tapdances.slots[i].setting)) {
            dirty = true;
            break;
        }
    }
    k_mutex_unlock(&tapdance_lock);
    return dirty;
}

int zmk_runtime_tapdance_save(uint32_t *affected_count) {
    uint32_t interval_count = 0, entry_count = 0;
    int ret =
        zmk_custom_settings_save_scope(ZMK_RUNTIME_TAPDANCE_SUBSYSTEM_ID,
                                       ZMK_RUNTIME_TAPDANCE_TIMEOUT_MS_KEY, NULL, &interval_count);
    if (ret == 0) {
        ret = zmk_custom_settings_save_scope(ZMK_RUNTIME_TAPDANCE_SUBSYSTEM_ID, NULL,
                                             ZMK_RUNTIME_TAPDANCE_KEY_PREFIX, &entry_count);
    }
    if (affected_count) {
        *affected_count = interval_count + entry_count;
    }
    return ret;
}

int zmk_runtime_tapdance_discard(uint32_t *affected_count) {
    uint32_t deleted = 0;
    k_mutex_lock(&tapdance_lock, K_FOREVER);
    for (uint32_t i = 0; i < COUNT; i++) {
        if (!runtime_tapdances.slots[i].in_use) {
            continue;
        }
        const struct zmk_custom_setting *entry = &runtime_tapdances.slots[i].setting;
        if (!(entry->state->flags & ZMK_CUSTOM_SETTING_STATE_HAS_PERSISTENT)) {
            char key[KEY_SIZE];
            snprintf(key, sizeof(key), "%s", zmk_custom_setting_public_key(entry));
            int ret = zmk_custom_setting_keyspace_delete(&runtime_tapdances, key);
            if (ret < 0) {
                k_mutex_unlock(&tapdance_lock);
                return ret;
            }
            deleted++;
        }
    }
    uint32_t interval_count = 0, entry_count = 0;
    int ret = zmk_custom_settings_discard_scope(ZMK_RUNTIME_TAPDANCE_SUBSYSTEM_ID,
                                                ZMK_RUNTIME_TAPDANCE_TIMEOUT_MS_KEY, NULL,
                                                &interval_count);
    if (ret == 0) {
        ret = zmk_custom_settings_discard_scope(ZMK_RUNTIME_TAPDANCE_SUBSYSTEM_ID, NULL,
                                                ZMK_RUNTIME_TAPDANCE_KEY_PREFIX, &entry_count);
    }
    k_mutex_unlock(&tapdance_lock);
    if (affected_count) {
        *affected_count = deleted + interval_count + entry_count;
    }
    return ret;
}

uint32_t zmk_runtime_tapdance_timeout_ms(void) {
    struct zmk_custom_setting_value value;
    if (zmk_custom_setting_read_by_key(ZMK_RUNTIME_TAPDANCE_SUBSYSTEM_ID,
                                       ZMK_RUNTIME_TAPDANCE_TIMEOUT_MS_KEY, &value) == 0 &&
        value.type == ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32 && value.int32_value >= 1 &&
        value.int32_value <= 2000) {
        return value.int32_value;
    }
    return 200;
}

int zmk_runtime_tapdance_set_timeout_ms(uint32_t value, bool persist) {
    if (value < 1 || value > 2000) {
        return -EINVAL;
    }
    struct zmk_custom_setting_value setting = ZMK_CUSTOM_SETTING_VALUE_INT32(value);
    return zmk_custom_setting_write_by_key(
        ZMK_RUNTIME_TAPDANCE_SUBSYSTEM_ID, ZMK_RUNTIME_TAPDANCE_TIMEOUT_MS_KEY, &setting,
        persist ? ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST : ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
}

static int migrate_legacy_config(uint32_t index, const struct zmk_runtime_tapdance_config *config) {
    if (find_entry(index)) {
        return 0;
    }
    char key[KEY_SIZE];
    char name[CONFIG_ZMK_RUNTIME_TAPDANCE_NAME_MAX_LEN + 1];
    int ret = make_key(index, key);
    if (ret < 0) {
        return ret;
    }
    snprintf(name, sizeof(name), "Tapdance %u", index);
    const struct zmk_runtime_tapdance_config empty = {0};
    uint8_t packed[PACKED_SIZE];
    size_t size;
    ret = encode_entry(name, &empty, packed, &size);
    if (ret < 0) {
        return ret;
    }
    struct zmk_custom_setting_value value = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES,
                                             .size = size};
    memcpy(value.bytes_value, packed, size);
    const struct zmk_custom_setting *entry;
    ret = zmk_custom_setting_keyspace_create(&runtime_tapdances, key, &value,
                                             ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY, &entry);
    if (ret < 0) {
        return ret;
    }
    ret = encode_entry(name, config, packed, &size);
    if (ret == 0) {
        ret = zmk_custom_setting_write_bytes(entry, packed, size,
                                             ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST);
    }
    if (ret < 0) {
        zmk_custom_setting_keyspace_delete(&runtime_tapdances, key);
    }
    return ret;
}

static int legacy_initialized(const zmk_event_t *event) {
    ARG_UNUSED(event);
    uint32_t highest_id_plus_one = 0;
    bool saw_legacy = false;
    /* Reserve all legacy IDs before importing any entry. If migration stops
     * partway through (for example, because the pool is full), Create must
     * never assign an ID that a keymap already uses for a legacy slot. */
    for (uint32_t index = 0; index < COUNT; index++) {
        struct zmk_custom_setting_value value;
        int ret = zmk_custom_setting_read_array_by_key(ZMK_RUNTIME_TAPDANCE_SUBSYSTEM_ID, "slots",
                                                       index, &value);
        if (ret == -ENOENT || (ret == 0 && value.size == 0)) {
            continue;
        }
        if (ret < 0 || value.type != ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES ||
            value.size != LEGACY_PACKED_SIZE || value.bytes_value[0] != 3) {
            LOG_ERR("Invalid legacy tap dance slot %u", index);
            return ZMK_EV_EVENT_BUBBLE;
        }
        if (sys_get_le32(value.bytes_value + 1) != 0) {
            highest_id_plus_one = index + 1;
        }
    }
    struct zmk_custom_setting_value next;
    int ret = zmk_custom_setting_read_by_key(ZMK_RUNTIME_TAPDANCE_SUBSYSTEM_ID,
                                             ZMK_RUNTIME_TAPDANCE_NEXT_ID_KEY, &next);
    if (ret == 0 && next.type == ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32 && next.int32_value >= 0 &&
        highest_id_plus_one > (uint32_t)next.int32_value) {
        next.int32_value = highest_id_plus_one;
        ret = zmk_custom_setting_write_by_key(ZMK_RUNTIME_TAPDANCE_SUBSYSTEM_ID,
                                              ZMK_RUNTIME_TAPDANCE_NEXT_ID_KEY, &next,
                                              ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST);
        if (ret < 0) {
            LOG_ERR("Could not reserve legacy tap dance IDs: %d", ret);
            return ZMK_EV_EVENT_BUBBLE;
        }
    }
    for (uint32_t index = 0; index < COUNT; index++) {
        struct zmk_custom_setting_value value;
        ret = zmk_custom_setting_read_array_by_key(ZMK_RUNTIME_TAPDANCE_SUBSYSTEM_ID, "slots",
                                                   index, &value);
        if (ret == -ENOENT || (ret == 0 && value.size == 0)) {
            continue;
        }
        if (ret < 0 || value.type != ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES ||
            value.size != LEGACY_PACKED_SIZE || value.bytes_value[0] != 3) {
            LOG_ERR("Invalid legacy tap dance slot %u", index);
            return ZMK_EV_EVENT_BUBBLE;
        }
        const uint8_t *p = value.bytes_value + 1;
        struct zmk_runtime_tapdance_config config = {
            .single_binding = {sys_get_le32(p), sys_get_le32(p + 4), sys_get_le32(p + 8)},
            .double_binding = {sys_get_le32(p + 12), sys_get_le32(p + 16), sys_get_le32(p + 20)},
            .triple_binding = {sys_get_le32(p + 24), sys_get_le32(p + 28), sys_get_le32(p + 32)},
        };
        if (validate(&config) < 0) {
            LOG_ERR("Invalid legacy tap dance binding in slot %u", index);
            return ZMK_EV_EVENT_BUBBLE;
        }
        if (!config.single_binding.behavior_id) {
            continue;
        }
        ret = migrate_legacy_config(index, &config);
        if (ret < 0) {
            LOG_ERR("Legacy tap dance migration failed at slot %u: %d", index, ret);
            return ZMK_EV_EVENT_BUBBLE;
        }
        saw_legacy = true;
        highest_id_plus_one = index + 1;
    }
    for (uint32_t i = 0; i < COUNT; i++) {
        if (!runtime_tapdances.slots[i].in_use) {
            continue;
        }
        uint32_t id;
        if (id_from_entry(&runtime_tapdances.slots[i].setting, &id) == 0 &&
            id + 1 > highest_id_plus_one) {
            highest_id_plus_one = id + 1;
        }
    }
    ret = zmk_custom_setting_read_by_key(ZMK_RUNTIME_TAPDANCE_SUBSYSTEM_ID,
                                         ZMK_RUNTIME_TAPDANCE_NEXT_ID_KEY, &next);
    if (ret == 0 && next.type == ZMK_CUSTOM_SETTING_VALUE_TYPE_INT32 && next.int32_value >= 0 &&
        highest_id_plus_one > (uint32_t)next.int32_value) {
        next.int32_value = highest_id_plus_one;
        ret = zmk_custom_setting_write_by_key(ZMK_RUNTIME_TAPDANCE_SUBSYSTEM_ID,
                                              ZMK_RUNTIME_TAPDANCE_NEXT_ID_KEY, &next,
                                              ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST);
        if (ret < 0) {
            LOG_ERR("Could not advance tap dance ID counter: %d", ret);
            return ZMK_EV_EVENT_BUBBLE;
        }
    }
    if (saw_legacy) {
        const struct zmk_custom_setting *slots =
            zmk_custom_setting_find(ZMK_RUNTIME_TAPDANCE_SUBSYSTEM_ID, "slots");
        if (slots) {
            ret = zmk_custom_setting_reset(slots);
            if (ret < 0) {
                LOG_ERR("Could not erase migrated tap dance slots: %d", ret);
            }
        }
    }
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(runtime_tapdance_legacy_migration, legacy_initialized);
ZMK_SUBSCRIPTION(runtime_tapdance_legacy_migration, zmk_custom_settings_initialized);

#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

struct sequence {
    bool active, pressed, decided, action_pressed;
    uint8_t taps;
    uint32_t slot, interval;
    int64_t deadline;
    struct zmk_behavior_binding_event event;
    struct zmk_runtime_tapdance_config config;
    struct zmk_custom_setting_behavior_value action;
};
static struct sequence sequences[CONFIG_ZMK_RUNTIME_TAPDANCE_MAX_ACTIVE];
static void timer_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(timer, timer_handler);

static bool same_key(const struct sequence *seq, uint32_t position, uint8_t source) {
    if (!seq->active || seq->event.position != position) {
        return false;
    }
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    return seq->event.source == source;
#else
    ARG_UNUSED(source);
    return true;
#endif
}

static struct sequence *find_key(uint32_t position, uint8_t source) {
    for (size_t i = 0; i < ARRAY_SIZE(sequences); i++) {
        if (same_key(&sequences[i], position, source)) {
            return &sequences[i];
        }
    }
    return NULL;
}

static int invoke(struct sequence *seq, bool pressed, int64_t timestamp) {
    struct zmk_behavior_binding binding;
    int ret = binding_from_value(&seq->action, &binding);
    if (ret < 0) {
        return ret;
    }
    struct zmk_behavior_binding_event event = seq->event;
    event.timestamp = timestamp;
    return zmk_behavior_invoke_binding(&binding, event, pressed);
}

static void finish(struct sequence *seq, int64_t timestamp, bool held) {
    seq->decided = true;
    seq->deadline = INT64_MAX;
    seq->action = held && seq->config.hold_bindings[seq->taps - 1].behavior_id
                      ? seq->config.hold_bindings[seq->taps - 1]
                      : *tap_binding(&seq->config, seq->taps - 1);
    int ret = invoke(seq, true, timestamp);
    if (ret < 0) {
        LOG_WRN("Runtime tap dance action rejected: %d", ret);
        seq->active = false;
        return;
    }
    seq->action_pressed = true;
    if (!seq->pressed) {
        invoke(seq, false, timestamp);
        seq->active = false;
    }
}

static void expire(int64_t timestamp) {
    for (size_t i = 0; i < ARRAY_SIZE(sequences); i++) {
        struct sequence *seq = &sequences[i];
        if (seq->active && !seq->decided && seq->deadline <= timestamp) {
            finish(seq, seq->deadline, seq->pressed);
        }
    }
}

static void schedule(void) {
    int64_t next = INT64_MAX;
    for (size_t i = 0; i < ARRAY_SIZE(sequences); i++) {
        if (sequences[i].active && !sequences[i].decided) {
            next = MIN(next, sequences[i].deadline);
        }
    }
    if (next == INT64_MAX) {
        k_work_cancel_delayable(&timer);
    } else {
        k_work_reschedule(&timer, K_MSEC(MAX(0, next - k_uptime_get())));
    }
}

static void release(struct sequence *seq, int64_t timestamp) {
    if (!seq || !seq->pressed) {
        return;
    }
    seq->pressed = false;
    uint8_t max_taps = seq->config.triple_binding.behavior_id   ? 3
                       : seq->config.double_binding.behavior_id ? 2
                                                                : 1;
    if (!seq->decided && seq->taps == max_taps) {
        finish(seq, timestamp, false);
        return;
    }
    if (seq->decided) {
        if (seq->action_pressed) {
            invoke(seq, false, timestamp);
        }
        seq->active = false;
    }
}

int zmk_runtime_tapdance_pressed(uint32_t slot, struct zmk_behavior_binding_event event) {
    k_mutex_lock(&tapdance_lock, K_FOREVER);
    expire(event.timestamp);
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    uint8_t source = event.source;
#else
    uint8_t source = 0;
#endif
    struct sequence *seq = find_key(event.position, source);
    if (seq && seq->slot != slot) {
        if (!seq->decided) {
            finish(seq, event.timestamp, seq->pressed);
        }
        if (seq->action_pressed && seq->active) {
            invoke(seq, false, event.timestamp);
        }
        seq->active = false;
        seq = NULL;
    }
    if (!seq) {
        struct zmk_runtime_tapdance_config config;
        if (read_raw(slot, &config) < 0 || !config.single_binding.behavior_id) {
            goto out;
        }
        for (size_t i = 0; i < ARRAY_SIZE(sequences); i++) {
            if (!sequences[i].active) {
                seq = &sequences[i];
                break;
            }
        }
        if (!seq) {
            LOG_WRN("Too many active runtime tap dances");
            goto out;
        }
        *seq = (struct sequence){.active = true,
                                 .slot = slot,
                                 .interval = zmk_runtime_tapdance_timeout_ms(),
                                 .event = event,
                                 .config = config};
    }
    if (!seq->pressed && !seq->decided) {
        seq->pressed = true;
        seq->taps++;
        uint16_t interval = seq->config.tap_intervals_ms[seq->taps - 1];
        if (!interval)
            interval = seq->config.interval_ms;
        seq->deadline = event.timestamp + (interval ? interval : seq->interval);
        uint8_t max_taps = seq->config.triple_binding.behavior_id   ? 3
                           : seq->config.double_binding.behavior_id ? 2
                                                                    : 1;
        if (seq->taps == max_taps && !seq->config.hold_bindings[seq->taps - 1].behavior_id) {
            finish(seq, event.timestamp, false);
        }
    }
out:
    schedule();
    k_mutex_unlock(&tapdance_lock);
    return ZMK_BEHAVIOR_OPAQUE;
}

int zmk_runtime_tapdance_released(struct zmk_behavior_binding_event event) {
    k_mutex_lock(&tapdance_lock, K_FOREVER);
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    release(find_key(event.position, event.source), event.timestamp);
#else
    release(find_key(event.position, 0), event.timestamp);
#endif
    schedule();
    k_mutex_unlock(&tapdance_lock);
    return ZMK_BEHAVIOR_OPAQUE;
}

static void timer_handler(struct k_work *work) {
    ARG_UNUSED(work);
    k_mutex_lock(&tapdance_lock, K_FOREVER);
    expire(k_uptime_get());
    schedule();
    k_mutex_unlock(&tapdance_lock);
}

static int listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (!ev) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    k_mutex_lock(&tapdance_lock, K_FOREVER);
    expire(ev->timestamp);
    if (ev->state) {
        for (size_t i = 0; i < ARRAY_SIZE(sequences); i++) {
            struct sequence *seq = &sequences[i];
            if (seq->active && !seq->decided && !same_key(seq, ev->position, ev->source)) {
                finish(seq, ev->timestamp, seq->pressed);
            }
        }
    } else {
        release(find_key(ev->position, ev->source), ev->timestamp);
    }
    schedule();
    k_mutex_unlock(&tapdance_lock);
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(runtime_tapdance, listener);
ZMK_SUBSCRIPTION(runtime_tapdance, zmk_position_state_changed);

#endif
