#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "AppController.h"
#include "RiskEngine.h"
#include "SensorHub.h"
#include "esp_err.h"

#define EVENT_LOG_MAX_RECORDS 10
#define EVENT_LOG_REASON_MAX_LEN 192

typedef struct {
    /* 记录落库时刻，单位 ms，来源于 FreeRTOS tick 转换后的运行时间。 */
    uint32_t timestamp_ms;
    /* 事件发生时系统最终处于的应用状态。 */
    app_state_t state;
    /* 风险原因字符串，例如 high_temp / mq2_warn / manual_sos_triggered。 */
    char reason[EVENT_LOG_REASON_MAX_LEN];
    /* 事件发生瞬间的完整传感器快照，便于事后复盘。 */
    sensor_hub_data_t sensor_snapshot;
} event_record_t;

/* 初始化循环日志缓冲区。系统启动阶段调用一次即可。 */
esp_err_t EventLog_Init(void);
/* 根据当前状态和风险结果决定是否要追加一条新的异常事件记录。 */
esp_err_t EventLog_Update(app_state_t state,
                          const sensor_hub_data_t *sensor_data,
                          const risk_result_t *risk_result);
/* 按时间从旧到新打印当前缓存中的事件记录。 */
void EventLog_DumpRecent(void);
/* 获取当前已保存的事件条数。 */
size_t EventLog_GetCount(void);
/* 按“从最旧开始的序号”读取一条记录。 */
esp_err_t EventLog_GetRecord(size_t index_from_oldest, event_record_t *record);
