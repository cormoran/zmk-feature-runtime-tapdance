/* SPDX-License-Identifier: MIT */
/* Native-sim integration test: use the public position event and inspect the
 * resulting public HID keycode events, rather than private state-machine fields. */
#include <errno.h>
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

static uint32_t observed[100];
static size_t observed_count;
static bool collecting;
static int64_t timestamp;
static int observe(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (collecting && ev && ev->keycode != (D & 0xFFFF)) {
        __ASSERT(observed_count < ARRAY_SIZE(observed), "keycode observation overflow");
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
static void flush(void) { position(3, false, 100); }
#define TEST_DOWN(key) (((key) & 0xFFFF) | BIT(16))
#define TEST_UP(key) ((key) & 0xFFFF)
#define EXPECT(...)                                                                                \
    do {                                                                                           \
        const uint32_t expected[] = {__VA_ARGS__};                                                 \
        __ASSERT(observed_count == ARRAY_SIZE(expected), "keycode count mismatch %u vs %u",        \
                 (unsigned)observed_count, (unsigned)ARRAY_SIZE(expected));                        \
        for (size_t i = 0; i < observed_count; i++) {                                              \
            __ASSERT(observed[i] == expected[i], "keycode mismatch at %u: %x vs %x", (unsigned)i,  \
                     observed[i], expected[i]);                                                    \
        }                                                                                          \
        observed_count = 0;                                                                        \
    } while (0)
static void verify_holdtap(struct k_work *work) {
    ARG_UNUSED(work);
    timestamp = k_uptime_get();
    position(2, false, 0);
    EXPECT(TEST_DOWN(SPACE), TEST_UP(SPACE), TEST_DOWN(LSHIFT), TEST_DOWN(ENTER), TEST_UP(ENTER),
           TEST_UP(LSHIFT));
    collecting = false;
    LOG_INF("PASS: runtime_tapdance_public_events");
}
K_WORK_DELAYABLE_DEFINE(holdtap_check_work, verify_holdtap);
static void verify_timer(struct k_work *work) {
    ARG_UNUSED(work);
    EXPECT(TEST_DOWN(A), TEST_UP(A));
    uint32_t id = zmk_behavior_get_local_id("mod_tap");
    __ASSERT(id && id != UINT16_MAX, "hold_tap local id missing");
    struct zmk_runtime_tapdance_config config = {.enabled = true,
                                                 .position = 2,
                                                 .delay_single = false,
                                                 .double_binding = {id, LCTRL, ENTER}};
    __ASSERT(zmk_runtime_tapdance_write(2, &config, false) == 0, "holdtap config");
    __ASSERT(zmk_runtime_tapdance_set_timeout_ms(250, false) == 0, "holdtap interval");
    timestamp = k_uptime_get();
    position(2, true, 0);
    position(2, false, 0);
    position(2, true, 1);
    /* Normal Shift resolves before the independent action hold-tap begins. */
    k_work_schedule(&holdtap_check_work, K_MSEC(350));
}
K_WORK_DELAYABLE_DEFINE(timer_check_work, verify_timer);
static void run(struct k_work *work) {
    ARG_UNUSED(work);
    uint32_t id = zmk_behavior_get_local_id("key_press");
    __ASSERT(id && id != UINT16_MAX, "key_press local id missing");
    struct zmk_runtime_tapdance_config cfg = {.enabled = true,
                                              .position = 0,
                                              .delay_single = true,
                                              .double_binding = {id, B, 0},
                                              .triple_binding = {id, C, 0}};
    __ASSERT(zmk_runtime_tapdance_write(0, &cfg, false) == 0, "config write");
    __ASSERT(zmk_runtime_tapdance_set_timeout_ms(100, false) == 0, "interval");
    __ASSERT(zmk_runtime_tapdance_set_timeout_ms(0, false) == -EINVAL, "invalid interval");
    __ASSERT(zmk_runtime_tapdance_set_timeout_ms(2001, false) == -EINVAL, "invalid interval");
    __ASSERT(zmk_runtime_tapdance_write(1, &cfg, false) == -EEXIST, "duplicate position");
    struct zmk_runtime_tapdance_config bad = cfg;
    bad.position = 4;
    __ASSERT(zmk_runtime_tapdance_write(1, &bad, false) == -EINVAL, "invalid position");
    bad = cfg;
    bad.double_binding.behavior_id = UINT32_MAX;
    __ASSERT(zmk_runtime_tapdance_write(0, &bad, false) == -ENODEV, "invalid behavior id");
    bad = cfg;
    bad.double_binding.param2 = 1;
    __ASSERT(zmk_runtime_tapdance_write(0, &bad, false) < 0, "invalid behavior param");
    const struct zmk_custom_setting *array = zmk_custom_setting_find_array(
        ZMK_RUNTIME_TAPDANCE_SUBSYSTEM_ID, ZMK_RUNTIME_TAPDANCE_SLOTS_KEY);
    __ASSERT(array, "slots array missing");
    while (zmk_custom_setting_array_size(array)) {
        __ASSERT(zmk_custom_setting_array_pop_back(array, NULL,
                                                   ZMK_CUSTOM_SETTING_WRITE_MODE_MEMORY) == 0,
                 "shrink array");
    }
    struct zmk_runtime_tapdance_config empty;
    __ASSERT(zmk_runtime_tapdance_read(0, &empty) == 0 && !empty.enabled, "short array read");
    __ASSERT(zmk_runtime_tapdance_write(0, &cfg, false) == 0, "recover short array");
    timestamp = k_uptime_get() + 10000;
    collecting = true;
    position(0, true, 1);
    position(0, false, 10);
    __ASSERT(observed_count == 0, "delayed single emitted before deadline");
    flush();
    EXPECT(TEST_DOWN(A), TEST_UP(A));
    position(0, true, 1);
    position(0, false, 10);
    position(0, true, 10);
    position(0, false, 10);
    __ASSERT(observed_count == 0, "double must await triple deadline");
    flush();
    EXPECT(TEST_DOWN(B), TEST_UP(B));
    position(0, true, 1);
    position(0, false, 10);
    position(0, true, 10);
    flush();
    EXPECT(TEST_DOWN(B));
    position(0, false, 10);
    EXPECT(TEST_UP(B));
    position(0, true, 1);
    position(0, false, 10);
    position(0, true, 10);
    position(0, false, 10);
    position(0, true, 10);
    EXPECT(TEST_DOWN(C));
    position(0, false, 10);
    EXPECT(TEST_UP(C));
    position(0, true, 1);
    flush();
    EXPECT(TEST_DOWN(A));
    position(0, false, 10);
    EXPECT(TEST_UP(A));
    /* A second press exactly at the first deadline starts a new single. */
    position(0, true, 1);
    position(0, false, 10);
    position(0, true, 90);
    position(0, false, 10);
    flush();
    EXPECT(TEST_DOWN(A), TEST_UP(A), TEST_DOWN(A), TEST_UP(A));
    /* Editing the slot does not mutate an already-active sequence. */
    position(0, true, 1);
    position(0, false, 10);
    cfg.double_binding.param1 = C;
    __ASSERT(zmk_runtime_tapdance_write(0, &cfg, false) == 0, "edit");
    position(0, true, 10);
    position(0, false, 10);
    flush();
    EXPECT(TEST_DOWN(B), TEST_UP(B));
    cfg.double_binding = (struct zmk_custom_setting_behavior_value){0};
    cfg.triple_binding = (struct zmk_custom_setting_behavior_value){0};
    __ASSERT(zmk_runtime_tapdance_write(0, &cfg, false) == 0, "fallback config");
    position(0, true, 1);
    position(0, false, 10);
    position(0, true, 10);
    position(0, false, 10);
    flush();
    EXPECT(TEST_DOWN(A), TEST_UP(A), TEST_DOWN(A), TEST_UP(A));
    position(0, true, 1);
    position(0, false, 10);
    position(0, true, 10);
    position(0, false, 10);
    position(0, true, 10);
    position(0, false, 10);
    EXPECT(TEST_DOWN(A), TEST_UP(A), TEST_DOWN(A), TEST_UP(A), TEST_DOWN(A), TEST_UP(A));
    cfg.double_binding = (struct zmk_custom_setting_behavior_value){
        zmk_behavior_get_local_id("tapdance_test_reject"), 0, 0};
    __ASSERT(zmk_runtime_tapdance_write(0, &cfg, false) == 0, "rejected action config");
    position(0, true, 1);
    position(0, false, 10);
    position(0, true, 10);
    position(0, false, 10);
    flush();
    EXPECT(TEST_DOWN(A), TEST_UP(A), TEST_DOWN(A), TEST_UP(A));
    __ASSERT(rejected_releases == 0, "rejected action received release");
    cfg.delay_single = false;
    cfg.double_binding = (struct zmk_custom_setting_behavior_value){id, B, 0};
    __ASSERT(zmk_runtime_tapdance_write(0, &cfg, false) == 0, "immediate config");
    position(0, true, 1);
    position(0, false, 10);
    position(0, true, 10);
    position(0, false, 10);
    flush();
    EXPECT(TEST_DOWN(A), TEST_UP(A), TEST_DOWN(A), TEST_UP(A), TEST_DOWN(B), TEST_UP(B));
    cfg.delay_single = true;
    __ASSERT(zmk_runtime_tapdance_write(0, &cfg, false) == 0, "delayed config");
    cfg.position = 1;
    __ASSERT(zmk_runtime_tapdance_write(1, &cfg, false) == 0, "second slot");
    position(0, true, 1);
    position(1, true, 10);
    position(0, false, 10);
    position(1, false, 10);
    flush();
    EXPECT(TEST_DOWN(A), TEST_UP(A), TEST_DOWN(E), TEST_UP(E));
    position(1, true, 1);
    position(0, true, 10);
    position(1, false, 10);
    position(0, false, 10);
    flush();
    EXPECT(TEST_DOWN(E), TEST_UP(E), TEST_DOWN(A), TEST_UP(A));
    __ASSERT(zmk_runtime_tapdance_delete(0, false) == 0, "delete");
    position(0, true, 1);
    position(0, false, 10);
    EXPECT(TEST_DOWN(A), TEST_UP(A));
    cfg.position = 0;
    __ASSERT(zmk_runtime_tapdance_write(0, &cfg, false) == 0, "timer config");
    timestamp = k_uptime_get();
    position(0, true, 0);
    position(0, false, 0);
    __ASSERT(observed_count == 0, "real timer single too early");
    k_work_schedule(&timer_check_work, K_MSEC(150));
}
K_WORK_DELAYABLE_DEFINE(test_work, run);
static int start(void) {
    k_work_schedule(&test_work, K_MSEC(50));
    return 0;
}
SYS_INIT(start, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
