/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/matrix.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <cormoran/runtime_tapdance/runtime_tapdance.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define COUNT CONFIG_ZMK_RUNTIME_TAPDANCE_MAX_TAPDANCES
#define PACKED_SIZE 30
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
    *binding = (struct zmk_behavior_binding){
        .behavior_dev = name, .param1 = value->param1, .param2 = value->param2};
    return zmk_behavior_validate_binding(binding);
}
static int validate(const struct zmk_runtime_tapdance_config *config) {
    if (config->position >= ZMK_KEYMAP_LEN) {
        return -EINVAL;
    }
    struct zmk_behavior_binding binding;
    int ret = binding_from_value(&config->double_binding, &binding);
    return ret < 0 ? ret : binding_from_value(&config->triple_binding, &binding);
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
    if (value.size != PACKED_SIZE || p[0] != 1 || (p[1] & ~3)) {
        return -EINVAL;
    }
    config->enabled = p[1] & 1;
    config->delay_single = p[1] & 2;
    config->position = sys_get_le32(p + 2);
    config->double_binding = (struct zmk_custom_setting_behavior_value){
        sys_get_le32(p + 6), sys_get_le32(p + 10), sys_get_le32(p + 14)};
    config->triple_binding = (struct zmk_custom_setting_behavior_value){
        sys_get_le32(p + 18), sys_get_le32(p + 22), sys_get_le32(p + 26)};
    return validate(config);
}
int zmk_runtime_tapdance_read(uint32_t index, struct zmk_runtime_tapdance_config *config) {
    k_mutex_lock(&tapdance_lock, K_FOREVER);
    int ret = read_raw(index, config);
    if (ret == 0 && config->enabled) {
        for (uint32_t i = 0; i < COUNT; i++) {
            struct zmk_runtime_tapdance_config other;
            if (i != index && read_raw(i, &other) == 0 && other.enabled &&
                other.position == config->position) {
                ret = -EEXIST;
                break;
            }
        }
    }
    k_mutex_unlock(&tapdance_lock);
    return ret;
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
    for (uint32_t i = 0; i < COUNT && config->enabled; i++) {
        struct zmk_runtime_tapdance_config other;
        if (i != index && read_raw(i, &other) == 0 && other.enabled &&
            other.position == config->position) {
            ret = -EEXIST;
            goto out;
        }
    }
    struct zmk_custom_setting_value value = {.type = ZMK_CUSTOM_SETTING_VALUE_TYPE_BYTES,
                                             .size = PACKED_SIZE};
    uint8_t *p = value.bytes_value;
    p[0] = 1;
    p[1] = config->enabled | (config->delay_single << 1);
    sys_put_le32(config->position, p + 2);
    sys_put_le32(config->double_binding.behavior_id, p + 6);
    sys_put_le32(config->double_binding.param1, p + 10);
    sys_put_le32(config->double_binding.param2, p + 14);
    sys_put_le32(config->triple_binding.behavior_id, p + 18);
    sys_put_le32(config->triple_binding.param1, p + 22);
    sys_put_le32(config->triple_binding.param2, p + 26);
    const struct zmk_custom_setting *slot = zmk_custom_setting_find_array_element(
        ZMK_RUNTIME_TAPDANCE_SUBSYSTEM_ID, ZMK_RUNTIME_TAPDANCE_SLOTS_KEY, index);
    if (!slot) {
        ret = -ENOENT;
        goto out;
    }
    ret = zmk_custom_setting_write_array_element(
        slot, &value, MAX(zmk_custom_setting_array_size(slot), index + 1),
        persist ? ZMK_CUSTOM_SETTING_WRITE_MODE_PERSIST : ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY);
out:
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
    bool active, held, normal, matched;
    uint8_t taps, captured_count;
    uint32_t interval;
    int64_t deadline;
    struct zmk_runtime_tapdance_config config;
    struct zmk_custom_setting_behavior_value binding;
    struct zmk_position_state_changed last;
    struct zmk_position_state_changed_event captured[6];
};
static struct sequence sequences[COUNT];
static void timer_handler(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(timer, timer_handler);
static int listener(const zmk_event_t *eh);
ZMK_LISTENER(runtime_tapdance, listener);
static void replay(struct sequence *seq) {
    for (uint8_t i = 0; i < seq->captured_count; i++) {
        struct zmk_position_state_changed_event ev = seq->captured[i];
        ZMK_EVENT_RAISE_AFTER(ev, runtime_tapdance);
    }
    seq->captured_count = 0;
}
static int invoke(struct sequence *seq, bool pressed, int64_t timestamp) {
    struct zmk_behavior_binding binding;
    int ret = binding_from_value(&seq->binding, &binding);
    if (ret < 0) {
        return ret;
    }
    struct zmk_behavior_binding_event event = {
        .position = ZMK_RUNTIME_TAPDANCE_ACTION_POSITION((uint32_t)(seq - sequences)),
        .timestamp = timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = seq->last.source,
#endif
    };
    return zmk_behavior_invoke_binding(&binding, event, pressed);
}
static void finish(struct sequence *seq, int64_t now) {
    /* Invoked behaviors/replayed events can raise events synchronously. Mark
     * this deadline consumed before entering any downstream event callback. */
    seq->deadline = INT64_MAX;
    seq->binding = seq->taps == 3 ? seq->config.triple_binding : seq->config.double_binding;
    int ret = 0;
    bool has_action = seq->taps > 1 && seq->binding.behavior_id;
    if (has_action) {
        ret = invoke(seq, true, now);
        if (ret < 0) {
            LOG_WRN("Runtime tap dance action rejected: %d", ret);
        }
    }
    if (has_action && ret >= 0) {
        seq->matched = true;
        seq->captured_count = 0;
        if (!seq->held) {
            invoke(seq, false, now);
            seq->active = false;
        }
    } else {
        if (seq->config.delay_single) {
            replay(seq);
        }
        seq->normal = seq->held;
        seq->active = seq->held;
    }
}
static void expire(int64_t now) {
    for (;;) {
        struct sequence *next = NULL;
        for (uint32_t i = 0; i < COUNT; i++) {
            if (sequences[i].active && sequences[i].deadline <= now &&
                (!next || sequences[i].deadline < next->deadline)) {
                next = &sequences[i];
            }
        }
        if (!next) {
            return;
        }
        finish(next, next->deadline);
    }
}
static void schedule(void) {
    int64_t next = INT64_MAX;
    for (uint32_t i = 0; i < COUNT; i++) {
        if (sequences[i].active) {
            next = MIN(next, sequences[i].deadline);
        }
    }
    if (next == INT64_MAX) {
        k_work_cancel_delayable(&timer);
    } else {
        k_work_reschedule(&timer, K_MSEC(MAX(0, next - k_uptime_get())));
    }
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
    struct sequence *seq = NULL;
    for (uint32_t i = 0; i < COUNT; i++) {
        if (sequences[i].active && sequences[i].config.position == ev->position &&
            sequences[i].last.source == ev->source) {
            seq = &sequences[i];
            break;
        }
    }
    if (!seq && ev->state) {
        for (uint32_t i = 0; i < COUNT; i++) {
            struct zmk_runtime_tapdance_config config;
            if (!sequences[i].active && zmk_runtime_tapdance_read(i, &config) == 0 &&
                config.enabled && config.position == ev->position) {
                seq = &sequences[i];
                *seq = (struct sequence){.active = true,
                                         .config = config,
                                         .interval = zmk_runtime_tapdance_timeout_ms()};
                break;
            }
        }
    }
    int result = ZMK_EV_EVENT_BUBBLE;
    if (seq) {
        seq->last = *ev;
        if (seq->normal || seq->matched) {
            if (seq->matched) {
                if (!ev->state) {
                    invoke(seq, false, ev->timestamp);
                }
                if (seq->config.delay_single) {
                    result = ZMK_EV_EVENT_HANDLED;
                }
            }
            if (!ev->state) {
                seq->active = false;
            }
        } else if (ev->state != seq->held) {
            seq->held = ev->state;
            if (seq->config.delay_single) {
                seq->captured[seq->captured_count++] = copy_raised_zmk_position_state_changed(ev);
                result = ZMK_EV_EVENT_HANDLED;
            }
            if (ev->state) {
                seq->taps++;
                seq->deadline = ev->timestamp + seq->interval;
                if (seq->taps == 3) {
                    finish(seq, ev->timestamp);
                }
            }
        } else if (seq->config.delay_single) {
            result = ZMK_EV_EVENT_HANDLED;
        }
    }
    schedule();
    k_mutex_unlock(&tapdance_lock);
    return result;
}
ZMK_SUBSCRIPTION(runtime_tapdance, zmk_position_state_changed);
#endif
