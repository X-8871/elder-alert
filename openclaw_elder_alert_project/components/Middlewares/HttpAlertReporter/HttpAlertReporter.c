/**
 * @file HttpAlertReporter.c
 * @brief HTTP 告警上报器实现，通过 POST JSON 向云端推送事件和遥测数据。
 *
 * 上报触发条件：
 *   - EVENT：应用状态变化、风险原因变化、SOS 新触发
 *   - TELEMETRY：距上次上报超过 10 秒
 * 不会因上报失败而阻塞主流程，失败仅记日志。
 */

#include "HttpAlertReporter.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "WiFiManager.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"

#define HTTP_ALERT_REPORTER_URL "http://118.25.94.68:3000/api/alert"
#define HTTP_ALERT_REPORTER_DEVICE_TOKEN ""
#define HTTP_ALERT_REPORTER_TIMEOUT_MS 3000
#define HTTP_ALERT_REPORTER_TELEMETRY_INTERVAL_MS 10000U
#define HTTP_ALERT_REPORTER_MAX_REASON_LEN 192
#define HTTP_ALERT_REPORTER_MAX_ESCAPED_REASON_LEN 384
#define HTTP_ALERT_REPORTER_MAX_DEVICE_ID_LEN 18
#define HTTP_ALERT_REPORTER_REQUEST_BUFFER_LEN 768

static const char *TAG = "HttpAlertReporter";

static bool s_initialized = false;
static bool s_device_id_ready = false;
static char s_device_id[HTTP_ALERT_REPORTER_MAX_DEVICE_ID_LEN] = {0};
static app_state_t s_last_handled_state = APP_STATE_NORMAL;
static char s_last_handled_reason[HTTP_ALERT_REPORTER_MAX_REASON_LEN] = {0};
static uint32_t s_last_handled_sos_trigger_count = 0;
static uint32_t s_last_report_attempt_ms = 0;

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
                                 int *status_code)
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
    if (ret == ESP_OK) {
        ret = esp_http_client_set_post_field(client, json_payload, (int)strlen(json_payload));
    }
    if (ret == ESP_OK) {
        ret = esp_http_client_perform(client);
    }

    if (status_code != NULL) {
        *status_code = ret == ESP_OK ? esp_http_client_get_status_code(client) : -1;
    }

    esp_http_client_cleanup(client);
    return ret;
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
        "\"lux\":%u,\"mq2_raw\":%d,\"motion_detected\":%s,\"timestamp_ms\":%" PRIu32 "}",
        s_device_id,
        AppController_StateToString(state),
        RiskEngine_LevelToString(risk_result->level),
        escaped_reason,
        sensor_data->aht_temperature,
        sensor_data->humidity,
        (unsigned int)sensor_data->lux,
        sensor_data->mq2_raw,
        sensor_data->motion_detected ? "true" : "false",
        now_ms);
    if (written < 0 || written >= (int)sizeof(request_body)) {
        ESP_LOGW(TAG, "request body truncated, skip report");
        return ESP_OK;
    }

    int status_code = -1;
    ret = post_alert_json(request_body, report_mode, &status_code);
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

    ESP_LOGI(TAG,
             "report sent mode=%s state=%s risk=%s url=%s",
             report_mode == REPORT_MODE_EVENT ? "event" : "telemetry",
             AppController_StateToString(state),
             RiskEngine_LevelToString(risk_result->level),
             HTTP_ALERT_REPORTER_URL);
    return ESP_OK;
}
