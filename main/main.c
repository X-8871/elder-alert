/**
 * @file main.c
 * @brief 系统入口 —— app_main() 初始化各模块并运行主循环。
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "driver/gpio.h"

/* ---- BSP 层 ---- */
#include "BSP_INMP441.h"
#include "BSP_LD2410B.h"
#include "BSP_MAX98357A.h"

/* ---- 中间件层 ---- */
#include "AppController.h"
#include "AlertController.h"
#include "DisplayController.h"
#include "EventLog.h"
#include "HttpAlertReporter.h"
#include "InputController.h"
#include "RainMakerReporter.h"
#include "RiskEngine.h"
#include "SensorHub.h"
#include "SpeechReplyPlayer.h"
#include "SpeechUploader.h"
#include "VoicePrompt.h"
#include "WiFiManager.h"

#define TAG "SENSOR_DEMO"  /* 串口日志标签 */

/* ================================================================
 * 编译期测试开关
 * ================================================================ */

#define ENABLE_LD2410B_UART_TEST 0       /* LD2410B 雷达 UART 独立测试模式 */
#define LD2410B_TEST_UART_NUM    UART_NUM_1
#define LD2410B_TEST_TX_GPIO     GPIO_NUM_18
#define LD2410B_TEST_RX_GPIO     GPIO_NUM_16
#define LD2410B_TEST_BAUD_RATE   BSP_LD2410B_DEFAULT_BAUD_RATE

#define ENABLE_INMP441_LEVEL_TEST  0     /* INMP441 麦克风音量串口测试模式 */

#define ENABLE_INMP441_UPLOAD_TEST 1     /* 语音上传测试（GPIO17 按键录音+上传云端 ASR） */
#define INMP441_TEST_BCLK_GPIO     GPIO_NUM_12
#define INMP441_TEST_WS_GPIO       GPIO_NUM_13
#define INMP441_TEST_DIN_GPIO      GPIO_NUM_14
#define INMP441_UPLOAD_RECORD_MS   5000U  /* 每次录音时长 5 秒 */

#define ENABLE_MAX98357A_OUTPUT_TEST 0   /* 开机功放测试音（3声滴滴） */
#define ENABLE_MAX98357A_UPLOAD_OK_PROMPT 1 /* 录音上传成功后播放云端/本地提示音 */

/* MAX98357A 功放引脚 */
#define MAX98357A_TEST_BCLK_GPIO     GPIO_NUM_12
#define MAX98357A_TEST_WS_GPIO       GPIO_NUM_13
#define MAX98357A_TEST_DOUT_GPIO     GPIO_NUM_15
#define MAX98357A_TEST_SD_GPIO       GPIO_NUM_21  /* SD=Shutdown 关断引脚 */

/* ================================================================
 * 独立测试模式函数（仅当对应宏开启时编译）
 * ================================================================ */

#if ENABLE_LD2410B_UART_TEST
/* LD2410B 雷达 UART 独立测试：直接读取模块原始数据并打印到串口 */
static const char *ld2410b_target_state_name(bsp_ld2410b_target_state_t state)
{
    switch (state) {
    case BSP_LD2410B_TARGET_NONE:                    return "none";
    case BSP_LD2410B_TARGET_MOVING:                  return "moving";
    case BSP_LD2410B_TARGET_STATIONARY:              return "stationary";
    case BSP_LD2410B_TARGET_MOVING_AND_STATIONARY:   return "moving+stationary";
    default:                                          return "unknown";
    }
}

static void run_ld2410b_uart_test(void)
{
    esp_log_level_set("*", ESP_LOG_NONE);           /* 关闭所有日志 */
    esp_log_level_set(TAG, ESP_LOG_INFO);            /* 只开本文件的日志 */
    esp_log_level_set("BSP_LD2410B", ESP_LOG_INFO);  /* 只开 LD2410B 驱动的日志 */

    const bsp_ld2410b_config_t config = {
        .uart_num = LD2410B_TEST_UART_NUM,
        .tx_gpio = LD2410B_TEST_TX_GPIO,
        .rx_gpio = LD2410B_TEST_RX_GPIO,
        .baud_rate = LD2410B_TEST_BAUD_RATE,
    };

    esp_err_t ret = BSP_LD2410B_Init(&config);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "LD2410B init failed: %s", esp_err_to_name(ret)); return; }

    ESP_LOGI(TAG, "LD2410B UART test only: module TX->GPIO16, module RX->GPIO18");
    ESP_LOGI(TAG, "Move, sit still, then leave the detection area and compare with the phone app");

    TickType_t last_print_tick = 0;
    bsp_ld2410b_status_t latest = {0};
    bool has_status = false;

    while (1) {
        bsp_ld2410b_status_t status = {0};
        ret = BSP_LD2410B_ReadStatus(&status, 250);
        if (ret == ESP_OK)       { latest = status; has_status = true; }
        else if (ret != ESP_ERR_TIMEOUT) { ESP_LOGW(TAG, "LD2410B frame parse failed: %s", esp_err_to_name(ret)); }

        TickType_t now = xTaskGetTickCount();
        if (has_status && now - last_print_tick >= pdMS_TO_TICKS(500)) {
            ESP_LOGI(TAG, "LD2410B presence=%d state=%s moving=%d moving_cm=%u moving_energy=%u"
                     " stationary=%d stationary_cm=%u stationary_energy=%u detect_cm=%u",
                     latest.presence, ld2410b_target_state_name(latest.target_state),
                     latest.moving_target, (unsigned)latest.moving_distance_cm, (unsigned)latest.moving_energy,
                     latest.stationary_target, (unsigned)latest.stationary_distance_cm, (unsigned)latest.stationary_energy,
                     (unsigned)latest.detection_distance_cm);
            last_print_tick = now;
        }
    }
}
#endif

#if ENABLE_INMP441_LEVEL_TEST
/* INMP441 麦克风音量串口测试：不断读取音量并打印 */
static void run_inmp441_level_test(void)
{
    const bsp_inmp441_config_t mic_config = {
        .bclk_gpio = INMP441_TEST_BCLK_GPIO, .ws_gpio = INMP441_TEST_WS_GPIO,
        .data_in_gpio = INMP441_TEST_DIN_GPIO, .sample_rate_hz = BSP_INMP441_DEFAULT_SAMPLE_RATE_HZ,
    };
    esp_err_t ret = BSP_INMP441_Init(&mic_config);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "INMP441 init failed: %s", esp_err_to_name(ret)); return; }

    ESP_LOGI(TAG, "INMP441 level test started, speak or clap near the microphone");
    while (1) {
        bsp_inmp441_level_t level = {0};
        ret = BSP_INMP441_ReadLevel(&level, 1000);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "INMP441 level: mean_abs=%" PRId32 " peak_abs=%" PRId32 " samples=%u",
                     level.mean_abs, level.peak_abs, (unsigned)level.sample_count);
        } else {
            ESP_LOGW(TAG, "INMP441 read failed: %s", esp_err_to_name(ret));
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
#endif

/* ---- 功放静音 ---- */

/* 拉低 MAX98357A 的 SD 引脚，防止上电瞬间杂音 */
static void mute_max98357a_early(void)
{
    gpio_reset_pin(MAX98357A_TEST_SD_GPIO);
    gpio_set_direction(MAX98357A_TEST_SD_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(MAX98357A_TEST_SD_GPIO, 0);  /* SD=低电平=关断功放 */
    ESP_LOGI(TAG, "MAX98357A SD muted on GPIO%d", MAX98357A_TEST_SD_GPIO);
}

/* ---- 语音上传测试相关 ---- */

#if ENABLE_INMP441_UPLOAD_TEST
static bool s_speech_upload_ready = false;

static const speech_uploader_config_t s_speech_config = {
    .bclk_gpio = INMP441_TEST_BCLK_GPIO, .ws_gpio = INMP441_TEST_WS_GPIO,
    .data_in_gpio = INMP441_TEST_DIN_GPIO, .sample_rate_hz = BSP_INMP441_DEFAULT_SAMPLE_RATE_HZ,
};

static void speech_upload_phase_changed(speech_uploader_phase_t phase, void *user_ctx)
{
    (void)user_ctx;
    const display_activity_t activity = phase == SPEECH_UPLOADER_PHASE_RECORDING
                                            ? DISPLAY_ACTIVITY_LISTENING
                                            : DISPLAY_ACTIVITY_PROCESSING;
    esp_err_t ret = DisplayController_SetActivity(activity);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "display activity update failed: %s", esp_err_to_name(ret));
    }
}

static void init_inmp441_upload_test(void)
{
    esp_err_t ret = SpeechUploader_Init(&s_speech_config);
    if (ret != ESP_OK) { ESP_LOGW(TAG, "SpeechUploader init failed: %s", esp_err_to_name(ret)); return; }
    SpeechUploader_SetPhaseCallback(speech_upload_phase_changed, NULL);
    s_speech_upload_ready = true;
    ESP_LOGI(TAG, "speech upload test ready, long-press OK key (GPIO7) 3s to record and upload");
}

#if ENABLE_MAX98357A_UPLOAD_OK_PROMPT
/* 录音上传成功后的提示音播报：云端 TTS → 失败回退本地人声 */
static esp_err_t play_upload_ok_prompt(void)
{
    const bsp_max98357a_config_t amp_config = {
        .bclk_gpio = MAX98357A_TEST_BCLK_GPIO, .ws_gpio = MAX98357A_TEST_WS_GPIO,
        .data_out_gpio = MAX98357A_TEST_DOUT_GPIO, .sd_gpio = MAX98357A_TEST_SD_GPIO,
        .sample_rate_hz = BSP_MAX98357A_DEFAULT_SAMPLE_RATE_HZ,
    };

    DisplayController_SetActivity(DISPLAY_ACTIVITY_SPEAKING);
    esp_err_t ret = SpeechReplyPlayer_PlayLatest(&amp_config);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "dynamic reply playback failed, fallback to local prompt: %s", esp_err_to_name(ret));
        ret = VoicePrompt_PlayUploadOk(&amp_config);  /* 回退到本地"我听到了" */
    }
    DisplayController_SetActivity(DISPLAY_ACTIVITY_NONE);
    return ret;
}
#endif

/* 播放状态播报音频。INMP441 和 MAX98357A 共享 I2S，必须先释放麦克风再播放。 */
static esp_err_t play_state_voice_prompt(const char *event_key)
{
    if (event_key == NULL || event_key[0] == '\0') { return ESP_ERR_INVALID_ARG; }

    const bsp_max98357a_config_t amp_config = {
        .bclk_gpio = MAX98357A_TEST_BCLK_GPIO, .ws_gpio = MAX98357A_TEST_WS_GPIO,
        .data_out_gpio = MAX98357A_TEST_DOUT_GPIO, .sd_gpio = MAX98357A_TEST_SD_GPIO,
        .sample_rate_hz = BSP_MAX98357A_DEFAULT_SAMPLE_RATE_HZ,
    };

    ESP_LOGI(TAG, "play state voice prompt event_key=%s", event_key);

    /* 如果 INMP441 正在占用 I2S，先释放，播放完再恢复 */
    bool need_reinit_mic = false;
#if ENABLE_INMP441_UPLOAD_TEST
    if (s_speech_upload_ready) {
        ESP_LOGI(TAG, "state voice: release INMP441 I2S for speaker playback");
        s_speech_upload_ready = false;
        SpeechUploader_Deinit();
        need_reinit_mic = true;
    }
#endif

    DisplayController_SetActivity(DISPLAY_ACTIVITY_SPEAKING);
    esp_err_t ret = SpeechReplyPlayer_PlayEventKey(&amp_config, event_key);
    DisplayController_SetActivity(DISPLAY_ACTIVITY_NONE);

#if ENABLE_INMP441_UPLOAD_TEST
    if (need_reinit_mic) {
        init_inmp441_upload_test();
        ESP_LOGI(TAG, "state voice: INMP441 I2S restored");
    }
#endif

    return ret;
}

/** 消费 AppController 产生的待播报状态语音事件 */
static void service_pending_state_voice_prompt(void)
{
    const char *event_key = AppController_TakePendingVoicePromptKey();
    if (event_key == NULL) { return; }  /* 没有新的状态过渡事件 */

    esp_err_t ret = play_state_voice_prompt(event_key);
    if (ret != ESP_OK) { ESP_LOGW(TAG, "state voice prompt failed event_key=%s err=%s", event_key, esp_err_to_name(ret)); }
}

/**
 * 语音上传周期服务：3 秒长按 → 录制 WAV → HTTP POST ASR → 半双工播放提示音
 */
static void service_inmp441_upload_test(void)
{
    if (!s_speech_upload_ready) { return; }

    bool record_requested = false;
    esp_err_t ret = InputController_GetRecordEvent(&record_requested);
    if (ret != ESP_OK) { ESP_LOGW(TAG, "record key check failed: %s", esp_err_to_name(ret)); return; }
    if (!record_requested) { return; }

    ESP_LOGI(TAG, "record key long-press triggered, record and upload speech");
    ret = SpeechUploader_RecordWavAndUpload(INMP441_UPLOAD_RECORD_MS);
    if (ret != ESP_OK) {
        DisplayController_SetActivity(DISPLAY_ACTIVITY_NONE);
        ESP_LOGW(TAG, "speech upload test failed: %s", esp_err_to_name(ret));
        return;
    }

#if ENABLE_MAX98357A_UPLOAD_OK_PROMPT
    /* 半双工切换：释放 INMP441 I2S RX → 播放提示音 → 恢复 INMP441 */
    ESP_LOGI(TAG, "speech upload success, switch I2S to speaker prompt");
    s_speech_upload_ready = false;
    ret = SpeechUploader_Deinit();
    if (ret != ESP_OK) {
        DisplayController_SetActivity(DISPLAY_ACTIVITY_NONE);
        ESP_LOGW(TAG, "SpeechUploader deinit failed, skip speaker prompt: %s", esp_err_to_name(ret));
        init_inmp441_upload_test();
        return;
    }

    ret = play_upload_ok_prompt();
    if (ret != ESP_OK) { ESP_LOGW(TAG, "upload ok prompt failed: %s", esp_err_to_name(ret)); }

    init_inmp441_upload_test();  /* 恢复录音链路 */
#else
    DisplayController_SetActivity(DISPLAY_ACTIVITY_NONE);
#endif
}
#endif

#if ENABLE_MAX98357A_OUTPUT_TEST
/* MAX98357A 功放开机测试音：播放 3 声短滴滴（1kHz 方波） */
static void run_max98357a_output_test(void)
{
    const bsp_max98357a_config_t amp_config = {
        .bclk_gpio = MAX98357A_TEST_BCLK_GPIO, .ws_gpio = MAX98357A_TEST_WS_GPIO,
        .data_out_gpio = MAX98357A_TEST_DOUT_GPIO, .sd_gpio = MAX98357A_TEST_SD_GPIO,
        .sample_rate_hz = BSP_MAX98357A_DEFAULT_SAMPLE_RATE_HZ,
    };

    esp_err_t ret = BSP_MAX98357A_Init(&amp_config);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "MAX98357A init failed: %s", esp_err_to_name(ret)); return; }

    ESP_LOGI(TAG, "MAX98357A output test started: GPIO12=BCLK GPIO13=WS GPIO15=DIN");
    for (int i = 0; i < 3; ++i) {
        ret = BSP_MAX98357A_PlayTone(1000U, 250U, BSP_MAX98357A_DEFAULT_VOLUME);
        if (ret != ESP_OK) { ESP_LOGE(TAG, "MAX98357A tone failed: %s", esp_err_to_name(ret)); break; }
        ret = BSP_MAX98357A_PlaySilence(150U);
        if (ret != ESP_OK) { ESP_LOGE(TAG, "MAX98357A silence failed: %s", esp_err_to_name(ret)); break; }
    }
    if (ret == ESP_OK) { ESP_LOGI(TAG, "MAX98357A output test finished"); }

    ret = BSP_MAX98357A_Deinit();
    if (ret != ESP_OK) { ESP_LOGW(TAG, "MAX98357A deinit failed: %s", esp_err_to_name(ret)); }
}
#endif

/* ---- 联网基础环境准备 ---- */
static esp_err_t prepare_connectivity_primitives(void)
{
    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) { return ret; }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) { return ret; }

    return ESP_OK;
}

/*
 * ================================================================
 * Agent 设备命令处理
 * ================================================================
 */
static void service_agent_commands(void)
{
    device_command_t cmds[HTTP_ALERT_REPORTER_MAX_COMMANDS] = {0};
    uint32_t count = HttpAlertReporter_TakePendingCommands(cmds, HTTP_ALERT_REPORTER_MAX_COMMANDS);
    if (count == 0U) {
        return;
    }

    for (uint32_t i = 0; i < count; ++i) {
        const device_command_t *cmd = &cmds[i];
        switch (cmd->type) {
        case DEVICE_CMD_CONFIRM_ALERT:
            ESP_LOGI(TAG, "agent command: confirm_alert");
            AppController_RemoteConfirm();
            break;

        case DEVICE_CMD_SHOW_SCREEN_MESSAGE:
            ESP_LOGI(TAG, "agent command: show_screen_message msg=%s dur=%d",
                     cmd->message, cmd->duration);
            DisplayController_ShowMessage(cmd->message,
                                          cmd->duration > 0 ? cmd->duration : 5);
            break;

        case DEVICE_CMD_BEEP_ONCE:
            ESP_LOGI(TAG, "agent command: beep_once");
            AlertController_BeepOnce();
            break;

        case DEVICE_CMD_PLAY_TTS:
            ESP_LOGI(TAG, "agent command: play_tts url=%s (logged, playback deferred)",
                     cmd->url);
            /* TTS 播放需要 I2S 半双工切换，复杂场景留待后续实现 */
            break;

        case DEVICE_CMD_SET_RUN_MODE:
            ESP_LOGI(TAG, "agent command: set_run_mode mode=%s",
                     cmd->run_mode ? "REAL" : "DEMO");
            AppController_SetRunMode(cmd->run_mode ? RISK_RUN_MODE_REAL : RISK_RUN_MODE_DEMO);
            break;

        default:
            ESP_LOGW(TAG, "agent command: unknown type=%d", (int)cmd->type);
            break;
        }
    }
}

/*
 * ================================================================
 * 独立声光闪烁任务
 * 主循环播放语音时会阻塞，此任务保证 LED/蜂鸣器持续闪烁。
 * ================================================================
 */
static void alert_blink_task(void *arg)
{
    (void)arg;
    while (1) {
        AlertController_Update();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* 主循环：2s 采样周期 + 20×100ms 服务循环。状态优先级 SOS > ALARM > REMIND > NORMAL */

void app_main(void)
{
    /* 独立测试模式入口 */
#if ENABLE_LD2410B_UART_TEST
    run_ld2410b_uart_test();
    return;
#endif
#if ENABLE_INMP441_LEVEL_TEST
    run_inmp441_level_test();
    return;
#endif

    /* 拉低功放 SD 引脚，防止上电杂音 */
    mute_max98357a_early();

#if ENABLE_MAX98357A_OUTPUT_TEST
    run_max98357a_output_test();
#endif

    /* ====== 初始化阶段（按依赖顺序）====== */

    /* 1. 传感器中枢 */
    esp_err_t ret = SensorHub_Init();
    if (ret != ESP_OK) { ESP_LOGE(TAG, "SensorHub init failed: %s", esp_err_to_name(ret)); return; }

    /* 2. 声光提醒 */
    ret = AlertController_Init();
    if (ret != ESP_OK) { ESP_LOGE(TAG, "AlertController init failed: %s", esp_err_to_name(ret)); return; }

    /* 3. 用户输入 */
    ret = InputController_Init();
    if (ret != ESP_OK) { ESP_LOGE(TAG, "InputController init failed: %s", esp_err_to_name(ret)); return; }

    /* 4. TFT 显示（失败不阻塞主流程） */
    ret = DisplayController_Init();
    if (ret != ESP_OK) { ESP_LOGW(TAG, "DisplayController init skipped: %s", esp_err_to_name(ret)); }

    /* 5. 联网基础 + Wi-Fi + RainMaker（失败不阻塞本地闭环） */
    ret = prepare_connectivity_primitives();
    if (ret != ESP_OK) { ESP_LOGE(TAG, "Connectivity primitives init failed: %s", esp_err_to_name(ret)); return; }

    ret = RainMakerReporter_Init();
    if (ret != ESP_OK) { ESP_LOGW(TAG, "RainMakerReporter init failed: %s", esp_err_to_name(ret)); }

    ret = WiFiManager_Init();
    if (ret != ESP_OK) { ESP_LOGW(TAG, "WiFiManager init failed: %s", esp_err_to_name(ret)); }

    /* 6. 应用状态机 */
    ret = AppController_Init();
    if (ret != ESP_OK) { ESP_LOGE(TAG, "AppController init failed: %s", esp_err_to_name(ret)); return; }

    /* 7. 事件日志 */
    ret = EventLog_Init();
    if (ret != ESP_OK) { ESP_LOGE(TAG, "EventLog init failed: %s", esp_err_to_name(ret)); return; }

    /* 8. HTTP 上报 */
    ret = HttpAlertReporter_Init();
    if (ret != ESP_OK) { ESP_LOGW(TAG, "HttpAlertReporter init failed: %s", esp_err_to_name(ret)); }

    /* 9. 语音上传 */
#if ENABLE_INMP441_UPLOAD_TEST
    init_inmp441_upload_test();
#endif

    /* 启动独立声光闪烁任务 */
    xTaskCreate(alert_blink_task, "alert_blink", 2048, NULL, 5, NULL);
    ESP_LOGI(TAG, "alert blink task started (50ms interval)");

    /* ====== 主业务循环 ====== */
    while (1) {
        sensor_hub_data_t sensor_data = {0};
        risk_result_t risk_result = {0};
        risk_context_t risk_context = {0};

        /* ---- 外层：2 秒采样周期 ---- */

        ret = SensorHub_Read(&sensor_data);
        if (ret == ESP_OK) {
            /* A. 更新上下文 */
            ret = AppController_UpdateContext(&sensor_data);
            if (ret != ESP_OK) { ESP_LOGE(TAG, "AppController context update failed: %s", esp_err_to_name(ret)); }

            /* B. 构建风险上下文 */
            risk_context.now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
            risk_context.inactive_ms = AppController_GetInactiveTimeMs();
            risk_context.manual_sos_active = AppController_IsSosLatched();
            risk_context.remind_timeout_active = AppController_IsRemindTimeoutLatched();
            risk_context.rest_context_active = AppController_IsRestContextActive();
            risk_context.fall_detected = AppController_IsFallDetected();

            /* C. 风险评估 */
            RiskEngine_Evaluate(&sensor_data, &risk_context, &risk_result);

            /* D. 状态机推进 */
            ret = AppController_Process(&sensor_data, &risk_result);
            if (ret != ESP_OK) { ESP_LOGE(TAG, "AppController process failed: %s", esp_err_to_name(ret)); }

            /* E. 事件日志 */
            ret = EventLog_Update(AppController_GetState(), &sensor_data, &risk_result);
            if (ret != ESP_OK) { ESP_LOGE(TAG, "EventLog update failed: %s", esp_err_to_name(ret)); }

            /* F. HTTP 云端上报 */
            ret = HttpAlertReporter_Process(AppController_GetState(), &sensor_data, &risk_result);
            if (ret != ESP_OK) { ESP_LOGW(TAG, "HttpAlertReporter process failed: %s", esp_err_to_name(ret)); }

            /* F2. Agent 设备命令 */
            service_agent_commands();

            /* G. RainMaker 上报 */
            ret = RainMakerReporter_Process(AppController_GetState(), &sensor_data, &risk_result);
            if (ret != ESP_OK) { ESP_LOGW(TAG, "RainMakerReporter process failed: %s", esp_err_to_name(ret)); }

            /* H. 更新 TFT 显示 */
            ret = DisplayController_Update(AppController_GetState(), &sensor_data, &risk_result);
            if (ret != ESP_OK) { ESP_LOGW(TAG, "DisplayController update failed: %s", esp_err_to_name(ret)); }

            /* I. 状态播报语音 */
            service_pending_state_voice_prompt();
        } else {
            ESP_LOGE(TAG, "SensorHub read failed: %s", esp_err_to_name(ret));
        }

        /* ---- 内层：20 × 100ms ≈ 2s 服务循环 ---- */
        for (int i = 0; i < 20; ++i) {
            /* 确认键长按 30 秒 → 清除配网并重启 */
            bool reset_wifi_requested = false;
            ret = InputController_GetConfirmLongPressEvent(&reset_wifi_requested);
            if (ret == ESP_OK && reset_wifi_requested) {
                ESP_LOGW(TAG, "confirm key held for 30s, reset Wi-Fi provisioning");
                ret = WiFiManager_ResetProvisioningAndRestart();
                if (ret != ESP_OK) { ESP_LOGE(TAG, "Wi-Fi provisioning reset failed: %s", esp_err_to_name(ret)); }
            } else if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Confirm long press check failed: %s", esp_err_to_name(ret));
            }

            /* 状态机服务（按键、超时升级、冷却计时） */
            ret = AppController_Service();
            if (ret != ESP_OK) { ESP_LOGE(TAG, "AppController service failed: %s", esp_err_to_name(ret)); }

            /* 检查状态语音播报 */
            service_pending_state_voice_prompt();

            /* LVGL tick */
            ret = DisplayController_Service(100);
            if (ret != ESP_OK) { ESP_LOGW(TAG, "DisplayController service failed: %s", esp_err_to_name(ret)); }

#if ENABLE_INMP441_UPLOAD_TEST
            service_inmp441_upload_test();
#endif
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}
