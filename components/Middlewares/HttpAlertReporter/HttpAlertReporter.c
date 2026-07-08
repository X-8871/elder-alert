/**
 * @file HttpAlertReporter.c
 * @brief HTTP 告警上报器实现，通过 POST JSON 向云端推送事件和遥测数据。
 *
 * 上报触发条件：
 *   - 事件模式（EVENT）：应用状态变化、风险原因变化、SOS 新触发
 *   - 遥测模式（TELEMETRY）：距上次上报超过 10 秒
 * 不会因上报失败而阻塞主流程，失败仅记日志。
 */

#include "HttpAlertReporter.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "WiFiManager.h"
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"

#define HTTP_ALERT_REPORTER_URL "http://spectator0618.online:3000/api/alert"
#define HTTP_ALERT_REPORTER_DEVICE_TOKEN ""
#define HTTP_ALERT_REPORTER_TIMEOUT_MS 3000
#define HTTP_ALERT_REPORTER_TELEMETRY_INTERVAL_MS 10000U
#define HTTP_ALERT_REPORTER_MAX_REASON_LEN 192
#define HTTP_ALERT_REPORTER_MAX_ESCAPED_REASON_LEN 384
#define HTTP_ALERT_REPORTER_MAX_DEVICE_ID_LEN 18
#define HTTP_ALERT_REPORTER_REQUEST_BUFFER_LEN 1536
#define HTTP_ALERT_REPORTER_RESPONSE_BUFFER_LEN 2048

static const char *TAG = "HttpAlertReporter";

static bool s_initialized = false;
static bool s_device_id_ready = false;
static char s_device_id[HTTP_ALERT_REPORTER_MAX_DEVICE_ID_LEN] = {0};
static app_state_t s_last_handled_state = APP_STATE_NORMAL;
static char s_last_handled_reason[HTTP_ALERT_REPORTER_MAX_REASON_LEN] = {0};
static uint32_t s_last_handled_sos_trigger_count = 0;
static uint32_t s_last_report_attempt_ms = 0;

/* 服务器下发的设备命令缓存 */
static device_command_t s_pending_commands[HTTP_ALERT_REPORTER_MAX_COMMANDS] = {0};
static uint32_t s_pending_command_count = 0;

typedef enum {
    REPORT_MODE_NONE = 0,
    REPORT_MODE_EVENT,
    REPORT_MODE_TELEMETRY,
} report_mode_t;

static bool should_send_periodic_report(uint32_t now_ms)
{
    return s_last_report_attempt_ms == 0 ||
           (now_ms - s_last_report_attempt_ms) >= HTTP_ALERT_REPORTER_TELEMETRY_INTERVAL_MS;
}

static esp_err_t ensure_device_id(void)
{
    if (s_device_id_ready) {
        return ESP_OK;
    }

    uint8_t mac[6] = {0};
    esp_err_t ret = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (ret != ESP_OK) {
        return ret;
    }

    snprintf(s_device_id,
             sizeof(s_device_id),
             "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0],
             mac[1],
             mac[2],
             mac[3],
             mac[4],
             mac[5]);
    s_device_id_ready = true;
    return ESP_OK;
}

static void build_reason(const risk_result_t *risk_result, char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0) {
        return;
    }

    buffer[0] = '\0';
    if (risk_result != NULL) {
        RiskEngine_BuildReasonString(risk_result, buffer, buffer_size);
    }
    if (buffer[0] == '\0') {
        snprintf(buffer, buffer_size, "state_unknown");
    }
}

static bool json_escape_string(const char *input, char *output, size_t output_size)
{
    static const char hex[] = "0123456789ABCDEF";

    if (input == NULL || output == NULL || output_size == 0) {
        return false;
    }

    size_t out_pos = 0;
    for (size_t in_pos = 0; input[in_pos] != '\0'; ++in_pos) {
        unsigned char ch = (unsigned char)input[in_pos];
        const char *escape = NULL;

        switch (ch) {
        case '\"':
            escape = "\\\"";
            break;
        case '\\':
            escape = "\\\\";
            break;
        case '\b':
            escape = "\\b";
            break;
        case '\f':
            escape = "\\f";
            break;
        case '\n':
            escape = "\\n";
            break;
        case '\r':
            escape = "\\r";
            break;
        case '\t':
            escape = "\\t";
            break;
        default:
            break;
        }

        if (escape != NULL) {
            for (size_t i = 0; escape[i] != '\0'; ++i) {
                if (out_pos + 1U >= output_size) {
                    output[out_pos] = '\0';
                    return false;
                }
                output[out_pos++] = escape[i];
            }
            continue;
        }

        if (ch < 0x20U) {
            if (out_pos + 6U >= output_size) {
                output[out_pos] = '\0';
                return false;
            }
            output[out_pos++] = '\\';
            output[out_pos++] = 'u';
            output[out_pos++] = '0';
            output[out_pos++] = '0';
            output[out_pos++] = hex[(ch >> 4) & 0x0FU];
            output[out_pos++] = hex[ch & 0x0FU];
            continue;
        }

        if (out_pos + 1U >= output_size) {
            output[out_pos] = '\0';
            return false;
        }
        output[out_pos++] = (char)ch;
    }

    output[out_pos] = '\0';
    return true;
}

static const char *run_mode_string(void)
{
    return RiskEngine_GetRunMode() == RISK_RUN_MODE_REAL ? "REAL" : "DEMO";
}

static uint32_t remind_confirm_timeout_ms(void)
{
    return RiskEngine_GetRunMode() == RISK_RUN_MODE_REAL
               ? APP_CONTROLLER_REMIND_CONFIRM_TIMEOUT_MS_REAL
               : APP_CONTROLLER_REMIND_CONFIRM_TIMEOUT_MS_DEMO;
}

static uint32_t no_motion_remind_ms(void)
{
    return RiskEngine_GetRunMode() == RISK_RUN_MODE_REAL
               ? (5U * 60U * 1000U)
               : RISK_ENGINE_NO_MOTION_REMIND_MS;
}

static report_mode_t get_report_mode(app_state_t state,
                                     const char *reason,
                                     uint32_t sos_trigger_count,
                                     uint32_t now_ms)
{
    if (state == APP_STATE_SOS && sos_trigger_count != s_last_handled_sos_trigger_count) {
        return REPORT_MODE_EVENT;
    }

    if (state != s_last_handled_state) {
        return REPORT_MODE_EVENT;
    }

    if (reason != NULL &&
        strncmp(reason, s_last_handled_reason, sizeof(s_last_handled_reason)) != 0) {
        return REPORT_MODE_EVENT;
    }

    if (should_send_periodic_report(now_ms)) {
        return REPORT_MODE_TELEMETRY;
    }

    return REPORT_MODE_NONE;
}

static esp_err_t post_alert_json(const char *json_payload,
                                 report_mode_t report_mode,
                                 int *status_code,
                                 char *response_buf,
                                 size_t response_buf_size)
{
    esp_http_client_config_t config = {
        .url = HTTP_ALERT_REPORTER_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = HTTP_ALERT_REPORTER_TIMEOUT_MS,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_FAIL;
    }

    esp_err_t ret = esp_http_client_set_header(client, "Content-Type", "application/json");
    if (ret == ESP_OK) {
        ret = esp_http_client_set_header(client,
                                         "X-Report-Mode",
                                         report_mode == REPORT_MODE_EVENT ? "event" : "telemetry");
    }
    if (ret == ESP_OK && strlen(HTTP_ALERT_REPORTER_DEVICE_TOKEN) > 0U) {
        ret = esp_http_client_set_header(client, "X-Device-Token", HTTP_ALERT_REPORTER_DEVICE_TOKEN);
    }

    int post_len = (int)strlen(json_payload);
    if (ret == ESP_OK) {
        ret = esp_http_client_open(client, post_len);
    }
    if (ret != ESP_OK) {
        esp_http_client_cleanup(client);
        return ret;
    }

    int written = esp_http_client_write(client, json_payload, post_len);
    if (written < 0) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    int content_length = esp_http_client_fetch_headers(client);
    (void)content_length; /* chunked encoding 时可能为 -1 */

    if (status_code != NULL) {
        *status_code = esp_http_client_get_status_code(client);
    }

    /* 读取响应体 */
    if (response_buf != NULL && response_buf_size > 1U) {
        int total_read = 0;
        int read_len = 0;
        while (total_read < (int)response_buf_size - 1) {
            read_len = esp_http_client_read(client,
                                            response_buf + total_read,
                                            (int)response_buf_size - 1 - total_read);
            if (read_len <= 0) {
                break;
            }
            total_read += read_len;
        }
        response_buf[total_read] = '\0';
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ESP_OK;
}

/* 解析服务器响应中的 commands 数组，缓存设备命令 */
static void parse_commands_from_response(const char *response_body)
{
    if (response_body == NULL || response_body[0] == '\0') {
        return;
    }

    cJSON *root = cJSON_Parse(response_body);
    if (root == NULL) {
        return;
    }

    cJSON *commands = cJSON_GetObjectItem(root, "commands");
    if (commands == NULL || !cJSON_IsArray(commands)) {
        cJSON_Delete(root);
        return;
    }

    int array_size = cJSON_GetArraySize(commands);
    for (int i = 0; i < array_size && s_pending_command_count < HTTP_ALERT_REPORTER_MAX_COMMANDS; ++i) {
        cJSON *cmd = cJSON_GetArrayItem(commands, i);
        if (cmd == NULL || !cJSON_IsObject(cmd)) {
            continue;
        }

        cJSON *type_item = cJSON_GetObjectItem(cmd, "type");
        if (type_item == NULL || !cJSON_IsString(type_item)) {
            continue;
        }

        const char *type_str = type_item->valuestring;
        device_command_t *out = &s_pending_commands[s_pending_command_count];
        memset(out, 0, sizeof(*out));

        if (strcmp(type_str, "confirm_alert") == 0) {
            out->type = DEVICE_CMD_CONFIRM_ALERT;
        } else if (strcmp(type_str, "show_screen_message") == 0) {
            out->type = DEVICE_CMD_SHOW_SCREEN_MESSAGE;
            cJSON *msg = cJSON_GetObjectItem(cmd, "message");
            if (msg != NULL && cJSON_IsString(msg)) {
                strncpy(out->message, msg->valuestring, sizeof(out->message) - 1U);
            }
            cJSON *dur = cJSON_GetObjectItem(cmd, "duration");
            if (dur != NULL && cJSON_IsNumber(dur)) {
                out->duration = dur->valueint;
            } else {
                out->duration = 5;
            }
        } else if (strcmp(type_str, "beep_once") == 0) {
            out->type = DEVICE_CMD_BEEP_ONCE;
        } else if (strcmp(type_str, "play_tts") == 0) {
            out->type = DEVICE_CMD_PLAY_TTS;
            cJSON *url = cJSON_GetObjectItem(cmd, "url");
            if (url != NULL && cJSON_IsString(url)) {
                strncpy(out->url, url->valuestring, sizeof(out->url) - 1U);
            }
        } else if (strcmp(type_str, "set_run_mode") == 0) {
            out->type = DEVICE_CMD_SET_RUN_MODE;
            cJSON *mode_item = cJSON_GetObjectItem(cmd, "mode");
            if (mode_item != NULL && cJSON_IsString(mode_item)) {
                out->run_mode = (strcmp(mode_item->valuestring, "REAL") == 0) ? 1 : 0;
            } else if (mode_item != NULL && cJSON_IsNumber(mode_item)) {
                out->run_mode = (mode_item->valueint != 0) ? 1 : 0;
            } else {
                out->run_mode = 0; /* 默认 DEMO */
            }
        } else {
            continue; /* 未知命令类型，跳过 */
        }

        s_pending_command_count++;
        ESP_LOGI(TAG, "parsed command[%u] type=%d", s_pending_command_count - 1U, (int)out->type);
    }

    cJSON_Delete(root);
}

esp_err_t HttpAlertReporter_Init(void)
{
    s_last_handled_state = APP_STATE_NORMAL;
    s_last_handled_reason[0] = '\0';
    s_last_handled_sos_trigger_count = 0;
    s_last_report_attempt_ms = 0;
    s_initialized = true;
    return ensure_device_id();
}

esp_err_t HttpAlertReporter_Process(app_state_t state,
                                    const sensor_hub_data_t *sensor_data,
                                    const risk_result_t *risk_result)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (sensor_data == NULL || risk_result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char reason[HTTP_ALERT_REPORTER_MAX_REASON_LEN] = {0};
    build_reason(risk_result, reason, sizeof(reason));
    uint32_t sos_trigger_count = AppController_GetSosTriggerCount();
    uint32_t now_ms = (uint32_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
    report_mode_t report_mode = get_report_mode(state, reason, sos_trigger_count, now_ms);

    if (report_mode == REPORT_MODE_NONE) {
        return ESP_OK;
    }

    s_last_handled_state = state;
    strncpy(s_last_handled_reason, reason, sizeof(s_last_handled_reason) - 1U);
    s_last_handled_reason[sizeof(s_last_handled_reason) - 1U] = '\0';
    s_last_handled_sos_trigger_count = sos_trigger_count;
    s_last_report_attempt_ms = now_ms;

    if (!WiFiManager_IsConnected()) {
        ESP_LOGW(TAG,
                 "skip report, wifi not connected state=%s reason=%s",
                 AppController_StateToString(state),
                 reason[0] != '\0' ? reason : "none");
        return ESP_OK;
    }

    esp_err_t ret = ensure_device_id();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "device id unavailable: %s", esp_err_to_name(ret));
        return ESP_OK;
    }

    char escaped_reason[HTTP_ALERT_REPORTER_MAX_ESCAPED_REASON_LEN] = {0};
    if (!json_escape_string(reason, escaped_reason, sizeof(escaped_reason))) {
        ESP_LOGW(TAG, "reason json escape failed, skip report");
        return ESP_OK;
    }

    char request_body[HTTP_ALERT_REPORTER_REQUEST_BUFFER_LEN] = {0};
    int written = snprintf(
        request_body,
        sizeof(request_body),
        "{\"device_id\":\"%s\",\"state\":\"%s\",\"risk_level\":\"%s\","
        "\"reason\":\"%s\",\"temperature\":%.1f,\"humidity\":%.1f,"
        "\"lux\":%u,\"mq2_raw\":%d,"
        "\"sensor_aht20_ok\":%s,"
        "\"sensor_bh1750_ok\":%s,\"sensor_mq2_ok\":%s,"
        "\"ld2410b_ok\":%s,\"ld2410b_presence\":%s,"
        "\"ld2410b_moving_target\":%s,\"ld2410b_stationary_target\":%s,"
        "\"ld2410b_moving_distance_cm\":%u,\"ld2410b_stationary_distance_cm\":%u,"
        "\"ld2410b_detection_distance_cm\":%u,"
        "\"ld2410b_moving_energy\":%u,\"ld2410b_stationary_energy\":%u,"
        "\"mmwave_fusion_active\":%s,"
        "\"run_mode\":\"%s\",\"no_motion_remind_ms\":%u,"
        "\"remind_confirm_timeout_ms\":%u,"
        "\"remind_confirm_timeout_demo_ms\":%u,"
        "\"remind_confirm_timeout_real_ms\":%u,"
        "\"timestamp_ms\":%" PRIu32 "}",
        s_device_id,
        AppController_StateToString(state),
        RiskEngine_LevelToString(risk_result->level),
        escaped_reason,
        sensor_data->aht_temperature,
        sensor_data->humidity,
        (unsigned int)sensor_data->lux,
        sensor_data->mq2_raw,
        sensor_data->aht20_ok ? "true" : "false",
        sensor_data->bh1750_ok ? "true" : "false",
        sensor_data->mq2_ok ? "true" : "false",
        sensor_data->ld2410b_ok ? "true" : "false",
        sensor_data->ld2410b_presence ? "true" : "false",
        sensor_data->ld2410b_moving_target ? "true" : "false",
        sensor_data->ld2410b_stationary_target ? "true" : "false",
        (unsigned int)sensor_data->ld2410b_moving_distance_cm,
        (unsigned int)sensor_data->ld2410b_stationary_distance_cm,
        (unsigned int)sensor_data->ld2410b_detection_distance_cm,
        (unsigned int)sensor_data->ld2410b_moving_energy,
        (unsigned int)sensor_data->ld2410b_stationary_energy,
        sensor_data->ld2410b_ok ? "true" : "false",
        run_mode_string(),
        (unsigned int)no_motion_remind_ms(),
        (unsigned int)remind_confirm_timeout_ms(),
        (unsigned int)APP_CONTROLLER_REMIND_CONFIRM_TIMEOUT_MS_DEMO,
        (unsigned int)APP_CONTROLLER_REMIND_CONFIRM_TIMEOUT_MS_REAL,
        now_ms);
    if (written < 0 || written >= (int)sizeof(request_body)) {
        ESP_LOGW(TAG, "request body truncated, skip report");
        return ESP_OK;
    }

    int status_code = -1;
    char response_buf[HTTP_ALERT_REPORTER_RESPONSE_BUFFER_LEN] = {0};
    ret = post_alert_json(request_body, report_mode, &status_code,
                          response_buf, sizeof(response_buf));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "report failed mode=%s state=%s err=%s",
                 report_mode == REPORT_MODE_EVENT ? "event" : "telemetry",
                 AppController_StateToString(state),
                 esp_err_to_name(ret));
        return ESP_OK;
    }

    if (status_code < 200 || status_code >= 300) {
        ESP_LOGW(TAG,
                 "report rejected mode=%s state=%s http_status=%d",
                 report_mode == REPORT_MODE_EVENT ? "event" : "telemetry",
                 AppController_StateToString(state),
                 status_code);
        return ESP_OK;
    }

    /* 解析服务器下发的设备命令 */
    if (response_buf[0] != '\0') {
        parse_commands_from_response(response_buf);
    }

    ESP_LOGI(TAG,
             "report sent mode=%s state=%s risk=%s url=%s commands=%u",
             report_mode == REPORT_MODE_EVENT ? "event" : "telemetry",
             AppController_StateToString(state),
             RiskEngine_LevelToString(risk_result->level),
             HTTP_ALERT_REPORTER_URL,
             s_pending_command_count);
    return ESP_OK;
}

uint32_t HttpAlertReporter_TakePendingCommands(device_command_t *out_commands,
                                                uint32_t max_count)
{
    if (out_commands == NULL || max_count == 0U) {
        return 0U;
    }

    uint32_t count = (s_pending_command_count < max_count)
                         ? s_pending_command_count
                         : max_count;
    if (count > 0U) {
        memcpy(out_commands, s_pending_commands, count * sizeof(device_command_t));
        s_pending_command_count = 0U;
        memset(s_pending_commands, 0, sizeof(s_pending_commands));
    }
    return count;
}
