/**
 * @file main.c
 * @brief 系统入口 —— app_main() 是整个固件的"总指挥部"。
 *
 * 【学弟必读：这个文件做什么？】
 * 把 main.c 想象成一个乐队的指挥：
 * - 她不亲自演奏任何乐器（不直接操作 GPIO/I2C/ADC）
 * - 她告诉各个声部什么时候进、什么时候停（初始化顺序、主循环调度）
 * - 她确保整个乐队节奏统一（外层 2s 采样 + 内层 100ms 服务）
 *
 * 代码阅读顺序建议：
 *   1. 先看本文件顶部的"系统总架构说明"
 *   2. 看 app_main() 的 9 步初始化顺序
 *   3. 看主循环 while(1) 的执行流程
 *   4. 再跳到 components/Middlewares/ 看各模块具体实现
 *   5. 最后看 components/BSP/ 的硬件驱动
 */

#include "freertos/FreeRTOS.h"   /* FreeRTOS 内核（任务调度、tick、延时） */
#include "freertos/task.h"       /* vTaskDelay / xTaskGetTickCount */
#include "esp_err.h"             /* ESP-IDF 统一错误码 */
#include "esp_event.h"           /* 事件驱动框架 */
#include "esp_log.h"             /* 日志宏 ESP_LOGI/ESP_LOGW/ESP_LOGE */
#include "esp_netif.h"           /* 网络接口框架（Wi-Fi 联网需要） */
#include "driver/gpio.h"         /* GPIO 驱动 */

/* ---- 本项目 BSP 层（硬件驱动）---- */
#include "BSP_INMP441.h"         /* I2S 麦克风 */
#include "BSP_LD2410B.h"         /* 毫米波人体存在雷达 */
#include "BSP_MAX98357A.h"       /* I2S 功放+喇叭 */

/* ---- 本项目中间件层（业务编排）---- */
#include "AppController.h"       /* 应用状态机 NORMAL/REMIND/ALARM/SOS */
#include "AlertController.h"     /* 声光提醒控制 */
#include "DisplayController.h"   /* LVGL TFT 显示 */
#include "EventLog.h"            /* 异常事件日志 */
#include "HttpAlertReporter.h"   /* HTTP 云端上报 */
#include "InputController.h"     /* 按键事件封装 */
#include "RainMakerReporter.h"   /* RainMaker 备选上报 */
#include "RiskEngine.h"          /* 风险评估引擎 */
#include "SensorHub.h"           /* 传感器统一采集中枢 */
#include "SpeechReplyPlayer.h"   /* 云端 TTS 音频下载播放 */
#include "SpeechUploader.h"      /* 语音录制+上传 ASR */
#include "VoicePrompt.h"         /* 本地固定人声提示 */
#include "WiFiManager.h"         /* Wi-Fi STA + BLE 配网 + SNTP */

#define TAG "SENSOR_DEMO"  /* 串口日志标签 */

/* ================================================================
 * 测试开关：编译期可以单独启用某个硬件模块的独立测试
 *
 * 正常业务运行时这些应该全部为 0（除了语音上传测试）。
 * 需要单独调试某个模块时，把对应宏改为 1 重新编译烧录。
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
#define INMP441_UPLOAD_RECORD_MS   3000U  /* 每次录音时长 3 秒 */

#define ENABLE_MAX98357A_OUTPUT_TEST 0   /* 开机功放测试音（3声滴滴） */
#define ENABLE_MAX98357A_UPLOAD_OK_PROMPT 1 /* 录音上传成功后播放云端/本地提示音 */

/* MAX98357A 功放引脚 */
#define MAX98357A_TEST_BCLK_GPIO     GPIO_NUM_12
#define MAX98357A_TEST_WS_GPIO       GPIO_NUM_13
#define MAX98357A_TEST_DOUT_GPIO     GPIO_NUM_15
#define MAX98357A_TEST_SD_GPIO       GPIO_NUM_21  /* SD=Shutdown 关断引脚，注意不能和 I2C_SDA(GPIO4) 共用 */

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

/** 在系统启动早期拉低 MAX98357A 的 SD 引脚，防止上电瞬间喇叭发出 POP 声或嗡嗡声 */
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

static void init_inmp441_upload_test(void)
{
    esp_err_t ret = SpeechUploader_Init(&s_speech_config);
    if (ret != ESP_OK) { ESP_LOGW(TAG, "SpeechUploader init failed: %s", esp_err_to_name(ret)); return; }
    s_speech_upload_ready = true;
    ESP_LOGI(TAG, "speech upload test ready, press GPIO17 record key to upload a short WAV");
}

#if ENABLE_MAX98357A_UPLOAD_OK_PROMPT
/**
 * 录音上传成功后的提示音播报：
 * 优先尝试云端动态 TTS 回复音频 → 失败则回退到本地固定中文人声"我听到了"
 */
static esp_err_t play_upload_ok_prompt(void)
{
    const bsp_max98357a_config_t amp_config = {
        .bclk_gpio = MAX98357A_TEST_BCLK_GPIO, .ws_gpio = MAX98357A_TEST_WS_GPIO,
        .data_out_gpio = MAX98357A_TEST_DOUT_GPIO, .sd_gpio = MAX98357A_TEST_SD_GPIO,
        .sample_rate_hz = BSP_MAX98357A_DEFAULT_SAMPLE_RATE_HZ,
    };

    esp_err_t ret = SpeechReplyPlayer_PlayLatest(&amp_config);
    if (ret == ESP_OK) { return ESP_OK; }  /* 云端 TTS 播放成功 */

    ESP_LOGW(TAG, "dynamic reply playback failed, fallback to local prompt: %s", esp_err_to_name(ret));
    return VoicePrompt_PlayUploadOk(&amp_config);  /* 回退到本地"我听到了" */
}
#endif

/* 播放状态播报音频（云端 TTS 生成的状态过渡语音） */
static esp_err_t play_state_voice_prompt(const char *event_key)
{
    if (event_key == NULL || event_key[0] == '\0') { return ESP_ERR_INVALID_ARG; }

    const bsp_max98357a_config_t amp_config = {
        .bclk_gpio = MAX98357A_TEST_BCLK_GPIO, .ws_gpio = MAX98357A_TEST_WS_GPIO,
        .data_out_gpio = MAX98357A_TEST_DOUT_GPIO, .sd_gpio = MAX98357A_TEST_SD_GPIO,
        .sample_rate_hz = BSP_MAX98357A_DEFAULT_SAMPLE_RATE_HZ,
    };

    ESP_LOGI(TAG, "play state voice prompt event_key=%s", event_key);
    return SpeechReplyPlayer_PlayEventKey(&amp_config, event_key);
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
 * 语音上传测试的周期性服务函数：
 * 检测 GPIO17 录音键 → 录制 3 秒 WAV → HTTP POST 到云端 ASR
 * → 半双工切换到 MAX98357A → 播放云端或本地提示音 → 恢复录音链路
 */
static void service_inmp441_upload_test(void)
{
    if (!s_speech_upload_ready) { return; }

    bool record_requested = false;
    esp_err_t ret = InputController_GetRecordEvent(&record_requested);
    if (ret != ESP_OK) { ESP_LOGW(TAG, "record key check failed: %s", esp_err_to_name(ret)); return; }
    if (!record_requested) { return; }

    ESP_LOGI(TAG, "record key pressed, record and upload speech");
    ret = SpeechUploader_RecordWavAndUpload(INMP441_UPLOAD_RECORD_MS);
    if (ret != ESP_OK) { ESP_LOGW(TAG, "speech upload test failed: %s", esp_err_to_name(ret)); return; }

#if ENABLE_MAX98357A_UPLOAD_OK_PROMPT
    /* 半双工切换：释放 INMP441 I2S RX → 播放提示音 → 恢复 INMP441 */
    ESP_LOGI(TAG, "speech upload success, switch I2S to speaker prompt");
    s_speech_upload_ready = false;
    ret = SpeechUploader_Deinit();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SpeechUploader deinit failed, skip speaker prompt: %s", esp_err_to_name(ret));
        init_inmp441_upload_test();
        return;
    }

    ret = play_upload_ok_prompt();
    if (ret != ESP_OK) { ESP_LOGW(TAG, "upload ok prompt failed: %s", esp_err_to_name(ret)); }

    init_inmp441_upload_test();  /* 恢复录音链路 */
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
 * 系统总架构说明
 *
 * 这份工程按 "main → Middlewares → BSP" 三层来组织。
 * 读代码时建议也按这个顺序往下看，因为控制权就是这样一层层往下分发的。
 *
 * 1. 顶层 main
 *    `app_main()` 只做总调度，不直接碰具体硬件。
 *    它负责：
 *    - 按顺序初始化所有中间层模块
 *    - 周期性拉取一次完整传感器快照
 *    - 把快照送给风险判断模块
 *    - 把风险结果送给应用状态机
 *    - 再把状态同步给事件日志和显示模块
 *    - 在两次采样之间，持续处理按键和声光提醒刷新
 *
 * 2. 中间件层
 *    这一层负责"业务编排"，把多个底层硬件能力组合成可用的系统行为。
 *
 *    - SensorHub：统一管理 AHT20/BH1750/MQ2/LD2410B
 *    - RiskEngine：根据传感器数据和上下文做风险判定
 *    - AppController：状态机核心，管理 NORMAL/REMIND/ALARM/SOS
 *    - AlertController：状态→声光提醒的翻译器
 *    - InputController：确认键/SOS键/录音键事件封装
 *    - DisplayController：LVGL TFT 界面渲染
 *    - EventLog：异常事件环形缓冲区
 *    - WiFiManager：Wi-Fi + BLE配网 + SNTP 授时
 *    - HttpAlertReporter：HTTP POST JSON 到腾讯云
 *    - RainMakerReporter：ESP RainMaker 备选上报
 *    - SpeechUploader：INMP441 录音 + WAV 上传 ASR
 *    - SpeechReplyPlayer：云端 TTS WAV 下载 + MAX98357A 播放
 *    - VoicePrompt：本地固定 PCM 语音
 *
 * 3. BSP 底层
 *    这一层直接操作具体硬件，是"设备驱动适配层"。
 *
 *    - BSP_I2C：共享 I2C 总线
 *    - BSP_AHT20/BSP_BH1750：I2C 传感器
 *    - BSP_MQ2：ADC 烟雾/气体采样
 *    - BSP_LD2410B：毫米波人体存在 UART 驱动
 *    - BSP_INMP441：I2S MEMS 麦克风驱动
 *    - BSP_MAX98357A：I2S D类功放驱动
 *    - BSP_Alert：LED + 蜂鸣器输出
 *    - BSP_OLED：SSD1306 备选显示
 *    - BSP_TFT：ST7735 SPI 彩屏
 *    - KEY：按键扫描与中断事件
 *
 * 4. 一次完整主循环的控制流
 *    app_main() 外层 while(1) 每约 2 秒执行：
 *    1) SensorHub_Read()          → 采集一帧完整传感器数据
 *    2) AppController_UpdateContext() → 更新活动/休息上下文
 *    3) RiskEngine_Evaluate()     → 计算当前风险等级与原因
 *    4) AppController_Process()   → 根据风险结果更新状态机
 *    5) EventLog_Update()         → 异常状态记录
 *    6) HttpAlertReporter_Process() → 云端上报
 *    7) DisplayController_Update() → 刷新 TFT 显示
 *    8) 内层 20×100ms 服务循环：
 *       - 检测确认键长按 8s（重置 Wi-Fi）
 *       - AppController_Service()（按键/超时/冷却处理）
 *       - 消费状态播报语音事件
 *       - DisplayController_Service(100)（推进 LVGL）
 *       - 语音上传测试服务
 *
 * 5. 状态优先级
 *    SOS > ALARM > REMIND > NORMAL
 *
 * 6. 为什么分成"外层 2s 采样 + 内层 100ms 服务"
 *    - 外层采样周期负责环境感知，不需要太快（温度/光照变化很慢）
 *    - 内层服务周期负责交互响应，必须更及时（按键要立即响应）
 *    这样既能控制采样节奏，又能保证按键和提醒模式切换不迟钝。
 * ================================================================
 */

void app_main(void)
{
    /* 独立测试模式：如果编译时打开了测试宏，直接进入测试入口不跑主业务 */
#if ENABLE_LD2410B_UART_TEST
    run_ld2410b_uart_test();
    return;
#endif
#if ENABLE_INMP441_LEVEL_TEST
    run_inmp441_level_test();
    return;
#endif

    /* 最早做的事：拉低功放 SD 引脚，防止上电瞬间杂音 */
    mute_max98357a_early();

#if ENABLE_MAX98357A_OUTPUT_TEST
    run_max98357a_output_test();
#endif

    /* ====== 初始化阶段（按依赖顺序，共 9 步）====== */

    /* 第 1 步：传感器中枢——初始化所有数据采集链路（I2C/AHT20/BH1750/MQ2/LD2410B） */
    esp_err_t ret = SensorHub_Init();
    if (ret != ESP_OK) { ESP_LOGE(TAG, "SensorHub init failed: %s", esp_err_to_name(ret)); return; }

    /* 第 2 步：声光提醒——初始化 LED + 蜂鸣器 */
    ret = AlertController_Init();
    if (ret != ESP_OK) { ESP_LOGE(TAG, "AlertController init failed: %s", esp_err_to_name(ret)); return; }

    /* 第 3 步：用户输入——初始化确认键/SOS键/录音键 */
    ret = InputController_Init();
    if (ret != ESP_OK) { ESP_LOGE(TAG, "InputController init failed: %s", esp_err_to_name(ret)); return; }

    /* 第 4 步：TFT 显示（失败不阻塞主流程，只是没有屏幕输出） */
    ret = DisplayController_Init();
    if (ret != ESP_OK) { ESP_LOGW(TAG, "DisplayController init skipped: %s", esp_err_to_name(ret)); }

    /* 第 5 步：联网基础 + Wi-Fi + RainMaker（失败不阻塞本地闭环） */
    ret = prepare_connectivity_primitives();
    if (ret != ESP_OK) { ESP_LOGE(TAG, "Connectivity primitives init failed: %s", esp_err_to_name(ret)); return; }

    ret = RainMakerReporter_Init();
    if (ret != ESP_OK) { ESP_LOGW(TAG, "RainMakerReporter init failed: %s", esp_err_to_name(ret)); }

    ret = WiFiManager_Init();
    if (ret != ESP_OK) { ESP_LOGW(TAG, "WiFiManager init failed: %s", esp_err_to_name(ret)); }

    /* 第 6 步：应用状态机——系统从 NORMAL 状态起步 */
    ret = AppController_Init();
    if (ret != ESP_OK) { ESP_LOGE(TAG, "AppController init failed: %s", esp_err_to_name(ret)); return; }

    /* 第 7 步：事件日志——准备记录异常事件 */
    ret = EventLog_Init();
    if (ret != ESP_OK) { ESP_LOGE(TAG, "EventLog init failed: %s", esp_err_to_name(ret)); return; }

    /* 第 8 步：HTTP 上报——初始化设备 ID 和上报状态 */
    ret = HttpAlertReporter_Init();
    if (ret != ESP_OK) { ESP_LOGW(TAG, "HttpAlertReporter init failed: %s", esp_err_to_name(ret)); }

    /* 第 9 步：语音上传测试（如果编译时开启） */
#if ENABLE_INMP441_UPLOAD_TEST
    init_inmp441_upload_test();
#endif

    /* ====== 主业务循环 ====== */
    while (1) {
        sensor_hub_data_t sensor_data = {0};
        risk_result_t risk_result = {0};
        risk_context_t risk_context = {0};

        /* ---- 外层：2 秒一个完整采样周期 ---- */

        ret = SensorHub_Read(&sensor_data);
        if (ret == ESP_OK) {
            /* A. 更新上下文（活动时间、休息判断、距离追踪） */
            ret = AppController_UpdateContext(&sensor_data);
            if (ret != ESP_OK) { ESP_LOGE(TAG, "AppController context update failed: %s", esp_err_to_name(ret)); }

            /* B. 构建风险上下文 */
            risk_context.now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
            risk_context.inactive_ms = AppController_GetInactiveTimeMs();
            risk_context.manual_sos_active = AppController_IsSosLatched();
            risk_context.remind_timeout_active = AppController_IsRemindTimeoutLatched();
            risk_context.rest_context_active = AppController_IsRestContextActive();

            /* C. 风险评估：把传感器数据 + 上下文 → 风险结论 */
            RiskEngine_Evaluate(&sensor_data, &risk_context, &risk_result);

            /* D. 状态机推进：根据风险结论更新 NORMAL/REMIND/ALARM/SOS */
            ret = AppController_Process(&sensor_data, &risk_result);
            if (ret != ESP_OK) { ESP_LOGE(TAG, "AppController process failed: %s", esp_err_to_name(ret)); }

            /* E. 事件日志：异常状态变化时记录现场快照 */
            ret = EventLog_Update(AppController_GetState(), &sensor_data, &risk_result);
            if (ret != ESP_OK) { ESP_LOGE(TAG, "EventLog update failed: %s", esp_err_to_name(ret)); }

            /* F. HTTP 云端上报（事件 + 周期遥测） */
            ret = HttpAlertReporter_Process(AppController_GetState(), &sensor_data, &risk_result);
            if (ret != ESP_OK) { ESP_LOGW(TAG, "HttpAlertReporter process failed: %s", esp_err_to_name(ret)); }

            /* G. RainMaker 上报（备选路线） */
            ret = RainMakerReporter_Process(AppController_GetState(), &sensor_data, &risk_result);
            if (ret != ESP_OK) { ESP_LOGW(TAG, "RainMakerReporter process failed: %s", esp_err_to_name(ret)); }

            /* H. 更新 TFT 显示：始终根据最终状态来刷新 */
            ret = DisplayController_Update(AppController_GetState(), &sensor_data, &risk_result);
            if (ret != ESP_OK) { ESP_LOGW(TAG, "DisplayController update failed: %s", esp_err_to_name(ret)); }

            /* I. 消费状态播报语音事件 */
            service_pending_state_voice_prompt();
        } else {
            ESP_LOGE(TAG, "SensorHub read failed: %s", esp_err_to_name(ret));
        }

        /* ---- 内层：20 × 100ms ≈ 2s 高频服务循环 ---- */
        for (int i = 0; i < 20; ++i) {
            /* 检测确认键长按 8 秒 → 清除 Wi-Fi 配网信息并重启 */
            bool reset_wifi_requested = false;
            ret = InputController_GetConfirmLongPressEvent(&reset_wifi_requested);
            if (ret == ESP_OK && reset_wifi_requested) {
                ESP_LOGW(TAG, "confirm key held for 8s, reset Wi-Fi provisioning");
                ret = WiFiManager_ResetProvisioningAndRestart();
                if (ret != ESP_OK) { ESP_LOGE(TAG, "Wi-Fi provisioning reset failed: %s", esp_err_to_name(ret)); }
            } else if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Confirm long press check failed: %s", esp_err_to_name(ret));
            }

            /* 状态机高频服务（按键处理、超时升级、冷却计时、声光刷新） */
            ret = AppController_Service();
            if (ret != ESP_OK) { ESP_LOGE(TAG, "AppController service failed: %s", esp_err_to_name(ret)); }

            /* 再次检查有无状态的语音播报要播放 */
            service_pending_state_voice_prompt();

            /* 推进 LVGL tick（100ms） */
            ret = DisplayController_Service(100);
            if (ret != ESP_OK) { ESP_LOGW(TAG, "DisplayController service failed: %s", esp_err_to_name(ret)); }

#if ENABLE_INMP441_UPLOAD_TEST
            service_inmp441_upload_test();
#endif
            vTaskDelay(pdMS_TO_TICKS(100));  /* 精确延时 100ms */
        }
    }
}
