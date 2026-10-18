/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <limits.h>
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
#define PACKED_SIZE 37
BUILD_ASSERT(PACKED_SIZE <= CONFIG_ZMK_CUSTOM_SETTINGS_VALUE_MAX_SIZE);
#define SLOT_DEFAULT(i, _) {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES, .size = 0}
static const struct zmk_custom_setting_value defaults[COUNT] = {LISTIFY(COUNT, SLOT_DEFAULT, (, ))};
ZMK_CUSTOM_SETTING_ARRAY_DEFINE(runtime_tapdance_slots, ZMK_RUNTIME_TAPDANCE_SUBSYSTEM_ID,
                                ZMK_RUNTIME_TAPDANCE_SLOTS_KEY, ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES,
                                COUNT, COUNT, defaults,
                                ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                                ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                                ZMK_CUSTOM_SETTING_PERMISSION_SECURE,
                                ZMK_CUSTOM_SETTING_NO_CONSTRAINT);
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
    if ((!config->single_binding.behavior_id &&
         (config->double_binding.behavior_id || config->triple_binding.behavior_id)) ||
        (config->triple_binding.behavior_id && !config->double_binding.behavior_id)) {
        return -EINVAL;
    }
    return 0;
}

static int read_raw(uint32_t index, struct zmk_runtime_tapdance_config *config) {
    if (!config || index >= COUNT) {
        return -EINVAL;
    }
    struct zmk_custom_setting_value value;
    int ret = zmk_custom_setting_read_array_by_key(ZMK_RUNTIME_TAPDANCE_SUBSYSTEM_ID,
                                                   ZMK_RUNTIME_TAPDANCE_SLOTS_KEY, index, &value);
    *config = (struct zmk_runtime_tapdance_config){0};
    if (ret == -ENOENT) {
        return 0;
    }
    if (ret < 0) {
        return ret;
    }
    if (value.type != ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES) {
        return -EINVAL;
    }
    if (!value.size) {
        return 0;
    }
    const uint8_t *p = value.bytes_value;
    if (value.size != PACKED_SIZE || p[0] != 3) {
        return -EINVAL;
    }
    config->single_binding = (struct zmk_custom_setting_behavior_value){
        sys_get_le32(p + 1), sys_get_le32(p + 5), sys_get_le32(p + 9)};
    config->double_binding = (struct zmk_custom_setting_behavior_value){
        sys_get_le32(p + 13), sys_get_le32(p + 17), sys_get_le32(p + 21)};
    config->triple_binding = (struct zmk_custom_setting_behavior_value){
        sys_get_le32(p + 25), sys_get_le32(p + 29), sys_get_le32(p + 33)};
    return validate(config);
}

int zmk_runtime_tapdance_read(uint32_t index, struct zmk_runtime_tapdance_config *config) {
    k_mutex_lock(&tapdance_lock, K_FOREVER);
    int ret = read_raw(index, config);
    k_mutex_unlock(&tapdance_lock);
    return ret;
}

static int write_raw_locked(uint32_t index, const struct zmk_runtime_tapdance_config *config,
                            bool persist) {
    struct zmk_custom_setting_value value = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES,
                                             .size = PACKED_SIZE};
    uint8_t *p = value.bytes_value;
    p[0] = 3;
    sys_put_le32(config->single_binding.behavior_id, p + 1);
    sys_put_le32(config->single_binding.param1, p + 5);
    sys_put_le32(config->single_binding.param2, p + 9);
    sys_put_le32(config->double_binding.behavior_id, p + 13);
    sys_put_le32(config->double_binding.param1, p + 17);
    sys_put_le32(config->double_binding.param2, p + 21);
    sys_put_le32(config->triple_binding.behavior_id, p + 25);
    sys_put_le32(config->triple_binding.param1, p + 29);
    sys_put_le32(config->triple_binding.param2, p + 33);
    const struct zmk_custom_setting *slot = zmk_custom_setting_find_array_element(
        ZMK_RUNTIME_TAPDANCE_SUBSYSTEM_ID, ZMK_RUNTIME_TAPDANCE_SLOTS_KEY, index);
    return slot ? zmk_custom_setting_write_array_element(
                      slot, &value, MAX(zmk_custom_setting_array_size(slot), index + 1),
                      persist ? ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST
                              : ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY)
                : -ENOENT;
}

int zmk_runtime_tapdance_write(uint32_t index, const struct zmk_runtime_tapdance_config *config,
                               bool persist) {
    if (!config || index >= COUNT) {
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

int zmk_runtime_tapdance_set_binding(uint32_t index, uint32_t tap_count,
                                     const struct zmk_custom_setting_behavior_value *binding,
                                     bool persist) {
    if (!binding || index >= COUNT || tap_count < 1 || tap_count > 3) {
        return -EINVAL;
    }
    k_mutex_lock(&tapdance_lock, K_FOREVER);
    struct zmk_runtime_tapdance_config config;
    int ret = read_raw(index, &config);
    if (ret == 0) {
        struct zmk_custom_setting_behavior_value *selected = tap_count == 1 ? &config.single_binding
                                                             : tap_count == 2
                                                                 ? &config.double_binding
                                                                 : &config.triple_binding;
        *selected = *binding;
        ret = validate(&config);
        if (ret == 0) {
            ret = write_raw_locked(index, &config, persist);
        }
    }
    k_mutex_unlock(&tapdance_lock);
    return ret;
}

int zmk_runtime_tapdance_delete(uint32_t index, bool persist) {
    const struct zmk_runtime_tapdance_config empty = {0};
    return zmk_runtime_tapdance_write(index, &empty, persist);
}

uint32_t zmk_runtime_tapdance_max_count(void) { return COUNT; }

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

static void finish(struct sequence *seq, int64_t timestamp) {
    seq->decided = true;
    seq->deadline = INT64_MAX;
    seq->action = seq->taps == 3   ? seq->config.triple_binding
                  : seq->taps == 2 ? seq->config.double_binding
                                   : seq->config.single_binding;
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
            finish(seq, seq->deadline);
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
            finish(seq, event.timestamp);
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
        seq->deadline = event.timestamp + seq->interval;
        uint8_t max_taps = seq->config.triple_binding.behavior_id   ? 3
                           : seq->config.double_binding.behavior_id ? 2
                                                                    : 1;
        if (seq->taps == max_taps) {
            finish(seq, event.timestamp);
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
                finish(seq, ev->timestamp);
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
