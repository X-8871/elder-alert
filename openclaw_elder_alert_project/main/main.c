#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"

#include "BSP_INMP441.h"
#include "AppController.h"
#include "AlertController.h"
#include "DisplayController.h"
#include "EventLog.h"
#include "HttpAlertReporter.h"
#include "InputController.h"
#include "RainMakerReporter.h"
#include "RiskEngine.h"
#include "SensorHub.h"
#include "SpeechUploader.h"
#include "WiFiManager.h"

#define TAG "SENSOR_DEMO"

#define ENABLE_INMP441_LEVEL_TEST  0
#define ENABLE_INMP441_UPLOAD_TEST 1
#define INMP441_TEST_BCLK_GPIO     GPIO_NUM_12
#define INMP441_TEST_WS_GPIO       GPIO_NUM_13
#define INMP441_TEST_DIN_GPIO      GPIO_NUM_14
#define INMP441_UPLOAD_RECORD_MS   3000U

#if ENABLE_INMP441_LEVEL_TEST
static void run_inmp441_level_test(void)
{
    const bsp_inmp441_config_t mic_config = {
        .bclk_gpio = INMP441_TEST_BCLK_GPIO,
        .ws_gpio = INMP441_TEST_WS_GPIO,
        .data_in_gpio = INMP441_TEST_DIN_GPIO,
        .sample_rate_hz = BSP_INMP441_DEFAULT_SAMPLE_RATE_HZ,
    };

    esp_err_t ret = BSP_INMP441_Init(&mic_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "INMP441 init failed: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "INMP441 level test started, speak or clap near the microphone");
    while (1) {
        bsp_inmp441_level_t level = {0};
        ret = BSP_INMP441_ReadLevel(&level, 1000);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG,
                     "INMP441 level: mean_abs=%" PRId32 " peak_abs=%" PRId32 " samples=%u",
                     level.mean_abs,
                     level.peak_abs,
                     (unsigned)level.sample_count);
        } else {
            ESP_LOGW(TAG, "INMP441 read failed: %s", esp_err_to_name(ret));
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
#endif

#if ENABLE_INMP441_UPLOAD_TEST
static bool s_speech_upload_ready = false;

static void init_inmp441_upload_test(void)
{
    const speech_uploader_config_t speech_config = {
        .bclk_gpio = INMP441_TEST_BCLK_GPIO,
        .ws_gpio = INMP441_TEST_WS_GPIO,
        .data_in_gpio = INMP441_TEST_DIN_GPIO,
        .sample_rate_hz = BSP_INMP441_DEFAULT_SAMPLE_RATE_HZ,
    };

    esp_err_t ret = SpeechUploader_Init(&speech_config);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SpeechUploader init failed: %s", esp_err_to_name(ret));
        return;
    }

    s_speech_upload_ready = true;
    ESP_LOGI(TAG, "speech upload test ready, press GPIO17 record key to upload a short WAV");
}

static void service_inmp441_upload_test(void)
{
    if (!s_speech_upload_ready) {
        return;
    }

    bool record_requested = false;
    esp_err_t ret = InputController_GetRecordEvent(&record_requested);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "record key check failed: %s", esp_err_to_name(ret));
        return;
    }
    if (!record_requested) {
        return;
    }

    ESP_LOGI(TAG, "record key pressed, record and upload speech");
    ret = SpeechUploader_RecordWavAndUpload(INMP441_UPLOAD_RECORD_MS);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "speech upload test failed: %s", esp_err_to_name(ret));
    }
}
#endif

static esp_err_t prepare_connectivity_primitives(void)
{
    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    return ESP_OK;
}

/*
 * 系统总架构说明
 *
 * 这份工程按“main -> Middleware -> BSP”三层来组织。
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
 * 2. Middleware 中间层
 *    这一层负责“业务编排”，把多个底层硬件能力组合成可用的系统行为。
 *
 *    - SensorHub
 *      统一管理 AHT20 / BMP280 / BH1750 / MQ2 / AM312。
 *      对上层输出一份统一结构 `sensor_hub_data_t`，
 *      让 main 和 RiskEngine 不需要分别理解每个传感器的驱动细节。
 *
 *    - RiskEngine
 *      根据传感器数据和上下文信息做风险判定。
 *      它只负责“算结论”，不负责切换硬件，也不直接改系统状态。
 *
 *    - AppController
 *      这是系统状态机核心，统一管理 NORMAL / REMIND / ALARM / SOS。
 *      它综合风险结果、人体活动状态、用户按键事件，决定系统当前应该处于什么状态。
 *
 *    - AlertController
 *      把 AppController 给出的抽象状态翻译成底层声光提醒模式，
 *      例如 LED 是否闪烁、蜂鸣器是否鸣叫、节拍多快。
 *
 *    - InputController
 *      对 KEY 底层做再封装。
 *      上层不需要直接读 GPIO，只关心“确认键是否触发”“SOS 键是否触发”。
 *
 *    - DisplayController
 *      负责把当前状态、风险等级、传感器摘要渲染到 OLED。
 *
 *    - EventLog
 *      负责记录异常状态切换时刻的关键现场快照，
 *      用于后续追踪“什么时候发生了提醒/报警/SOS，以及当时的环境数据是什么”。
 *
 * 3. BSP 底层
 *    这一层直接操作具体硬件，是“设备驱动适配层”。
 *
 *    - BSP_I2C: 提供共享 I2C 总线配置
 *    - BSP_AHT20 / BSP_BMP280 / BSP_BH1750: I2C 传感器驱动封装
 *    - BSP_MQ2: ADC 烟雾/气体传感器采样
 *    - BSP_AM312: 人体红外输入采样
 *    - BSP_Alert: LED + 蜂鸣器输出控制
 *    - BSP_OLED: SSD1306 OLED 显示
 *    - KEY: 按键扫描与中断事件
 *
 * 4. 一次完整主循环的控制流
 *    `app_main()` 外层 while(1) 每次执行如下步骤：
 *    1) SensorHub_Read()        -> 采集一帧完整传感器数据
 *    2) RiskEngine_Evaluate()   -> 计算当前风险等级与原因
 *    3) AppController_Process() -> 根据风险结果更新系统状态机
 *    4) EventLog_Update()       -> 若进入新的异常状态，则记录事件
 *    5) DisplayController_Update()
 *                                -> 把状态和摘要更新到屏幕
 *    6) AppController_Service() -> 在接下来约 2 秒内，每 100ms 处理一次按键和提醒刷新
 *
 * 5. 状态优先级
 *    - SOS    最高：用户主动求助，优先级高于其他状态
 *    - ALARM  次高：环境风险告警
 *    - REMIND 较低：长时间无人活动提醒
 *    - NORMAL 默认：没有异常
 *
 * 6. 为什么分成“外层 2 秒采样 + 内层 100ms 服务”
 *    - 外层采样周期负责环境感知，不需要太快
 *    - 内层服务周期负责交互响应，必须更及时
 *    这样既能控制采样节奏，又能保证按键和提醒模式切换不迟钝。
 */
void app_main(void)
{
#if ENABLE_INMP441_LEVEL_TEST
    run_inmp441_level_test();
    return;
#endif

    /* 第 1 步：先把所有“数据输入链路”准备好，后续主循环才能拿到统一传感器快照。 */
    esp_err_t ret = SensorHub_Init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SensorHub init failed: %s", esp_err_to_name(ret));
        return;
    }

    /* 第 2 步：初始化声光输出控制链路。AppController 后续切状态时会依赖它。 */
    ret = AlertController_Init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "AlertController init failed: %s", esp_err_to_name(ret));
        return;
    }

    /* 第 3 步：初始化用户输入链路，让确认键和 SOS 键开始产生事件。 */
    ret = InputController_Init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "InputController init failed: %s", esp_err_to_name(ret));
        return;
    }

    /* 第 4 步：显示是可选能力，失败时不阻塞主流程，只是失去屏幕输出。 */
    ret = DisplayController_Init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "DisplayController init skipped: %s", esp_err_to_name(ret));
    }

    /*
     * 第 5 步：初始化 Wi-Fi 联网能力。
     * 联网失败不能阻塞本地提醒闭环，因此这里只记录告警并继续主流程。
     */
    ret = prepare_connectivity_primitives();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Connectivity primitives init failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = RainMakerReporter_Init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "RainMakerReporter init failed: %s", esp_err_to_name(ret));
    }

    ret = WiFiManager_Init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WiFiManager init failed: %s", esp_err_to_name(ret));
    }

    /* 第 6 步：初始化应用状态机，系统从 NORMAL 起步。 */
    ret = AppController_Init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "AppController init failed: %s", esp_err_to_name(ret));
        return;
    }

    /* 第 7 步：初始化事件日志，为异常状态的现场快照记录做准备。 */
    ret = EventLog_Init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "EventLog init failed: %s", esp_err_to_name(ret));
        return;
    }

    /* 第 8 步：初始化 HTTP 上报能力。失败不阻塞本地闭环。 */
    ret = HttpAlertReporter_Init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "HttpAlertReporter init failed: %s", esp_err_to_name(ret));
    }

#if ENABLE_INMP441_UPLOAD_TEST
    init_inmp441_upload_test();
#endif

    /* 第 9 步：初始化 RainMaker 上云能力。失败不阻塞本地闭环。 */
    while (1) {
        /* 每一轮 while 都表示“采一帧环境快照，并据此推进一次系统业务状态”。 */
        sensor_hub_data_t sensor_data = {0};
        risk_result_t risk_result = {0};
        risk_context_t risk_context = {0};

        /* 统一从 SensorHub 拉取一份完整传感器数据。 */
        ret = SensorHub_Read(&sensor_data);
        if (ret == ESP_OK) {
            /*
             * 风险引擎除了原始传感器值外，还需要一些上下文：
             * - now_ms: 当前运行时刻，用于持续确认类风险计时
             * - inactive_ms: 最近多久没有检测到人体活动
             * - manual_sos_active: 用户是否已经手动触发 SOS
             * - remind_timeout_active: 本地提醒是否已因无人确认升级
             */
            risk_context.now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
            risk_context.inactive_ms = (!sensor_data.am312_ok || sensor_data.motion_detected)
                                           ? 0
                                           : AppController_GetInactiveTimeMs();
            risk_context.manual_sos_active = AppController_IsSosLatched();
            risk_context.remind_timeout_active = sensor_data.am312_ok &&
                                                  !sensor_data.motion_detected &&
                                                  AppController_IsRemindTimeoutLatched();

            /* 配网联调期间先关闭周期性传感器串口输出，保留 BLE/Wi-Fi/RainMaker 日志。 */
            // SensorHub_LogData(&sensor_data);

            /* 把“数据”转成“风险结论”。 */
            RiskEngine_Evaluate(&sensor_data, &risk_context, &risk_result);
            // RiskEngine_LogResult(&risk_result);

            /* 状态机根据风险结果决定系统应处于 NORMAL / REMIND / ALARM / SOS 中哪一态。 */
            ret = AppController_Process(&sensor_data, &risk_result);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "AppController process failed: %s", esp_err_to_name(ret));
            }

            /* 只有异常状态变化时才记录事件，避免重复刷同一条日志。 */
            ret = EventLog_Update(AppController_GetState(), &sensor_data, &risk_result);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "EventLog update failed: %s", esp_err_to_name(ret));
            }

            ret = HttpAlertReporter_Process(AppController_GetState(), &sensor_data, &risk_result);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "HttpAlertReporter process failed: %s", esp_err_to_name(ret));
            }

            ret = RainMakerReporter_Process(AppController_GetState(), &sensor_data, &risk_result);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "RainMakerReporter process failed: %s", esp_err_to_name(ret));
            }

            /* 显示模块始终根据“当前最终状态”来刷新，不直接依赖中间计算过程。 */
            ret = DisplayController_Update(AppController_GetState(), &sensor_data, &risk_result);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "DisplayController update failed: %s", esp_err_to_name(ret));
            }
        } else {
            ESP_LOGE(TAG, "SensorHub read failed: %s", esp_err_to_name(ret));
        }

        /*
         * 内层小循环负责高频服务任务：
         * - 处理按键事件
         * - 刷新 LED / 蜂鸣器闪烁节拍
         * 20 * 100ms ≈ 2s，刚好和下一轮完整采样周期对齐。
         */
        for (int i = 0; i < 20; ++i) {
            bool reset_wifi_requested = false;
            ret = InputController_GetConfirmLongPressEvent(&reset_wifi_requested);
            if (ret == ESP_OK && reset_wifi_requested) {
                ESP_LOGW(TAG, "confirm key held for 8s, reset Wi-Fi provisioning");
                ret = WiFiManager_ResetProvisioningAndRestart();
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "Wi-Fi provisioning reset failed: %s", esp_err_to_name(ret));
                }
            } else if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Confirm long press check failed: %s", esp_err_to_name(ret));
            }

            ret = AppController_Service();
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "AppController service failed: %s", esp_err_to_name(ret));
            }

#if ENABLE_INMP441_UPLOAD_TEST
            service_inmp441_upload_test();
#endif
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}
