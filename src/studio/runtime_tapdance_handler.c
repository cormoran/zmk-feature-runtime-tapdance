#include <pb_decode.h>
#include <pb_encode.h>
#include <zephyr/sys/util.h>
#include <zmk/studio/custom.h>
#include <cormoran/runtime-tapdance/runtime_tapdance.pb.h>

#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS)
#include <cormoran/zmk/custom_settings.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT_RELAY_EVENT)
#include <cormoran/runtime-tapdance/template_relay.h>
#endif

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

static struct zmk_rpc_custom_subsystem_meta runtime_tapdance_subsystem_meta = {
    ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS("https://cormoran.github.io/zmk-feature-runtime-tapdance/"),
    // Unsecured is suggested by default to avoid unlocking in un-reliable
    // environments.
    // The web template already implements the unlock prompt/retry flow (see
    // web/src/App.tsx), so switching this to ZMK_STUDIO_RPC_HANDLER_SECURED
    // requires no web changes.
    .security = ZMK_STUDIO_RPC_HANDLER_UNSECURED,
};

ZMK_RPC_CUSTOM_SUBSYSTEM(cormoran__runtime_tapdance, &runtime_tapdance_subsystem_meta,
                         runtime_tapdance_rpc_handle_request);

ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER(cormoran__runtime_tapdance,
                                         cormoran_runtime_tapdance_Response);

#if IS_ENABLED(CONFIG_ZMK_CUSTOM_SETTINGS)
ZMK_CUSTOM_SETTING_DEFINE(runtime_tapdance_sample_bool, "cormoran__runtime_tapdance", "sample_bool",
                          ZMK_CUSTOM_SETTING_VALUE_TYPE_BOOL, ZMK_CUSTOM_SETTING_VALUE_BOOL(true),
                          ZMK_CUSTOM_SETTING_CONFIDENTIALITY_RPC_PUBLIC,
                          ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE,
                          ZMK_CUSTOM_SETTING_PERMISSION_UNSECURE, ZMK_CUSTOM_SETTING_NO_CONSTRAINT);
#endif

static int handle_sample_request(const cormoran_runtime_tapdance_SampleRequest *req,
                                 cormoran_runtime_tapdance_Response *resp);

static bool runtime_tapdance_rpc_handle_request(const zmk_custom_CallRequest *raw_request,
                                                pb_callback_t *encode_response) {
    cormoran_runtime_tapdance_Response *resp = ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER_ALLOCATE(
        cormoran__runtime_tapdance, encode_response);

    cormoran_runtime_tapdance_Request req = cormoran_runtime_tapdance_Request_init_zero;

    pb_istream_t req_stream =
        pb_istream_from_buffer(raw_request->payload.bytes, raw_request->payload.size);
    if (!pb_decode(&req_stream, cormoran_runtime_tapdance_Request_fields, &req)) {
        LOG_WRN("Failed to decode runtime_tapdance request: %s", PB_GET_ERROR(&req_stream));
        cormoran_runtime_tapdance_ErrorResponse err =
            cormoran_runtime_tapdance_ErrorResponse_init_zero;
        snprintf(err.message, sizeof(err.message), "Failed to decode request");
        resp->which_response_type = cormoran_runtime_tapdance_Response_error_tag;
        resp->response_type.error = err;
        return true;
    }

    int rc = 0;
    switch (req.which_request_type) {
    case cormoran_runtime_tapdance_Request_sample_tag:
        rc = handle_sample_request(&req.request_type.sample, resp);
        break;
    default:
        LOG_WRN("Unsupported runtime_tapdance request type: %d", req.which_request_type);
        rc = -1;
    }

    if (rc != 0) {
        cormoran_runtime_tapdance_ErrorResponse err =
            cormoran_runtime_tapdance_ErrorResponse_init_zero;
        snprintf(err.message, sizeof(err.message), "Failed to process request");
        resp->which_response_type = cormoran_runtime_tapdance_Response_error_tag;
        resp->response_type.error = err;
    }
    return true;
}

static int handle_sample_request(const cormoran_runtime_tapdance_SampleRequest *req,
                                 cormoran_runtime_tapdance_Response *resp) {
    LOG_DBG("Received sample request with value: %d", req->value);

#if IS_ENABLED(CONFIG_ZMK_SPLIT_RELAY_EVENT)
    // Split-relay sample: forward the received value to the split
    // peripheral(s) over ZMK's split event-relay as a plain packed C struct
    // (see src/split/template_relay.c). A no-op unless this build is a split
    // central with a connected peripheral.
    template_relay_send_sample(req->value);
#endif

    cormoran_runtime_tapdance_SampleResponse result =
        cormoran_runtime_tapdance_SampleResponse_init_zero;

    snprintf(result.value, sizeof(result.value), "Hello from firmware! Received: %d", req->value);

    resp->which_response_type = cormoran_runtime_tapdance_Response_sample_tag;
    resp->response_type.sample = result;
    return 0;
}
