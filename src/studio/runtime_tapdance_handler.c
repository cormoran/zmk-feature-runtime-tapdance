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
BUILD_ASSERT(CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE >= 320,
             "Runtime tapdance requires a 320-byte Studio TX buffer");
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
    rc = zmk_runtime_tapdance_name(index, state->name, sizeof(state->name),
                                   &state->has_unsaved_changes);
    if (rc < 0) {
        return rc;
    }
    state->has_config = true;
    state->config.has_single_binding = true;
    state->config.has_double_binding = true;
    state->config.has_triple_binding = true;
    state->config.has_single_hold_binding = config.hold_bindings[0].behavior_id != 0;
    state->config.has_double_hold_binding = config.hold_bindings[1].behavior_id != 0;
    state->config.has_triple_hold_binding = config.hold_bindings[2].behavior_id != 0;
    encode_binding(&state->config.single_binding, &config.single_binding);
    encode_binding(&state->config.double_binding, &config.double_binding);
    encode_binding(&state->config.triple_binding, &config.triple_binding);
    encode_binding(&state->config.single_hold_binding, &config.hold_bindings[0]);
    encode_binding(&state->config.double_hold_binding, &config.hold_bindings[1]);
    encode_binding(&state->config.triple_hold_binding, &config.hold_bindings[2]);
    state->config.dance_interval_ms = config.interval_ms;
    state->config.single_interval_ms = config.tap_intervals_ms[0];
    state->config.double_interval_ms = config.tap_intervals_ms[1];
    state->config.triple_interval_ms = config.tap_intervals_ms[2];
    return 0;
}

static void set_global(cormoran_runtime_tapdance_Response *resp) {
    resp->which_response_type = cormoran_runtime_tapdance_Response_global_tag;
    cormoran_runtime_tapdance_GlobalResponse *global = &resp->response_type.global;
    global->interval_ms = zmk_runtime_tapdance_timeout_ms();
    global->max_entries = zmk_runtime_tapdance_max_count();
    global->used_entries = zmk_runtime_tapdance_used_count();
    global->max_name_length = CONFIG_ZMK_RUNTIME_TAPDANCE_NAME_MAX_LEN;
    global->pool_bytes_total = zmk_runtime_tapdance_pool_total();
    global->pool_bytes_used = zmk_runtime_tapdance_pool_used();
    global->has_unsaved_changes = zmk_runtime_tapdance_has_unsaved_changes();
    global->api_version = 3;
}

struct list_context {
    uint32_t after_id;
    bool has_after_id;
    uint32_t matching_count;
    uint32_t kept_count;
    cormoran_runtime_tapdance_Summary entries[4];
};

static int collect_list(uint32_t index, const char *name, bool dirty, void *context) {
    struct list_context *list = context;
    if (list->has_after_id && index <= list->after_id) {
        return 0;
    }
    list->matching_count++;
    uint32_t position = 0;
    while (position < list->kept_count && list->entries[position].index < index) {
        position++;
    }
    if (position >= ARRAY_SIZE(list->entries)) {
        return 0;
    }
    if (list->kept_count < ARRAY_SIZE(list->entries)) {
        list->kept_count++;
    }
    for (uint32_t i = list->kept_count - 1; i > position; i--) {
        list->entries[i] = list->entries[i - 1];
    }
    list->entries[position].index = index;
    snprintf(list->entries[position].name, sizeof(list->entries[position].name), "%s", name);
    list->entries[position].has_unsaved_changes = dirty;
    return 0;
}

static int set_list(cormoran_runtime_tapdance_Response *resp,
                    const cormoran_runtime_tapdance_ListRequest *request) {
    struct list_context list = {.after_id = request->after_id,
                                .has_after_id = request->has_after_id};
    int rc = zmk_runtime_tapdance_for_each(collect_list, &list);
    if (rc < 0) {
        return rc;
    }
    resp->which_response_type = cormoran_runtime_tapdance_Response_list_tag;
    cormoran_runtime_tapdance_ListResponse *result = &resp->response_type.list;
    result->entries_count = list.kept_count;
    result->has_more = list.matching_count > list.kept_count;
    for (uint32_t i = 0; i < list.kept_count; i++) {
        result->entries[i] = list.entries[i];
    }
    if (list.kept_count) {
        result->next_after_id = list.entries[list.kept_count - 1].index;
    }
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
    int rc = 0;
    uint32_t index = 0;
    bool return_state = false;
    switch (req.which_request_type) {
    case cormoran_runtime_tapdance_Request_get_tag:
        index = req.request_type.get.index;
        return_state = true;
        break;
    case cormoran_runtime_tapdance_Request_set_tag: {
        const cormoran_runtime_tapdance_SetRequest *set = &req.request_type.set;
        index = set->index;
        if (!set->has_binding) {
            set_error(resp, -EINVAL, "Missing tapdance binding");
            return true;
        }
        const struct zmk_custom_setting_behavior_value binding = {
            .behavior_id = set->binding.behavior_id,
            .param1 = set->binding.param1,
            .param2 = set->binding.param2,
        };
        rc = set->hold
                 ? zmk_runtime_tapdance_set_hold_binding(index, set->tap_count, &binding,
                                                         set->persist)
                 : zmk_runtime_tapdance_set_binding(index, set->tap_count, &binding, set->persist);
        return_state = true;
        break;
    }
    case cormoran_runtime_tapdance_Request_set_timing_tag:
        index = req.request_type.set_timing.index;
        rc = zmk_runtime_tapdance_set_interval(index, req.request_type.set_timing.tap_count,
                                               req.request_type.set_timing.interval_ms,
                                               req.request_type.set_timing.persist);
        return_state = true;
        break;
    case cormoran_runtime_tapdance_Request_remove_tag:
        index = req.request_type.remove.index;
        rc = zmk_runtime_tapdance_delete(index, req.request_type.remove.persist);
        if (rc == 0) {
            resp->which_response_type = cormoran_runtime_tapdance_Response_status_tag;
            resp->response_type.status.affected_count = 1;
        }
        break;
    case cormoran_runtime_tapdance_Request_set_interval_tag:
        rc = zmk_runtime_tapdance_set_timeout_ms(req.request_type.set_interval.interval_ms,
                                                 req.request_type.set_interval.persist);
        if (rc == 0) {
            set_global(resp);
        }
        break;
    case cormoran_runtime_tapdance_Request_list_tag:
        rc = set_list(resp, &req.request_type.list);
        break;
    case cormoran_runtime_tapdance_Request_create_tag:
        rc = zmk_runtime_tapdance_create(req.request_type.create.name,
                                         req.request_type.create.persist, &index);
        return_state = true;
        break;
    case cormoran_runtime_tapdance_Request_rename_tag:
        index = req.request_type.rename.index;
        rc = zmk_runtime_tapdance_rename(index, req.request_type.rename.name,
                                         req.request_type.rename.persist);
        return_state = true;
        break;
    case cormoran_runtime_tapdance_Request_delete_tag:
        rc = zmk_runtime_tapdance_delete(req.request_type.delete.index, true);
        if (rc == 0) {
            resp->which_response_type = cormoran_runtime_tapdance_Response_status_tag;
            resp->response_type.status.affected_count = 1;
        }
        break;
    case cormoran_runtime_tapdance_Request_get_global_tag:
        set_global(resp);
        break;
    case cormoran_runtime_tapdance_Request_save_tag:
        resp->which_response_type = cormoran_runtime_tapdance_Response_status_tag;
        rc = zmk_runtime_tapdance_save(&resp->response_type.status.affected_count);
        break;
    case cormoran_runtime_tapdance_Request_discard_tag:
        resp->which_response_type = cormoran_runtime_tapdance_Response_status_tag;
        rc = zmk_runtime_tapdance_discard(&resp->response_type.status.affected_count);
        break;
    default:
        set_error(resp, -ENOTSUP, "Unsupported request");
        return true;
    }
    if (!rc && return_state) {
        rc = set_state(resp, index);
    }
    if (rc) {
        const char *message = rc == -ELOOP      ? "A tap dance cannot invoke itself"
                              : rc == -ENOENT   ? "Tap dance ID does not exist"
                              : rc == -EEXIST   ? "Tap dance name already exists"
                              : rc == -ENOSPC   ? "Tap dance pool or entry limit reached"
                              : rc == -EMSGSIZE ? "Tap dance name is too long"
                                                : "Invalid configuration or settings write failed";
        set_error(resp, rc, message);
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
