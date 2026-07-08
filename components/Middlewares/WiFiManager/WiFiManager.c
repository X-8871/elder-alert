/*
 * WiFiManager.c —— Wi-Fi 联网管理模块实现
 *
 * BLE 配网 + Wi-Fi STA 模式，7 态状态机，事件驱动。
 */
#include "WiFiManager.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_ble.h"

/* ---- BLE 配网参数宏 ---- */

#define WIFI_MANAGER_PROV_QR_VERSION "v1"                          /* 二维码协议版本 */
#define WIFI_MANAGER_PROV_TRANSPORT  "ble"                         /* 配网传输方式：BLE */
#define WIFI_MANAGER_PROV_QR_BASE_URL "https://espressif.github.io/esp-jumpstart/qrcode.html"
#define WIFI_MANAGER_PROV_POP        "eldercare1234"               /* PoP（Proof of Possession）：配网口令，手机端必须知道此口令才能建立安全会话 */
#define WIFI_MANAGER_PROV_SERVICE_PREFIX "PROV_"                   /* 服务名前缀，完整名 = PROV_ + MAC 后三字节，如 PROV_A1B2C3 */
#define WIFI_MANAGER_SNTP_SYNC_WAIT_MS 30000U

static const char *TAG = "WiFiManager";

/* ---- 模块全局状态 ---- */

static bool s_initialized = false;                                 /* 标记 Init 是否已执行过，保证幂等 */
static bool s_provisioning_active = false;                         /* 标记 BLE 配网是否正在进行中，影响断线时的状态切换决策 */
static bool s_sntp_initialized = false;                            /* 标记 SNTP 是否已显式初始化，避免重复 init */
static bool s_sntp_wait_task_running = false;                      /* 防止重复创建等待任务 */
static wifi_manager_state_t s_state = WIFI_MANAGER_STATE_IDLE;     /* 状态机当前状态，唯一由 set_state() 修改 */
static char s_ip_string[16] = "0.0.0.0";                          /* 当前 IP 地址字符串，收到 IP_EVENT_STA_GOT_IP 时更新 */
static esp_netif_t *s_sta_netif = NULL;                            /* Wi-Fi STA 网络接口对象 */

/*
 * 状态切换函数 —— 整个文件中唯一能修改 s_state 的地方。
 * 相同状态不做任何事（去重），切换时自动打印新状态方便调试。
 */
static void set_state(wifi_manager_state_t state)
{
    if (s_state == state) {
        return;
    }

    s_state = state;
    ESP_LOGI(TAG, "wifi_state=%s", WiFiManager_GetStatusString());
}

/* NVS 初始化，持久保存 Wi-Fi 凭据。分区损坏时擦除重建。 */
static esp_err_t init_nvs_once(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    if (ret == ESP_ERR_INVALID_STATE) {
        return ESP_OK;
    }

    return ret;
}

/* 网络接口框架初始化。 */
static esp_err_t init_netif_once(void)
{
    esp_err_t ret = esp_netif_init();
    if (ret == ESP_ERR_INVALID_STATE) {
        return ESP_OK;
    }

    return ret;
}

/* 默认事件循环初始化。 */
static esp_err_t init_event_loop_once(void)
{
    esp_err_t ret = esp_event_loop_create_default();
    if (ret == ESP_ERR_INVALID_STATE) {
        return ESP_OK;
    }

    return ret;
}

/* 生成 BLE 配网服务名：PROV_ + MAC 后三字节。 */
static void get_device_service_name(char *service_name, size_t max_len)
{
    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(service_name,
             max_len,
             "%s%02X%02X%02X",
             WIFI_MANAGER_PROV_SERVICE_PREFIX,
             mac[3],
             mac[4],
             mac[5]);
}

/* 打印 BLE 配网二维码信息，供 ESP RainMaker App 扫码。 */
static void log_provisioning_qr(const char *service_name)
{
    char payload[160] = {0};
    snprintf(payload,
             sizeof(payload),
             "{\"ver\":\"%s\",\"name\":\"%s\",\"pop\":\"%s\",\"transport\":\"%s\",\"network\":\"wifi\"}",
             WIFI_MANAGER_PROV_QR_VERSION,
             service_name,
             WIFI_MANAGER_PROV_POP,
             WIFI_MANAGER_PROV_TRANSPORT);

    ESP_LOGI(TAG, "BLE provisioning service_name=%s pop=%s", service_name, WIFI_MANAGER_PROV_POP);
    ESP_LOGI(TAG, "Open this URL or scan its QR payload in ESP RainMaker app:");
    ESP_LOGI(TAG, "%s?data=%s", WIFI_MANAGER_PROV_QR_BASE_URL, payload);
}

static void sntp_time_sync_notification_cb(struct timeval *tv)
{
    (void)tv;

    time_t now = time(NULL);
    struct tm local_tm = {0};
    if (localtime_r(&now, &local_tm) == NULL) {
        ESP_LOGI(TAG, "SNTP time sync completed");
        return;
    }

    char time_buffer[32] = {0};
    strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S", &local_tm);
    ESP_LOGI(TAG, "SNTP time sync completed, local_time=%s", time_buffer);
}

static void sntp_sync_wait_task(void *arg)
{
    (void)arg;

    esp_err_t ret = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(WIFI_MANAGER_SNTP_SYNC_WAIT_MS));
    if (ret == ESP_OK) {
        time_t now = time(NULL);
        struct tm local_tm = {0};
        if (localtime_r(&now, &local_tm) != NULL) {
            char time_buffer[32] = {0};
            strftime(time_buffer, sizeof(time_buffer), "%Y-%m-%d %H:%M:%S", &local_tm);
            ESP_LOGI(TAG, "SNTP sync wait completed, local_time=%s", time_buffer);
        } else {
            ESP_LOGI(TAG, "SNTP sync wait completed");
        }
    } else {
        ESP_LOGW(TAG, "SNTP sync wait failed: %s", esp_err_to_name(ret));
    }

    s_sntp_wait_task_running = false;
    vTaskDelete(NULL);
}

static void create_sntp_sync_wait_task_if_needed(void)
{
    if (s_sntp_wait_task_running) {
        return;
    }

    BaseType_t task_ok = xTaskCreate(sntp_sync_wait_task,
                                     "sntp_wait",
                                     3072,
                                     NULL,
                                     tskIDLE_PRIORITY + 1,
                                     NULL);
    if (task_ok == pdPASS) {
        s_sntp_wait_task_running = true;
    } else {
        ESP_LOGW(TAG, "failed to create SNTP wait task");
    }
}

static void start_sntp(void)
{
    setenv("TZ", "CST-8", 1);
    tzset();

    if (!s_sntp_initialized) {
        esp_sntp_config_t sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
            3,
            ESP_SNTP_SERVER_LIST("ntp.aliyun.com", "ntp1.aliyun.com", "pool.ntp.org"));
        sntp_config.start = false;
        sntp_config.sync_cb = sntp_time_sync_notification_cb;

        esp_err_t ret = esp_netif_sntp_init(&sntp_config);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "SNTP init failed: %s", esp_err_to_name(ret));
            return;
        }

        s_sntp_initialized = true;
        ESP_LOGI(TAG, "SNTP initialized");
    }

    esp_err_t ret = esp_netif_sntp_start();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SNTP start failed: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "SNTP started with servers=ntp.aliyun.com, ntp1.aliyun.com, pool.ntp.org");
    create_sntp_sync_wait_task_if_needed();
}

/* 统一事件回调，接收 5 类事件并驱动状态切换。 */
static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;

    /* ===== 第一类事件：BLE 配网流程（NETWORK_PROV_EVENT） ===== */
    if (event_base == NETWORK_PROV_EVENT) {
        switch (event_id) {
        case NETWORK_PROV_START:
            /* BLE 配网开始广播，手机此时可以扫描到设备 */
            s_provisioning_active = true;
            set_state(WIFI_MANAGER_STATE_PROVISIONING);
            ESP_LOGI(TAG, "BLE provisioning started");
            break;

        case NETWORK_PROV_WIFI_CRED_RECV: {
            /* 收到手机发来的 Wi-Fi 凭据（SSID + 密码），仅记录日志，不改变状态 */
            wifi_sta_config_t *wifi_sta_cfg = (wifi_sta_config_t *)event_data;
            if (wifi_sta_cfg != NULL) {
                ESP_LOGI(TAG,
                         "received Wi-Fi credentials ssid=%s password=%s",
                         (const char *)wifi_sta_cfg->ssid,
                         (const char *)wifi_sta_cfg->password);
            }
            break;
        }

        case NETWORK_PROV_WIFI_CRED_FAIL: {
            /* 凭据无效：可能是密码错误（auth_error）或找不到 AP（ap_not_found），退回配网状态 */
            network_prov_wifi_sta_fail_reason_t *reason =
                (network_prov_wifi_sta_fail_reason_t *)event_data;
            ESP_LOGW(TAG,
                     "provisioning failed, reason=%s",
                     (reason != NULL && *reason == NETWORK_PROV_WIFI_STA_AUTH_ERROR)
                         ? "auth_error"
                         : "ap_not_found");
            set_state(WIFI_MANAGER_STATE_PROVISIONING);
            break;
        }

        case NETWORK_PROV_WIFI_CRED_SUCCESS:
            /* 凭据验证通过，接下来 Wi-Fi 驱动会用这些凭据去连路由器 */
            ESP_LOGI(TAG, "provisioning successful");
            if (s_state != WIFI_MANAGER_STATE_CONNECTED) {
                set_state(WIFI_MANAGER_STATE_CONNECTING);
            }
            break;

        case NETWORK_PROV_END:
            /* 配网流程结束，释放 BLE 配网相关资源 */
            s_provisioning_active = false;
            ESP_LOGI(TAG, "provisioning finished");
            network_prov_mgr_deinit();
            break;

        default:
            break;
        }
        return;
    }

    /* ===== 第二类事件：Wi-Fi 驱动层（WIFI_EVENT） ===== */
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            /* STA 已启动，配网期间不改状态。 */
            if (!s_provisioning_active) {
                set_state(WIFI_MANAGER_STATE_CONNECTING);
            }
            ESP_LOGI(TAG, "wifi station started");
            esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            /* 与路由器断开连接，清空 IP 地址，尝试自动重连 */
            wifi_event_sta_disconnected_t *disconnected = (wifi_event_sta_disconnected_t *)event_data;
            s_ip_string[0] = '\0';
            if (s_initialized) {
                /*
                 * 断线时的状态决策：
                 * - 正在配网 → 退回 PROVISIONING（配网还没完成，不应切换到重连）
                 * - 已经初始化过 → RECONNECTING（凭据已有，自动重连路由器）
                 */
                set_state(s_provisioning_active ? WIFI_MANAGER_STATE_PROVISIONING
                                                : WIFI_MANAGER_STATE_RECONNECTING);
                ESP_LOGW(TAG,
                         "wifi disconnected, reason=%d, retrying",
                         disconnected != NULL ? disconnected->reason : -1);
                esp_wifi_connect();
            } else {
                set_state(WIFI_MANAGER_STATE_DISCONNECTED);
            }
            break;
        }

        default:
            break;
        }
        return;
    }

    /* ===== 第三类事件：IP 获取（IP_EVENT） ===== */
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *got_ip = (ip_event_got_ip_t *)event_data;
        if (got_ip == NULL) {
            set_state(WIFI_MANAGER_STATE_FAILED);
            return;
        }

        /* 保存 IP 地址字符串并切到 CONNECTED 状态 */
        snprintf(s_ip_string,
                 sizeof(s_ip_string),
                 IPSTR,
                 IP2STR(&got_ip->ip_info.ip));
        set_state(WIFI_MANAGER_STATE_CONNECTED);
        ESP_LOGI(TAG, "wifi connected, ip=%s", s_ip_string);
        start_sntp();
        return;
    }

    /* ===== 第四类事件：BLE 传输层连接/断开（PROTOCOMM_TRANSPORT_BLE_EVENT） ===== */
    if (event_base == PROTOCOMM_TRANSPORT_BLE_EVENT) {
        switch (event_id) {
        case PROTOCOMM_TRANSPORT_BLE_CONNECTED:
            ESP_LOGI(TAG, "BLE transport connected");
            break;
        case PROTOCOMM_TRANSPORT_BLE_DISCONNECTED:
            ESP_LOGI(TAG, "BLE transport disconnected");
            break;
        default:
            break;
        }
        return;
    }

    /* ===== 第五类事件：安全会话（PROTOCOMM_SECURITY_SESSION_EVENT） ===== */
    if (event_base == PROTOCOMM_SECURITY_SESSION_EVENT) {
        switch (event_id) {
        case PROTOCOMM_SECURITY_SESSION_SETUP_OK:
            /* 安全会话建立成功，手机端 PoP 验证通过 */
            ESP_LOGI(TAG, "BLE provisioning secure session established");
            break;
        case PROTOCOMM_SECURITY_SESSION_INVALID_SECURITY_PARAMS:
            ESP_LOGW(TAG, "BLE provisioning invalid security params");
            break;
        case PROTOCOMM_SECURITY_SESSION_CREDENTIALS_MISMATCH:
            /* PoP 不匹配：手机端输入的口令与设备端不一致 */
            ESP_LOGW(TAG, "BLE provisioning PoP mismatch");
            break;
        default:
            break;
        }
    }
}

/* 启动 Wi-Fi STA 模式，用于已配过网的设备。 */
static esp_err_t start_wifi_sta(void)
{
    esp_err_t ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        return ret;
    }

    set_state(WIFI_MANAGER_STATE_CONNECTING);
    return ESP_OK;
}

/*
 * 启动 BLE 配网，用于首次上电或配网信息被清除的设备。
 * 使用 NETWORK_PROV_SECURITY_1 + PoP 安全机制。
 */
static esp_err_t start_ble_provisioning(void)
{
    char service_name[16] = {0};
    get_device_service_name(service_name, sizeof(service_name));

    /* 自定义 BLE 服务 UUID */
    uint8_t custom_service_uuid[] = {
        0xb4, 0xdf, 0x5a, 0x1c, 0x3f, 0x6b, 0xf4, 0xbf,
        0xea, 0x4a, 0x82, 0x03, 0x04, 0x90, 0x1a, 0x02,
    };
    network_prov_scheme_ble_set_service_uuid(custom_service_uuid);

    s_provisioning_active = true;
    set_state(WIFI_MANAGER_STATE_PROVISIONING);

    /* 启动 BLE 广播，等待手机连接并发送凭据 */
    esp_err_t ret = network_prov_mgr_start_provisioning(NETWORK_PROV_SECURITY_1,
                                                        WIFI_MANAGER_PROV_POP,
                                                        service_name,
                                                        NULL);
    if (ret != ESP_OK) {
        network_prov_mgr_deinit();
        s_provisioning_active = false;
        set_state(WIFI_MANAGER_STATE_FAILED);
        return ret;
    }

    log_provisioning_qr(service_name);
    return ESP_OK;
}

/* WiFiManager 初始化：NVS → netif → 事件循环 → STA 接口 → Wi-Fi 驱动 → 事件注册 → 配网判断。 */
esp_err_t WiFiManager_Init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    /* ---- ① NVS 初始化 ---- */
    esp_err_t ret = init_nvs_once();
    if (ret != ESP_OK) {
        set_state(WIFI_MANAGER_STATE_FAILED);
        return ret;
    }

    /* ---- ② 网络接口框架初始化 ---- */
    ret = init_netif_once();
    if (ret != ESP_OK) {
        set_state(WIFI_MANAGER_STATE_FAILED);
        return ret;
    }

    /* ---- ③ 默认事件循环初始化 ---- */
    ret = init_event_loop_once();
    if (ret != ESP_OK) {
        set_state(WIFI_MANAGER_STATE_FAILED);
        return ret;
    }

    /* ---- ④ 创建 Wi-Fi STA 网络接口 ---- */
    if (s_sta_netif == NULL) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
        if (s_sta_netif == NULL) {
            set_state(WIFI_MANAGER_STATE_FAILED);
            return ESP_FAIL;
        }
    }

    /* ---- ⑤ 初始化 Wi-Fi 驱动 ---- */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret == ESP_ERR_INVALID_STATE) {
        ret = ESP_OK;
    }
    if (ret != ESP_OK) {
        set_state(WIFI_MANAGER_STATE_FAILED);
        return ret;
    }

    /* ---- ⑥ 注册 5 类事件回调 ---- */

    /* 配网流程事件 */
    ret = esp_event_handler_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    if (ret != ESP_OK) {
        set_state(WIFI_MANAGER_STATE_FAILED);
        return ret;
    }

    /* BLE 传输层事件 */
    ret = esp_event_handler_register(PROTOCOMM_TRANSPORT_BLE_EVENT,
                                     ESP_EVENT_ANY_ID,
                                     &wifi_event_handler,
                                     NULL);
    if (ret != ESP_OK) {
        set_state(WIFI_MANAGER_STATE_FAILED);
        return ret;
    }

    /* 安全会话事件 */
    ret = esp_event_handler_register(PROTOCOMM_SECURITY_SESSION_EVENT,
                                     ESP_EVENT_ANY_ID,
                                     &wifi_event_handler,
                                     NULL);
    if (ret != ESP_OK) {
        set_state(WIFI_MANAGER_STATE_FAILED);
        return ret;
    }

    /* Wi-Fi 驱动事件 */
    ret = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    if (ret != ESP_OK) {
        set_state(WIFI_MANAGER_STATE_FAILED);
        return ret;
    }

    /* IP 事件 */
    ret = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);
    if (ret != ESP_OK) {
        set_state(WIFI_MANAGER_STATE_FAILED);
        return ret;
    }

    /* ---- ⑦ 判断是否已配过网 ---- */
    bool provisioned = false;
    network_prov_mgr_config_t probe_config = {
        .scheme = network_prov_scheme_ble,
        .scheme_event_handler = NETWORK_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM,
    };
    ret = network_prov_mgr_init(probe_config);
    if (ret != ESP_OK) {
        set_state(WIFI_MANAGER_STATE_FAILED);
        return ret;
    }

    ret = network_prov_mgr_is_wifi_provisioned(&provisioned);
    if (ret != ESP_OK) {
        network_prov_mgr_deinit();
        set_state(WIFI_MANAGER_STATE_FAILED);
        return ret;
    }

    /* ---- ⑧ 分支决策 ---- */
    s_ip_string[0] = '\0';
    if (provisioned) {
        /* 已配网，直接 STA 连接 */
        ESP_LOGI(TAG, "Wi-Fi already provisioned, starting station");
        network_prov_mgr_deinit();
        ret = start_wifi_sta();
    } else {
        /* 未配网，走 BLE 配网 */
        ESP_LOGI(TAG, "Wi-Fi not provisioned, starting BLE provisioning");
        ret = start_ble_provisioning();
    }

    if (ret != ESP_OK) {
        set_state(WIFI_MANAGER_STATE_FAILED);
        return ret;
    }

    s_initialized = true;
    return ESP_OK;
}

/* 清除配网信息并重启。 */
esp_err_t WiFiManager_ResetProvisioningAndRestart(void)
{
    ESP_LOGW(TAG, "reset Wi-Fi provisioning requested, clearing saved network and restarting");
    esp_err_t ret = network_prov_mgr_reset_wifi_provisioning();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "reset Wi-Fi provisioning failed: %s", esp_err_to_name(ret));
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return ESP_OK;
}

/* 获取当前联网状态 */
wifi_manager_state_t WiFiManager_GetState(void)
{
    return s_state;
}

/* 判断设备是否已联网（状态 == CONNECTED）。 */
bool WiFiManager_IsConnected(void)
{
    return s_state == WIFI_MANAGER_STATE_CONNECTED;
}

/* 状态全称字符串 */
const char *WiFiManager_GetStatusString(void)
{
    switch (s_state) {
    case WIFI_MANAGER_STATE_IDLE:
        return "IDLE";
    case WIFI_MANAGER_STATE_PROVISIONING:
        return "PROVISIONING";
    case WIFI_MANAGER_STATE_CONNECTING:
        return "CONNECTING";
    case WIFI_MANAGER_STATE_CONNECTED:
        return "CONNECTED";
    case WIFI_MANAGER_STATE_RECONNECTING:
        return "RECONNECTING";
    case WIFI_MANAGER_STATE_DISCONNECTED:
        return "DISCONNECTED";
    case WIFI_MANAGER_STATE_FAILED:
        return "FAILED";
    default:
        return "UNKNOWN";
    }
}

/* 状态简称 */
const char *WiFiManager_GetStatusShortString(void)
{
    switch (s_state) {
    case WIFI_MANAGER_STATE_IDLE:
        return "IDLE";
    case WIFI_MANAGER_STATE_PROVISIONING:
        return "PROV";
    case WIFI_MANAGER_STATE_CONNECTING:
        return "CONN";
    case WIFI_MANAGER_STATE_CONNECTED:
        return "OK";
    case WIFI_MANAGER_STATE_RECONNECTING:
        return "RETRY";
    case WIFI_MANAGER_STATE_DISCONNECTED:
        return "DISC";
    case WIFI_MANAGER_STATE_FAILED:
        return "FAIL";
    default:
        return "UNK";
    }
}

/* 获取当前 IP 地址字符串，未联网时返回 "0.0.0.0"。 */
const char *WiFiManager_GetIpString(void)
{
    return s_ip_string[0] != '\0' ? s_ip_string : "0.0.0.0";
}
