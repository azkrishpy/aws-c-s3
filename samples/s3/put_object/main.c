/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

/*
 * Minimal, standalone sample that uploads a single local file to S3 using ONLY
 * the public aws-c-s3 API (no private/ headers).
 *
 * Usage:
 *   put_object --region <region> --bucket <bucket> --key <key> --input <path>
 *
 * Credentials are resolved from the default credential provider chain
 * (environment variables, shared config/credentials profile, IMDS, etc.).
 */

#include <aws/auth/credentials.h>
#include <aws/common/command_line_parser.h>
#include <aws/common/condition_variable.h>
#include <aws/common/file.h>
#include <aws/common/mutex.h>
#include <aws/common/string.h>
#include <aws/common/zero.h>
#include <aws/http/request_response.h>
#include <aws/io/channel_bootstrap.h>
#include <aws/io/event_loop.h>
#include <aws/io/host_resolver.h>
#include <aws/io/stream.h>
#include <aws/s3/s3.h>
#include <aws/s3/s3_client.h>

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* Shared state used to block main() until the async meta request completes. */
struct app_state {
    struct aws_mutex mutex;
    struct aws_condition_variable c_var;
    bool done;
    int error_code;
    int response_status;
};

static bool s_meta_request_done(void *arg) {
    struct app_state *state = arg;
    return state->done;
}

/* Invoked once the entire meta request finishes (success or failure). */
static void s_on_finish(
    struct aws_s3_meta_request *meta_request,
    const struct aws_s3_meta_request_result *result,
    void *user_data) {

    (void)meta_request;
    struct app_state *state = user_data;

    aws_mutex_lock(&state->mutex);
    state->done = true;
    state->error_code = result->error_code;
    state->response_status = result->response_status;
    aws_mutex_unlock(&state->mutex);
    aws_condition_variable_notify_one(&state->c_var);
}

static void s_usage(int exit_code) {
    FILE *out = exit_code == 0 ? stdout : stderr;
    fprintf(out, "usage: put_object --region <region> --bucket <bucket> --key <key> --input <path>\n");
    exit(exit_code);
}

static struct aws_cli_option s_long_options[] = {
    {"region", AWS_CLI_OPTIONS_REQUIRED_ARGUMENT, NULL, 'r'},
    {"bucket", AWS_CLI_OPTIONS_REQUIRED_ARGUMENT, NULL, 'b'},
    {"key", AWS_CLI_OPTIONS_REQUIRED_ARGUMENT, NULL, 'k'},
    {"input", AWS_CLI_OPTIONS_REQUIRED_ARGUMENT, NULL, 'i'},
    {"help", AWS_CLI_OPTIONS_NO_ARGUMENT, NULL, 'h'},
    /* getopt(3) requires the last element to be all zeros. */
    {NULL, AWS_CLI_OPTIONS_NO_ARGUMENT, NULL, 0},
};

int main(int argc, char *argv[]) {
    const char *region = NULL;
    const char *bucket = NULL;
    const char *key = NULL;
    const char *input = NULL;

    while (true) {
        int option_index = 0;
        int c = aws_cli_getopt_long(argc, argv, "r:b:k:i:h", s_long_options, &option_index);
        if (c == -1) {
            break;
        }
        switch (c) {
            case 'r':
                region = aws_cli_optarg;
                break;
            case 'b':
                bucket = aws_cli_optarg;
                break;
            case 'k':
                key = aws_cli_optarg;
                break;
            case 'i':
                input = aws_cli_optarg;
                break;
            case 'h':
                s_usage(0);
                break;
            default:
                break;
        }
    }

    if (region == NULL || bucket == NULL || key == NULL || input == NULL) {
        fprintf(stderr, "error: --region, --bucket, --key and --input are all required\n");
        s_usage(1);
    }

    struct aws_allocator *allocator = aws_default_allocator();
    aws_s3_library_init(allocator);

    /* Determine content length; S3 requires a Content-Length for the request. */
    int64_t file_size = 0;
    {
        struct aws_string *input_path = aws_string_new_from_c_str(allocator, input);
        struct aws_string *open_mode = aws_string_new_from_c_str(allocator, "rb");
        FILE *input_file = aws_fopen_safe(input_path, open_mode);
        if (input_file == NULL) {
            fprintf(stderr, "error: cannot open input file '%s': %s\n", input, aws_error_debug_str(aws_last_error()));
            aws_string_destroy(open_mode);
            aws_string_destroy(input_path);
            aws_s3_library_clean_up();
            return 1;
        }
        aws_file_get_length(input_file, &file_size);
        fclose(input_file);
        aws_string_destroy(open_mode);
        aws_string_destroy(input_path);
    }

    /* Standard CRT plumbing: event loop group -> host resolver -> client bootstrap. */
    struct aws_event_loop_group *event_loop_group = aws_event_loop_group_new_default(allocator, 0, NULL);

    struct aws_host_resolver_default_options resolver_options = {
        .el_group = event_loop_group,
        .max_entries = 8,
    };
    struct aws_host_resolver *resolver = aws_host_resolver_new_default(allocator, &resolver_options);

    struct aws_client_bootstrap_options bootstrap_options = {
        .event_loop_group = event_loop_group,
        .host_resolver = resolver,
    };
    struct aws_client_bootstrap *client_bootstrap = aws_client_bootstrap_new(allocator, &bootstrap_options);

    /* Credentials from the default provider chain. */
    struct aws_credentials_provider_chain_default_options credentials_options;
    AWS_ZERO_STRUCT(credentials_options);
    credentials_options.bootstrap = client_bootstrap;
    struct aws_credentials_provider *credentials_provider =
        aws_credentials_provider_new_chain_default(allocator, &credentials_options);

    struct aws_byte_cursor region_cursor = aws_byte_cursor_from_c_str(region);

    struct aws_signing_config_aws signing_config;
    aws_s3_init_default_signing_config(&signing_config, region_cursor, credentials_provider);

    /* Create the S3 client. */
    struct aws_s3_client_config client_config;
    AWS_ZERO_STRUCT(client_config);
    client_config.client_bootstrap = client_bootstrap;
    client_config.region = region_cursor;
    client_config.signing_config = &signing_config;
    struct aws_s3_client *client = aws_s3_client_new(allocator, &client_config);

    /* Build the HTTP request describing the PUT. */
    char endpoint[512];
    snprintf(endpoint, sizeof(endpoint), "%s.s3.%s.amazonaws.com", bucket, region);
    char path[1024];
    snprintf(path, sizeof(path), "/%s", key);
    char content_length[32];
    snprintf(content_length, sizeof(content_length), "%" PRIi64, file_size);

    struct aws_http_message *message = aws_http_message_new_request(allocator);
    aws_http_message_set_request_method(message, aws_byte_cursor_from_c_str("PUT"));
    aws_http_message_set_request_path(message, aws_byte_cursor_from_c_str(path));
    struct aws_http_header host_header = {
        .name = aws_byte_cursor_from_c_str("host"),
        .value = aws_byte_cursor_from_c_str(endpoint),
    };
    aws_http_message_add_header(message, host_header);
    struct aws_http_header length_header = {
        .name = aws_byte_cursor_from_c_str("content-length"),
        .value = aws_byte_cursor_from_c_str(content_length),
    };
    aws_http_message_add_header(message, length_header);

    struct app_state state;
    AWS_ZERO_STRUCT(state);
    aws_mutex_init(&state.mutex);
    state.c_var = (struct aws_condition_variable)AWS_CONDITION_VARIABLE_INIT;

    /* send_filepath lets the client stream the body straight from disk for us. */
    struct aws_s3_meta_request_options options;
    AWS_ZERO_STRUCT(options);
    options.type = AWS_S3_META_REQUEST_TYPE_PUT_OBJECT;
    options.message = message;
    options.send_filepath = aws_byte_cursor_from_c_str(input);
    options.finish_callback = s_on_finish;
    options.user_data = &state;

    struct aws_s3_meta_request *meta_request = aws_s3_client_make_meta_request(client, &options);

    /* Wait for completion. */
    aws_mutex_lock(&state.mutex);
    aws_condition_variable_wait_pred(&state.c_var, &state.mutex, s_meta_request_done, &state);
    aws_mutex_unlock(&state.mutex);

    int exit_code = 0;
    if (state.error_code == AWS_OP_SUCCESS) {
        printf("Uploaded %s to s3://%s/%s (HTTP %d)\n", input, bucket, key, state.response_status);
    } else {
        fprintf(stderr, "Upload failed: %s (HTTP %d)\n", aws_error_debug_str(state.error_code), state.response_status);
        exit_code = 1;
    }

    /* Tear everything down in reverse order of creation. */
    aws_s3_meta_request_release(meta_request);
    aws_http_message_release(message);
    aws_mutex_clean_up(&state.mutex);
    aws_s3_client_release(client);
    aws_credentials_provider_release(credentials_provider);
    aws_client_bootstrap_release(client_bootstrap);
    aws_host_resolver_release(resolver);
    aws_event_loop_group_release(event_loop_group);
    aws_s3_library_clean_up();

    return exit_code;
}
