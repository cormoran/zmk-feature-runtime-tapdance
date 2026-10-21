/* SPDX-License-Identifier: MIT */
/* Exercise the public keymap binding and observe the resulting HID events. */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/__assert.h>
#include <drivers/behavior.h>
#include <dt-bindings/zmk/keys.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <cormoran/runtime_tapdance/runtime_tapdance.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static int rejected_releases;
static int reject_press(struct zmk_behavior_binding *binding,
                        struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);
    return -EBUSY;
}
static int reject_release(struct zmk_behavior_binding *binding,
                          struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);
    rejected_releases++;
    return 0;
}
static const struct behavior_parameter_metadata reject_metadata = {0};
static const struct behavior_driver_api reject_api = {
    .binding_pressed = reject_press,
    .binding_released = reject_release,
    .parameter_metadata = &reject_metadata,
};
BEHAVIOR_DT_DEFINE(DT_NODELABEL(tapdance_test_reject), NULL, NULL, NULL, NULL, POST_KERNEL,
                   CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &reject_api);

static uint32_t observed[64];
static size_t observed_count;
static bool collecting;
static int64_t timestamp;

static int observe(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (collecting && ev && ev->keycode != (D & 0xFFFF)) {
        __ASSERT(observed_count < ARRAY_SIZE(observed), "observation overflow");
        observed[observed_count++] = ev->keycode | (ev->state ? BIT(16) : 0);
    }
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(runtime_tapdance_test_observer, observe);
ZMK_SUBSCRIPTION(runtime_tapdance_test_observer, zmk_keycode_state_changed);

static void position(uint32_t pos, bool state, int delta) {
    timestamp += delta;
    raise_zmk_position_state_changed(
        (struct zmk_position_state_changed){.position = pos,
                                            .state = state,
                                            .timestamp = timestamp,
                                            .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL});
}
static void interrupt(void) {
    position(3, true, 1);
    position(3, false, 1);
}

#define DOWN(key) (((key) & 0xFFFF) | BIT(16))
#define UP(key) ((key) & 0xFFFF)
#define EXPECT(...)                                                                                \
    do {                                                                                           \
        const uint32_t expected[] = {__VA_ARGS__};                                                 \
        __ASSERT(observed_count == ARRAY_SIZE(expected), "keycode count %u vs %u",                 \
                 (unsigned)observed_count, (unsigned)ARRAY_SIZE(expected));                        \
        for (size_t i = 0; i < observed_count; i++) {                                              \
            __ASSERT(observed[i] == expected[i], "keycode %u: %x vs %x", (unsigned)i, observed[i], \
                     expected[i]);                                                                 \
        }                                                                                          \
        observed_count = 0;                                                                        \
    } while (0)

static void verify_timer(struct k_work *work) {
    ARG_UNUSED(work);
    EXPECT(DOWN(A), UP(A));
    uint32_t id;
    __ASSERT(zmk_runtime_tapdance_create("Second", false, &id) == -EEXIST,
             "duplicate name rejected");
    __ASSERT(zmk_runtime_tapdance_rename(1, "First", false) == -EEXIST,
             "duplicate rename rejected");
    __ASSERT(zmk_runtime_tapdance_rename(1, "Renamed", false) == 0, "rename keeps ID");
    char name[CONFIG_ZMK_RUNTIME_TAPDANCE_NAME_MAX_LEN + 1];
    __ASSERT(zmk_runtime_tapdance_name(1, name, sizeof(name), NULL) == 0 &&
                 strcmp(name, "Renamed") == 0,
             "renamed entry read by same ID");
    __ASSERT(zmk_runtime_tapdance_delete(0, false) == 0, "delete entry");
    __ASSERT(zmk_runtime_tapdance_read(0, &(struct zmk_runtime_tapdance_config){0}) == -ENOENT,
             "deleted ID is inert");
    __ASSERT(zmk_runtime_tapdance_create("Replacement", false, &id) == 0 && id == 2,
             "deleted ID was not reused");
    for (uint32_t i = 3; i <= 16; i++) {
        snprintf(name, sizeof(name), "Extra %u", i);
        __ASSERT(zmk_runtime_tapdance_create(name, false, &id) == 0 && id == i,
                 "shared entry capacity");
    }
    __ASSERT(zmk_runtime_tapdance_used_count() == 16, "all entries allocated");
    __ASSERT(zmk_runtime_tapdance_create("Overflow", false, &id) == -ENOSPC,
             "entry capacity enforced");
    __ASSERT(zmk_runtime_tapdance_delete(2, false) == 0, "free pool entry");
    __ASSERT(zmk_runtime_tapdance_create("After free", false, &id) == 0 && id > 16,
             "pool and entry slot recovered without ID reuse");
    for (uint32_t old_id = 1; old_id <= id; old_id++) {
        struct zmk_runtime_tapdance_config existing;
        if (zmk_runtime_tapdance_read(old_id, &existing) == 0) {
            __ASSERT(zmk_runtime_tapdance_delete(old_id, false) == 0, "clear entry for pool test");
        }
    }
    __ASSERT(zmk_runtime_tapdance_pool_used() == 0, "delete returns pooled bytes");
    char long_name[CONFIG_ZMK_RUNTIME_TAPDANCE_NAME_MAX_LEN + 1];
    memset(long_name, 'x', CONFIG_ZMK_RUNTIME_TAPDANCE_NAME_MAX_LEN);
    long_name[CONFIG_ZMK_RUNTIME_TAPDANCE_NAME_MAX_LEN] = '\0';
    uint32_t first_long_id = 0, long_count = 0;
    const uint32_t kp = zmk_behavior_get_local_id("key_press");
    struct zmk_runtime_tapdance_config full = {
        .single_binding = {kp, A, 0},
        .double_binding = {kp, B, 0},
        .triple_binding = {kp, C, 0},
        .hold_bindings = {{kp, G, 0}, {kp, E, 0}, {kp, F, 0}},
        .interval_ms = 150,
        .tap_intervals_ms = {20, 40, 60},
    };
    for (uint32_t i = 0; i < 16; i++) {
        long_name[CONFIG_ZMK_RUNTIME_TAPDANCE_NAME_MAX_LEN - 2] = '0' + i / 10;
        long_name[CONFIG_ZMK_RUNTIME_TAPDANCE_NAME_MAX_LEN - 1] = '0' + i % 10;
        int ret = zmk_runtime_tapdance_create(long_name, false, &id);
        if (ret == -ENOSPC) {
            break;
        }
        __ASSERT(ret == 0, "long entry creation: %d", ret);
        ret = zmk_runtime_tapdance_write(id, &full, false);
        if (ret == -ENOSPC) {
            __ASSERT(zmk_runtime_tapdance_delete(id, false) == 0,
                     "failed full entry can be removed");
            break;
        }
        __ASSERT(ret == 0, "full entry write: %d", ret);
        if (long_count == 0) {
            first_long_id = id;
        }
        long_count++;
    }
    __ASSERT(long_count > 0 && long_count < 16, "shared byte pool exhausted before entry limit");
    __ASSERT(zmk_runtime_tapdance_delete(first_long_id, false) == 0, "free pooled bytes");
    __ASSERT(zmk_runtime_tapdance_create("Recovered", false, &id) == 0,
             "pooled bytes reused after deletion");
    collecting = false;
    LOG_INF("PASS: runtime_tapdance_public_events");
}
K_WORK_DELAYABLE_DEFINE(timer_check_work, verify_timer);

static void run(struct k_work *work) {
    ARG_UNUSED(work);
    uint32_t kp = zmk_behavior_get_local_id("key_press");
    __ASSERT(kp && kp != UINT16_MAX, "key_press local id missing");
    struct zmk_runtime_tapdance_config cfg = {
        .single_binding = {kp, A, 0},
        .double_binding = {kp, B, 0},
        .triple_binding = {kp, C, 0},
    };
    uint32_t first_id, second_id;
    int create_rc = zmk_runtime_tapdance_create("First", false, &first_id);
    __ASSERT(create_rc == 0 && first_id == 0, "first allocated ID: %d", create_rc);
    __ASSERT(zmk_runtime_tapdance_create("Second", false, &second_id) == 0 && second_id == 1,
             "second allocated ID");
    __ASSERT(zmk_runtime_tapdance_write(0, &cfg, false) == 0, "config write");
    __ASSERT(zmk_runtime_tapdance_set_timeout_ms(100, false) == 0, "interval");
    __ASSERT(zmk_runtime_tapdance_set_timeout_ms(0, false) == -EINVAL, "invalid interval");
    struct zmk_runtime_tapdance_config bad = cfg;
    bad.single_binding.behavior_id = 0;
    __ASSERT(zmk_runtime_tapdance_write(0, &bad, false) == -EINVAL, "single required");
    bad = cfg;
    bad.double_binding.behavior_id = 0;
    __ASSERT(zmk_runtime_tapdance_write(0, &bad, false) == -EINVAL, "triple needs double");
    bad = cfg;
    bad.double_binding.behavior_id = UINT32_MAX;
    __ASSERT(zmk_runtime_tapdance_write(0, &bad, false) == -ENODEV, "invalid behavior id");
    bad = cfg;
    bad.single_binding.param2 = 1;
    __ASSERT(zmk_runtime_tapdance_write(0, &bad, false) < 0, "invalid behavior param");
    bad = cfg;
    bad.single_binding.behavior_id = zmk_behavior_get_local_id("runtime_tapdance");
    __ASSERT(zmk_runtime_tapdance_write(0, &bad, false) == -ELOOP, "recursive dance");
    const struct zmk_custom_setting_behavior_value empty = {0};
    const struct zmk_custom_setting_behavior_value alternate = {kp, D, 0};
    struct zmk_runtime_tapdance_config updated;
    __ASSERT(zmk_runtime_tapdance_set_binding(0, 0, &alternate, false) == -EINVAL,
             "invalid tap count");
    __ASSERT(zmk_runtime_tapdance_set_binding(0, 2, &empty, false) == -EINVAL,
             "double required by triple");
    __ASSERT(zmk_runtime_tapdance_set_binding(0, 2, &alternate, false) == 0, "edit one binding");
    __ASSERT(zmk_runtime_tapdance_read(0, &updated) == 0 && updated.single_binding.param1 == A &&
                 updated.double_binding.param1 == D && updated.triple_binding.param1 == C,
             "other bindings preserved");
    __ASSERT(zmk_runtime_tapdance_set_binding(0, 2, &cfg.double_binding, false) == 0,
             "restore double");
    struct zmk_runtime_tapdance_config same = cfg;
    same.single_binding.param1 = E;
    same.double_binding.param1 = F;
    same.triple_binding = (struct zmk_custom_setting_behavior_value){0};
    __ASSERT(zmk_runtime_tapdance_write(1, &same, false) == 0, "second slot");

    timestamp = k_uptime_get() + 1000;
    collecting = true;
    position(0, true, 1);
    position(0, false, 10);
    __ASSERT(observed_count == 0, "single before decision");
    interrupt();
    EXPECT(DOWN(A), UP(A));

    position(0, true, 1);
    position(0, false, 10);
    position(0, true, 10);
    position(0, false, 10);
    __ASSERT(observed_count == 0, "double before triple deadline");
    interrupt();
    EXPECT(DOWN(B), UP(B));

    position(0, true, 1);
    position(0, false, 10);
    position(0, true, 10);
    position(0, false, 10);
    position(0, true, 10);
    EXPECT(DOWN(C));
    position(0, false, 10);
    EXPECT(UP(C));

    position(0, true, 1);
    interrupt();
    EXPECT(DOWN(A));
    position(0, false, 10);
    EXPECT(UP(A));

    position(0, true, 1);
    position(0, false, 10);
    position(0, true, 90);
    position(0, false, 10);
    interrupt();
    EXPECT(DOWN(A), UP(A), DOWN(A), UP(A));

    position(0, true, 1);
    position(0, false, 10);
    cfg.double_binding.param1 = C;
    __ASSERT(zmk_runtime_tapdance_write(0, &cfg, false) == 0, "edit");
    position(0, true, 10);
    position(0, false, 10);
    interrupt();
    EXPECT(DOWN(B), UP(B));

    position(0, true, 1);
    position(0, false, 10);
    position(1, true, 1);
    EXPECT(DOWN(A), UP(A));
    position(1, false, 10);
    interrupt();
    EXPECT(DOWN(A), UP(A));

    position(2, true, 1);
    position(2, false, 10);
    interrupt();
    EXPECT(DOWN(E), UP(E));

    cfg.double_binding.param1 = B;
    cfg.hold_bindings[0] = (struct zmk_custom_setting_behavior_value){kp, G, 0};
    cfg.hold_bindings[1] = (struct zmk_custom_setting_behavior_value){kp, E, 0};
    cfg.hold_bindings[2] = (struct zmk_custom_setting_behavior_value){kp, F, 0};
    cfg.interval_ms = 150;
    cfg.tap_intervals_ms[0] = 20;
    cfg.tap_intervals_ms[1] = 40;
    cfg.tap_intervals_ms[2] = 60;
    size_t old_size = zmk_runtime_tapdance_pool_used();
    __ASSERT(zmk_runtime_tapdance_write(0, &cfg, false) == 0, "hold and timing config");
    __ASSERT(zmk_runtime_tapdance_pool_used() == old_size + 44,
             "only configured hold and timing fields use pool bytes");
    __ASSERT(zmk_runtime_tapdance_read(0, &updated) == 0 && updated.hold_bindings[2].param1 == F &&
                 updated.tap_intervals_ms[0] == 20 && updated.interval_ms == 150,
             "round trip hold and timing");
    __ASSERT(zmk_runtime_tapdance_set_interval(0, 2, 2001, false) == -EINVAL,
             "reject out-of-range interval");
    position(0, true, 1);
    interrupt();
    EXPECT(DOWN(G));
    position(0, false, 10);
    EXPECT(UP(G));
    position(0, true, 1);
    position(0, false, 10);
    position(0, true, 5);
    interrupt();
    EXPECT(DOWN(E));
    position(0, false, 10);
    EXPECT(UP(E));
    position(0, true, 1);
    position(0, false, 10);
    position(0, true, 5);
    position(0, false, 10);
    position(0, true, 10);
    EXPECT();
    position(0, false, 10);
    EXPECT(DOWN(C), UP(C));
    position(0, true, 1);
    position(0, false, 10);
    position(0, true, 5);
    position(0, false, 10);
    position(0, true, 10);
    interrupt();
    EXPECT(DOWN(F));
    position(0, false, 10);
    EXPECT(UP(F));
    cfg.hold_bindings[0] = (struct zmk_custom_setting_behavior_value){0};
    cfg.hold_bindings[1] = (struct zmk_custom_setting_behavior_value){0};
    cfg.hold_bindings[2] = (struct zmk_custom_setting_behavior_value){0};
    cfg.interval_ms = 0;
    memset(cfg.tap_intervals_ms, 0, sizeof(cfg.tap_intervals_ms));
    __ASSERT(zmk_runtime_tapdance_write(0, &cfg, false) == 0, "clear overrides");
    __ASSERT(zmk_runtime_tapdance_pool_used() == old_size, "clearing overrides returns pool bytes");

    cfg.double_binding = (struct zmk_custom_setting_behavior_value){
        zmk_behavior_get_local_id("tapdance_test_reject"), 0, 0};
    cfg.triple_binding = (struct zmk_custom_setting_behavior_value){0};
    __ASSERT(zmk_runtime_tapdance_write(0, &cfg, false) == 0, "reject config");
    position(0, true, 1);
    position(0, false, 10);
    position(0, true, 10);
    position(0, false, 10);
    EXPECT();
    __ASSERT(rejected_releases == 0, "rejected action received release");

    cfg = (struct zmk_runtime_tapdance_config){.single_binding = {kp, A, 0},
                                               .double_binding = {kp, B, 0}};
    __ASSERT(zmk_runtime_tapdance_write(0, &cfg, false) == 0, "timer config");
    timestamp = k_uptime_get();
    position(0, true, 0);
    position(0, false, 1);
    __ASSERT(observed_count == 0, "real timer single too early");
    k_work_schedule(&timer_check_work, K_MSEC(150));
}
K_WORK_DELAYABLE_DEFINE(test_work, run);
static int start(void) {
    k_work_schedule(&test_work, K_MSEC(50));
    return 0;
}
SYS_INIT(start, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
