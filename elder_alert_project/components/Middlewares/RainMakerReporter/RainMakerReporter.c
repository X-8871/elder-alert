/**
 * @file RainMakerReporter.c
 * @brief ESP RainMaker 云端上报器实现，通过 MQTT 同步传感器数据和告警事件。
 *
 * 启动流程：WiFi 连接后调用 esp_rmaker_start()，失败时按 10 秒间隔重试。
 * 参数更新：每次 Process 调用时更新所有 RainMaker 参数并上报。
 * 时序数据：依赖 SNTP 时间同步，时间就绪后通过 Simple TS 上报传感器时序。
 * 告警推送：EVENT 模式下通过 esp_rmaker_raise_alert 发送应用通知。
 */

#include "RainMakerReporter.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "WiFiManager.h"
#include "esp_log.h"
#include "esp_rmaker_connectivity.h"
#include "esp_rmaker_core.h"
#include "esp_rmaker_mqtt.h"
#include "esp_rmaker_standard_params.h"
#include "esp_rmaker_standard_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define RAINMAKER_NODE_NAME "eldercare-terminal"
#define RAINMAKER_NODE_TYPE "esp.node.eldercare-terminal"
#define RAINMAKER_DEVICE_NAME "eldercare-monitor"
#define RAINMAKER_TELEMETRY_INTERVAL_MS 30000U
#define RAINMAKER_START_RETRY_INTERVAL_MS 10000U
#define RAINMAKER_ALERT_BUFFER_LEN 192

#define RAINMAKER_PARAM_TYPE_HUMIDITY "esp.param.humidity"
#define RAINMAKER_PARAM_TYPE_TEMPERATURE "esp.param.temperature"
#define RAINMAKER_PARAM_TYPE_ILLUMINANCE "esp.param.illuminance"
#define RAINMAKER_PARAM_TYPE_MQ2_RAW "esp.param.mq2-raw"
#define RAINMAKER_PARAM_TYPE_MOTION "esp.param.motion-detected"
#define RAINMAKER_PARAM_TYPE_STATE "esp.param.state"
#define RAINMAKER_PARAM_TYPE_RISK "esp.param.risk-level"
#define RAINMAKER_PARAM_TYPE_REASON "esp.param.reason"

static const char *TAG = "RainMakerReporter";

typedef struct {
    esp_rmaker_node_t *node;
    esp_rmaker_device_t *device;
    esp_rmaker_param_t *temperature;
    esp_rmaker_param_t *humidity;
    esp_rmaker_param_t *illuminance;
    esp_rmaker_param_t *mq2_raw;
    esp_rmaker_param_t *motion_detected;
    esp_rmaker_param_t *state;
    esp_rmaker_param_t *risk_level;
    esp_rmaker_param_t *reason;
} rainmaker_handles_t;

static bool s_initialized = false;
static bool s_started = false;
static bool s_time_sync_wait_logged = false;
static esp_err_t s_init_error = ESP_OK;
static app_state_t s_last_handled_state = APP_STATE_NORMAL;
static char s_last_handled_reason[RAINMAKER_ALERT_BUFFER_LEN] = {0};
static uint32_t s_last_handled_sos_trigger_count = 0;
static uint32_t s_last_report_attempt_ms = 0;
static uint32_t s_last_start_attempt_ms = 0;
static rainmaker_handles_t s_handles = {0};

typedef enum {
    REPORT_MODE_NONE = 0,
    REPORT_MODE_EVENT,
    REPORT_MODE_TELEMETRY,
} report_mode_t;

static bool should_send_periodic_report(uint32_t now_ms)
{
    return s_last_report_attempt_ms == 0 ||
           (now_ms - s_last_report_attempt_ms) >= RAINMAKER_TELEMETRY_INTERVAL_MS;
}

static bool should_retry_start(uint32_t now_ms)
{
    return s_last_start_attempt_ms == 0 ||
           (now_ms - s_last_start_attempt_ms) >= RAINMAKER_START_RETRY_INTERVAL_MS;
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

static esp_err_t log_init_failure(const char *stage, esp_err_t err)
{
    s_init_error = err;
    ESP_LOGW(TAG, "init failed at %s: %s", stage, esp_err_to_name(err));
    return err;
}

static bool is_time_ready(void)
{
    time_t now = time(NULL);
    return now >= 1577836800;
}

static esp_err_t create_params(void)
{
    s_handles.temperature = esp_rmaker_param_create(ESP_RMAKER_DEF_TEMPERATURE_NAME,
                                                    RAINMAKER_PARAM_TYPE_TEMPERATURE,
                                                    esp_rmaker_float(0.0f),
                                                    PROP_FLAG_READ | PROP_FLAG_SIMPLE_TIME_SERIES);
    s_handles.humidity = esp_rmaker_param_create("Humidity",
                                                 RAINMAKER_PARAM_TYPE_HUMIDITY,
                                                 esp_rmaker_float(0.0f),
                                                 PROP_FLAG_READ | PROP_FLAG_SIMPLE_TIME_SERIES);
    s_handles.illuminance = esp_rmaker_param_create("Illuminance",
                                                    RAINMAKER_PARAM_TYPE_ILLUMINANCE,
                                                    esp_rmaker_int(0),
                                                    PROP_FLAG_READ | PROP_FLAG_SIMPLE_TIME_SERIES);
    s_handles.mq2_raw = esp_rmaker_param_create("MQ2Raw",
                                                RAINMAKER_PARAM_TYPE_MQ2_RAW,
                                                esp_rmaker_int(0),
                                                PROP_FLAG_READ | PROP_FLAG_SIMPLE_TIME_SERIES);
    s_handles.motion_detected = esp_rmaker_param_create("MotionDetected",
                                                        RAINMAKER_PARAM_TYPE_MOTION,
                                                        esp_rmaker_bool(false),
                                                        PROP_FLAG_READ | PROP_FLAG_SIMPLE_TIME_SERIES);
    s_handles.state = esp_rmaker_param_create("State",
                                              RAINMAKER_PARAM_TYPE_STATE,
                                              esp_rmaker_str("NORMAL"),
                                              PROP_FLAG_READ);
    s_handles.risk_level = esp_rmaker_param_create("RiskLevel",
                                                   RAINMAKER_PARAM_TYPE_RISK,
                                                   esp_rmaker_str("NORMAL"),
                                                   PROP_FLAG_READ);
    s_handles.reason = esp_rmaker_param_create("Reason",
                                               RAINMAKER_PARAM_TYPE_REASON,
                                               esp_rmaker_str("state_unknown"),
                                               PROP_FLAG_READ);

    if (s_handles.temperature == NULL || s_handles.humidity == NULL || s_handles.illuminance == NULL ||
        s_handles.mq2_raw == NULL || s_handles.motion_detected == NULL || s_handles.state == NULL ||
        s_handles.risk_level == NULL || s_handles.reason == NULL) {
        return ESP_FAIL;
    }

    esp_rmaker_param_add_ui_type(s_handles.temperature, ESP_RMAKER_UI_TEXT);
    esp_rmaker_param_add_ui_type(s_handles.humidity, ESP_RMAKER_UI_TEXT);
    esp_rmaker_param_add_ui_type(s_handles.illuminance, ESP_RMAKER_UI_TEXT);
    esp_rmaker_param_add_ui_type(s_handles.mq2_raw, ESP_RMAKER_UI_TEXT);
    esp_rmaker_param_add_ui_type(s_handles.motion_detected, ESP_RMAKER_UI_TEXT);
    esp_rmaker_param_add_ui_type(s_handles.state, ESP_RMAKER_UI_TEXT);
    esp_rmaker_param_add_ui_type(s_handles.risk_level, ESP_RMAKER_UI_TEXT);
    esp_rmaker_param_add_ui_type(s_handles.reason, ESP_RMAKER_UI_TEXT);
    return ESP_OK;
}

static esp_err_t add_params_to_device(void)
{
    esp_err_t ret = esp_rmaker_device_add_param(s_handles.device,
                                                esp_rmaker_name_param_create(ESP_RMAKER_DEF_NAME_PARAM,
                                                                             RAINMAKER_DEVICE_NAME));
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_rmaker_device_add_param(s_handles.device, s_handles.temperature);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = esp_rmaker_device_add_param(s_handles.device, s_handles.humidity);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = esp_rmaker_device_add_param(s_handles.device, s_handles.illuminance);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = esp_rmaker_device_add_param(s_handles.device, s_handles.mq2_raw);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = esp_rmaker_device_add_param(s_handles.device, s_handles.motion_detected);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = esp_rmaker_device_add_param(s_handles.device, s_handles.state);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = esp_rmaker_device_add_param(s_handles.device, s_handles.risk_level);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = esp_rmaker_device_add_param(s_handles.device, s_handles.reason);
    if (ret != ESP_OK) {
        return ret;
    }

    return esp_rmaker_device_assign_primary_param(s_handles.device, s_handles.state);
}

static esp_err_t update_latest_params(app_state_t state,
                                      const sensor_hub_data_t *sensor_data,
                                      const risk_result_t *risk_result,
                                      const char *reason)
{
    esp_err_t ret = esp_rmaker_param_update(s_handles.state,
                                            esp_rmaker_str(AppController_StateToString(state)));
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_rmaker_param_update(s_handles.risk_level,
                                  esp_rmaker_str(RiskEngine_LevelToString(risk_result->level)));
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_rmaker_param_update(s_handles.reason, esp_rmaker_str(reason));
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_rmaker_param_update(s_handles.temperature, esp_rmaker_float(sensor_data->aht_temperature));
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_rmaker_param_update(s_handles.humidity, esp_rmaker_float(sensor_data->humidity));
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_rmaker_param_update(s_handles.illuminance, esp_rmaker_int((int)sensor_data->lux));
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_rmaker_param_update(s_handles.mq2_raw, esp_rmaker_int(sensor_data->mq2_raw));
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_rmaker_param_update(s_handles.motion_detected,
                                  esp_rmaker_bool(sensor_data->motion_detected));
    if (ret != ESP_OK) {
        return ret;
    }

    return esp_rmaker_report_updated_params();
}

static void report_simple_time_series(const sensor_hub_data_t *sensor_data)
{
    if (!is_time_ready()) {
        if (!s_time_sync_wait_logged) {
            ESP_LOGI(TAG, "time not ready yet, skip simple ts until SNTP sync completes");
            s_time_sync_wait_logged = true;
        }
        return;
    }

    if (s_time_sync_wait_logged) {
        ESP_LOGI(TAG, "time sync ready, simple ts reporting enabled");
        s_time_sync_wait_logged = false;
    }

    esp_err_t ret = esp_rmaker_param_report_simple_ts_data(s_handles.temperature,
                                                           esp_rmaker_float(sensor_data->aht_temperature),
                                                           0,
                                                           0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "temperature simple ts skipped: %s", esp_err_to_name(ret));
    }

    ret = esp_rmaker_param_report_simple_ts_data(s_handles.humidity,
                                                 esp_rmaker_float(sensor_data->humidity),
                                                 0,
                                                 0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "humidity simple ts skipped: %s", esp_err_to_name(ret));
    }

    ret = esp_rmaker_param_report_simple_ts_data(s_handles.illuminance,
                                                 esp_rmaker_int((int)sensor_data->lux),
                                                 0,
                                                 0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "illuminance simple ts skipped: %s", esp_err_to_name(ret));
    }

    ret = esp_rmaker_param_report_simple_ts_data(s_handles.mq2_raw,
                                                 esp_rmaker_int(sensor_data->mq2_raw),
                                                 0,
                                                 0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "mq2 simple ts skipped: %s", esp_err_to_name(ret));
    }

    ret = esp_rmaker_param_report_simple_ts_data(s_handles.motion_detected,
                                                 esp_rmaker_bool(sensor_data->motion_detected),
                                                 0,
                                                 0);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "motion simple ts skipped: %s", esp_err_to_name(ret));
    }
}

static void raise_alert_if_needed(app_state_t state, const char *reason)
{
    if (state == APP_STATE_NORMAL) {
        return;
    }

    char alert[RAINMAKER_ALERT_BUFFER_LEN] = {0};
    snprintf(alert,
             sizeof(alert),
             "%s:%s",
             AppController_StateToString(state),
             (reason != NULL && reason[0] != '\0') ? reason : "state_unknown");

    esp_err_t ret = esp_rmaker_raise_alert(alert);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "raise alert failed: %s", esp_err_to_name(ret));
    }
}

esp_err_t RainMakerReporter_Init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    if (s_init_error != ESP_OK) {
        return s_init_error;
    }

    esp_rmaker_config_t rainmaker_cfg = {
        .enable_time_sync = true,
    };

    s_handles.node = esp_rmaker_node_init(&rainmaker_cfg, RAINMAKER_NODE_NAME, RAINMAKER_NODE_TYPE);
    if (s_handles.node == NULL) {
        return log_init_failure("esp_rmaker_node_init", ESP_FAIL);
    }

    s_handles.device = esp_rmaker_device_create(RAINMAKER_DEVICE_NAME, ESP_RMAKER_DEVICE_OTHER, NULL);
    if (s_handles.device == NULL) {
        return log_init_failure("esp_rmaker_device_create", ESP_FAIL);
    }

    esp_err_t ret = create_params();
    if (ret != ESP_OK) {
        return log_init_failure("create_params", ret);
    }

    ret = add_params_to_device();
    if (ret != ESP_OK) {
        return log_init_failure("add_params_to_device", ret);
    }

    ret = esp_rmaker_node_add_device(s_handles.node, s_handles.device);
    if (ret != ESP_OK) {
        return log_init_failure("esp_rmaker_node_add_device", ret);
    }

    ret = esp_rmaker_connectivity_enable();
    if (ret != ESP_OK) {
        return log_init_failure("esp_rmaker_connectivity_enable", ret);
    }

    s_last_handled_state = APP_STATE_NORMAL;
    s_last_handled_reason[0] = '\0';
    s_last_handled_sos_trigger_count = 0;
    s_last_report_attempt_ms = 0;
    s_last_start_attempt_ms = 0;
    s_time_sync_wait_logged = false;
    s_init_error = ESP_OK;
    s_initialized = true;
    return ESP_OK;
}

esp_err_t RainMakerReporter_Process(app_state_t state,
                                    const sensor_hub_data_t *sensor_data,
                                    const risk_result_t *risk_result)
{
    if (!s_initialized) {
        return s_init_error != ESP_OK ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    if (sensor_data == NULL || risk_result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t now_ms = (uint32_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (!s_started) {
        if (!WiFiManager_IsConnected()) {
            return ESP_OK;
        }
        if (!should_retry_start(now_ms)) {
            return ESP_OK;
        }

        s_last_start_attempt_ms = now_ms;
        esp_err_t ret = esp_rmaker_start();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "esp_rmaker_start failed: %s", esp_err_to_name(ret));
            return ESP_OK;
        }
        s_started = true;
        ESP_LOGI(TAG, "RainMaker started");
    }

    if (!esp_rmaker_is_mqtt_connected()) {
        return ESP_OK;
    }

    char reason[RAINMAKER_ALERT_BUFFER_LEN] = {0};
    build_reason(risk_result, reason, sizeof(reason));
    uint32_t sos_trigger_count = AppController_GetSosTriggerCount();
    report_mode_t report_mode = get_report_mode(state, reason, sos_trigger_count, now_ms);
    if (report_mode == REPORT_MODE_NONE) {
        return ESP_OK;
    }

    s_last_handled_state = state;
    strlcpy(s_last_handled_reason, reason, sizeof(s_last_handled_reason));
    s_last_handled_sos_trigger_count = sos_trigger_count;
    s_last_report_attempt_ms = now_ms;

    esp_err_t ret = update_latest_params(state, sensor_data, risk_result, reason);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "latest param report failed: %s", esp_err_to_name(ret));
        return ESP_OK;
    }

    report_simple_time_series(sensor_data);

    if (report_mode == REPORT_MODE_EVENT) {
        raise_alert_if_needed(state, reason);
    }

    ESP_LOGI(TAG,
             "report sent mode=%s state=%s risk=%s",
             report_mode == REPORT_MODE_EVENT ? "event" : "telemetry",
             AppController_StateToString(state),
             RiskEngine_LevelToString(risk_result->level));
    return ESP_OK;
}
