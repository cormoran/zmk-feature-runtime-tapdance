/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <stdio.h>
#include <pb_decode.h>
#include <pb_encode.h>
#include <zephyr/sys/util.h>
#include <zmk/studio/custom.h>
#include <cormoran/runtime-tapdance/runtime_tapdance.pb.h>
#include <cormoran/runtime_tapdance/runtime_tapdance.h>
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static struct zmk_rpc_custom_subsystem_meta runtime_tapdance_subsystem_meta = {
    ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS("https://cormoran.github.io/zmk-feature-runtime-tapdance/"),
    .security = ZMK_STUDIO_RPC_HANDLER_UNSECURED,
};
ZMK_RPC_CUSTOM_SUBSYSTEM(cormoran__runtime_tapdance, &runtime_tapdance_subsystem_meta,
                         runtime_tapdance_rpc_handle_request);
/* The macro owns static storage: RPC encoding runs after this handler returns. */
ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER(cormoran__runtime_tapdance,
                                         cormoran_runtime_tapdance_Response);
BUILD_ASSERT(CONFIG_ZMK_STUDIO_RPC_RX_BUF_SIZE >= 256,
             "Runtime tapdance requires a 256-byte Studio RX buffer");
BUILD_ASSERT(CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE >= 256,
             "Runtime tapdance requires a 256-byte Studio TX buffer");
BUILD_ASSERT(cormoran_runtime_tapdance_Request_size + 64 <= CONFIG_ZMK_STUDIO_RPC_RX_BUF_SIZE,
             "Runtime tapdance request exceeds Studio RX buffer");
BUILD_ASSERT(cormoran_runtime_tapdance_Response_size + 64 <= CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE,
             "Runtime tapdance response exceeds Studio TX buffer");

static void set_error(cormoran_runtime_tapdance_Response *resp, int code, const char *message) {
    resp->which_response_type = cormoran_runtime_tapdance_Response_error_tag;
    resp->response_type.error.code = code;
    snprintf(resp->response_type.error.message, sizeof(resp->response_type.error.message), "%s",
             message);
}

static void encode_binding(cormoran_runtime_tapdance_Binding *target,
                           const struct zmk_custom_setting_behavior_value *source) {
    target->behavior_id = source->behavior_id;
    target->param1 = source->param1;
    target->param2 = source->param2;
}

static int set_state(cormoran_runtime_tapdance_Response *resp, uint32_t index) {
    struct zmk_runtime_tapdance_config config;
    int rc = zmk_runtime_tapdance_read(index, &config);
    if (rc) {
        return rc;
    }
    resp->which_response_type = cormoran_runtime_tapdance_Response_state_tag;
    cormoran_runtime_tapdance_State *state = &resp->response_type.state;
    state->index = index;
    state->interval_ms = zmk_runtime_tapdance_timeout_ms();
    state->max_count = zmk_runtime_tapdance_max_count();
    state->has_config = true;
    state->config.enabled = config.enabled;
    state->config.position = config.position;
    state->config.delay_single = config.delay_single;
    state->config.has_double_binding = true;
    state->config.has_triple_binding = true;
    encode_binding(&state->config.double_binding, &config.double_binding);
    encode_binding(&state->config.triple_binding, &config.triple_binding);
    return 0;
}

static bool dispatch_request(const zmk_custom_CallRequest *raw_request,
                             pb_callback_t *encode_response) {
    cormoran_runtime_tapdance_Response *resp = ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER_ALLOCATE(
        cormoran__runtime_tapdance, encode_response);
    cormoran_runtime_tapdance_Request req = cormoran_runtime_tapdance_Request_init_zero;
    pb_istream_t stream =
        pb_istream_from_buffer(raw_request->payload.bytes, raw_request->payload.size);
    if (!pb_decode(&stream, cormoran_runtime_tapdance_Request_fields, &req)) {
        set_error(resp, -EINVAL, "Failed to decode request");
        return true;
    }
    int rc;
    uint32_t index = 0;
    switch (req.which_request_type) {
    case cormoran_runtime_tapdance_Request_get_tag:
        index = req.request_type.get.index;
        rc = 0;
        break;
    case cormoran_runtime_tapdance_Request_set_tag: {
        const cormoran_runtime_tapdance_SetRequest *set = &req.request_type.set;
        index = set->index;
        if (!set->has_config) {
            set_error(resp, -EINVAL, "Missing tapdance configuration");
            return true;
        }
        const cormoran_runtime_tapdance_Config *source = &set->config;
        struct zmk_runtime_tapdance_config config = {
            .enabled = source->enabled,
            .position = source->position,
            .delay_single = source->delay_single,
            .double_binding = {.behavior_id = source->double_binding.behavior_id,
                               .param1 = source->double_binding.param1,
                               .param2 = source->double_binding.param2},
            .triple_binding = {.behavior_id = source->triple_binding.behavior_id,
                               .param1 = source->triple_binding.param1,
                               .param2 = source->triple_binding.param2},
        };
        rc = zmk_runtime_tapdance_write(index, &config, set->persist);
        break;
    }
    case cormoran_runtime_tapdance_Request_remove_tag:
        index = req.request_type.remove.index;
        rc = zmk_runtime_tapdance_delete(index, req.request_type.remove.persist);
        break;
    case cormoran_runtime_tapdance_Request_set_interval_tag:
        rc = zmk_runtime_tapdance_set_timeout_ms(req.request_type.set_interval.interval_ms,
                                                 req.request_type.set_interval.persist);
        break;
    default:
        set_error(resp, -ENOTSUP, "Unsupported request");
        return true;
    }
    if (!rc) {
        rc = set_state(resp, index);
    }
    if (rc) {
        set_error(resp, rc, "Invalid configuration or settings write failed");
    }
    return true;
}

static bool runtime_tapdance_rpc_handle_request(const zmk_custom_CallRequest *raw_request,
                                                pb_callback_t *encode_response) {
    /* Mutations already return their state. Redundant registry notifications
     * can starve that response on the shared Studio transport. */
    zmk_custom_settings_notify_suppress_begin();
    bool result = dispatch_request(raw_request, encode_response);
    zmk_custom_settings_notify_suppress_end();
    return result;
}
