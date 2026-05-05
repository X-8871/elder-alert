#include "EventLog.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "EventLog";

/*
 * EventLog 采用环形缓冲区保存最近 N 条异常事件。
 * - s_head 指向“下一条要写入的位置”
 * - s_count 表示当前有效记录数
 * 当缓冲区写满后，新记录会覆盖最旧记录。
 */
static event_record_t s_records[EVENT_LOG_MAX_RECORDS];
static size_t s_head = 0;
static size_t s_count = 0;
static bool s_initialized = false;

/*
 * 下面两个变量用于“去重”：
 * 如果当前状态和原因与上一条已记录事件完全一致，
 * 就不重复写入，避免 ALARM 状态下每轮循环都刷一条相同记录。
 */
static app_state_t s_last_logged_state = APP_STATE_NORMAL;
static char s_last_logged_reason[EVENT_LOG_REASON_MAX_LEN] = {0};

static bool is_abnormal_state(app_state_t state)
{
    /* EventLog 只关心异常态，NORMAL 不需要记事件。 */
    return state == APP_STATE_REMIND ||
           state == APP_STATE_ALARM ||
           state == APP_STATE_SOS;
}

static void build_event_reason(app_state_t state,
                               const risk_result_t *risk_result,
                               char *buffer,
                               size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0) {
        return;
    }

    buffer[0] = '\0';

    switch (state) {
    case APP_STATE_REMIND:
    case APP_STATE_ALARM:
    case APP_STATE_SOS:
        /* 对异常状态优先使用 RiskEngine 生成的原因串。 */
        if (risk_result != NULL) {
            RiskEngine_BuildReasonString(risk_result, buffer, buffer_size);
        }
        /* 如果风险原因为空，则退化成 state_xxx，至少保证日志可读。 */
        if (buffer[0] == '\0') {
            snprintf(buffer, buffer_size, "state_%s", AppController_StateToString(state));
        }
        break;
    default:
        /* NORMAL 不会真正落盘，但这里仍给一个兜底字符串。 */
        snprintf(buffer, buffer_size, "normal");
        break;
    }
}

static void append_record(app_state_t state,
                          const char *reason,
                          const sensor_hub_data_t *sensor_data)
{
    event_record_t *record = &s_records[s_head];
    memset(record, 0, sizeof(*record));

    /* 时间戳使用系统启动后的运行时长，而不是绝对时钟。 */
    record->timestamp_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    record->state = state;
    if (reason != NULL) {
        strncpy(record->reason, reason, sizeof(record->reason) - 1U);
    }
    if (sensor_data != NULL) {
        /* 直接拷贝整份传感器快照，便于之后离线分析。 */
        record->sensor_snapshot = *sensor_data;
    }

    /* 写入完成后，head 向后移动一个槽位。 */
    s_head = (s_head + 1U) % EVENT_LOG_MAX_RECORDS;
    if (s_count < EVENT_LOG_MAX_RECORDS) {
        ++s_count;
    }

    ESP_LOGW(TAG,
             "event_recorded timestamp_ms=%" PRIu32 " state=%s reason=%s",
             record->timestamp_ms,
             AppController_StateToString(record->state),
             record->reason);
}

esp_err_t EventLog_Init(void)
{
    /* 启动时清空整个环形缓冲区。 */
    memset(s_records, 0, sizeof(s_records));
    s_head = 0;
    s_count = 0;
    s_last_logged_state = APP_STATE_NORMAL;
    s_last_logged_reason[0] = '\0';
    s_initialized = true;

    ESP_LOGI(TAG, "event log ready, capacity=%d", EVENT_LOG_MAX_RECORDS);
    return ESP_OK;
}

esp_err_t EventLog_Update(app_state_t state,
                          const sensor_hub_data_t *sensor_data,
                          const risk_result_t *risk_result)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (sensor_data == NULL || risk_result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!is_abnormal_state(state)) {
        /*
         * 一旦系统回到 NORMAL，就清空“上一条异常事件”的比较基准。
         * 这样下次再进入异常态时，即使原因相同，也会重新记录一条新事件。
         */
        s_last_logged_state = APP_STATE_NORMAL;
        s_last_logged_reason[0] = '\0';
        return ESP_OK;
    }

    char reason[EVENT_LOG_REASON_MAX_LEN] = {0};
    build_event_reason(state, risk_result, reason, sizeof(reason));

    if (state == s_last_logged_state &&
        strncmp(reason, s_last_logged_reason, sizeof(s_last_logged_reason)) == 0) {
        /* 状态和原因都没变化，说明还是同一类异常，不重复写。 */
        return ESP_OK;
    }

    append_record(state, reason, sensor_data);
    s_last_logged_state = state;
    strncpy(s_last_logged_reason, reason, sizeof(s_last_logged_reason) - 1U);

    return ESP_OK;
}

void EventLog_DumpRecent(void)
{
    if (!s_initialized) {
        ESP_LOGW(TAG, "event log not initialized");
        return;
    }

    /* 这里按“从最旧到最新”的顺序输出，便于人眼顺时间线阅读。 */
    ESP_LOGI(TAG, "recent_event_count=%u", (unsigned int)s_count);
    for (size_t i = 0; i < s_count; ++i) {
        event_record_t record = {0};
        if (EventLog_GetRecord(i, &record) != ESP_OK) {
            continue;
        }

        ESP_LOGI(TAG,
                 "[%u] t=%" PRIu32 "ms state=%s reason=%s temp=%.1f hum=%.1f lux=%u mq2=%d motion=%d",
                 (unsigned int)i,
                 record.timestamp_ms,
                 AppController_StateToString(record.state),
                 record.reason,
                 record.sensor_snapshot.aht_temperature,
                 record.sensor_snapshot.humidity,
                 (unsigned int)record.sensor_snapshot.lux,
                 record.sensor_snapshot.mq2_raw,
                 record.sensor_snapshot.motion_detected);
    }
}

size_t EventLog_GetCount(void)
{
    return s_count;
}

esp_err_t EventLog_GetRecord(size_t index_from_oldest, event_record_t *record)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (record == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (index_from_oldest >= s_count) {
        return ESP_ERR_NOT_FOUND;
    }

    /* oldest 指向当前环形缓冲区中最老的一条有效记录。 */
    size_t oldest = (s_head + EVENT_LOG_MAX_RECORDS - s_count) % EVENT_LOG_MAX_RECORDS;
    size_t slot = (oldest + index_from_oldest) % EVENT_LOG_MAX_RECORDS;
    *record = s_records[slot];
    return ESP_OK;
}
