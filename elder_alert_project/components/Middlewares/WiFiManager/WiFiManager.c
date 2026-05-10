/*
 * WiFiManager.c —— Wi-Fi 联网管理模块实现
 *
 * 本模块通过 BLE 配网 + Wi-Fi STA 模式实现设备联网，核心是一个 7 态状态机。
 * 所有状态切换由统一的 wifi_event_handler() 驱动，遵循"先启动动作，结果靠事件通知"的异步模型。
 *
 * 完整联网链路：
 *   NVS 初始化 → netif 初始化 → 事件循环初始化 → 创建 STA netif
 *   → 初始化 Wi-Fi 驱动 → 注册 5 类事件回调 → 判断是否已配网
 *   → 已配网：start_wifi_sta() 直接连
 *   → 未配网：start_ble_provisioning() 走 BLE 配网
 */
#include "WiFiManager.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
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

static const char *TAG = "WiFiManager";

/* ---- 模块全局状态 ---- */

static bool s_initialized = false;                                 /* 标记 Init 是否已执行过，保证幂等 */
static bool s_provisioning_active = false;                         /* 标记 BLE 配网是否正在进行中，影响断线时的状态切换决策 */
static wifi_manager_state_t s_state = WIFI_MANAGER_STATE_IDLE;     /* 状态机当前状态，唯一由 set_state() 修改 */
static char s_ip_string[16] = "0.0.0.0";                          /* 当前 IP 地址字符串，收到 IP_EVENT_STA_GOT_IP 时更新 */
static esp_netif_t *s_sta_netif = NULL;                            /* Wi-Fi STA 网络接口对象，相当于软件层面的"无线网卡" */

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

/*
 * NVS 初始化（非易失性存储）—— 联网链路的第一步。
 * NVS 用于持久保存 Wi-Fi 凭据（SSID/密码），使设备断电重启后能自动重连，无需重新配网。
 * 如果 NVS 分区损坏（NO_FREE_PAGES）或版本不匹配，先擦除再重新初始化。
 * ESP_ERR_INVALID_STATE 表示已被其他模块初始化过，视为成功。
 */
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

/*
 * 网络接口框架初始化 —— 联网链路的第二步。
 * esp_netif_init() 为后续创建 Wi-Fi STA 接口、获取 IP、绑定协议栈提供基础框架。
 * 相当于初始化"网络接口管理层"。
 */
static esp_err_t init_netif_once(void)
{
    esp_err_t ret = esp_netif_init();
    if (ret == ESP_ERR_INVALID_STATE) {
        return ESP_OK;
    }

    return ret;
}

/*
 * 默认事件循环初始化 —— 联网链路的第三步。
 * 创建系统级事件循环，让后续的 esp_event_handler_register() 能正常工作。
 * WiFiManager 的所有状态切换都依赖事件驱动，没有事件循环就收不到任何异步通知。
 */
static esp_err_t init_event_loop_once(void)
{
    esp_err_t ret = esp_event_loop_create_default();
    if (ret == ESP_ERR_INVALID_STATE) {
        return ESP_OK;
    }

    return ret;
}

/*
 * 生成 BLE 配网服务名，格式为 "PROV_" + MAC 后三字节（大写十六进制），如 PROV_A1B2C3。
 * 手机扫描 BLE 时看到的设备名就是这个，方便用户从列表中识别出自己的设备。
 */
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

/*
 * 打印 BLE 配网的二维码信息（JSON 格式），供 ESP RainMaker App 扫码配网。
 * 输出内容包含：协议版本、服务名、PoP 口令、传输方式。
 * 注意：生产环境中不应明文打印 PoP，此处仅为开发调试方便。
 */
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

/*
 * 统一事件回调 —— WiFiManager 状态机的"发动机"。
 *
 * 整个模块只有这一个事件入口，接收 5 类事件并驱动状态切换：
 *   1. NETWORK_PROV_EVENT             — BLE 配网流程事件
 *   2. WIFI_EVENT                     — Wi-Fi 驱动层事件（启动、断开）
 *   3. IP_EVENT                       — IP 协议栈事件（拿到 IP = 真正联网成功）
 *   4. PROTOCOMM_TRANSPORT_BLE_EVENT  — BLE 传输层连接/断开
 *   5. PROTOCOMM_SECURITY_SESSION_EVENT — 安全会话建立/失败（PoP 校验结果）
 *
 * 设计原则：事件是输入，状态是输出。收到什么事件决定状态怎么变。
 */
static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;

    /* ===== 第一类事件：BLE 配网流程（NETWORK_PROV_EVENT） =====
     * 这组事件反映"用户是否成功把 Wi-Fi 凭据发给设备"的过程。
     * 配网只负责前半段：把凭据送进设备并保存到 NVS。
     */
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

    /* ===== 第二类事件：Wi-Fi 驱动层（WIFI_EVENT） =====
     * 这组事件反映"设备和路由器之间的连接状态"。
     */
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            /*
             * Wi-Fi STA 模式已启动 —— 注意：这不等于连上网！
             * 相当于"无线功能打开了，现在开始去拨号连接"。
             * 配网期间（s_provisioning_active）不改状态，避免干扰配网流程。
             */
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

    /* ===== 第三类事件：IP 获取（IP_EVENT） =====
     * IP_EVENT_STA_GOT_IP 是整个联网链路中"设备真正具备网络通信能力"的唯一标志。
     * Wi-Fi 连上 ≠ 联网成功，拿到 IP 才算。
     */
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
        return;
    }

    /* ===== 第四类事件：BLE 传输层连接/断开（PROTOCOMM_TRANSPORT_BLE_EVENT） =====
     * 手机和设备之间的 BLE 物理链路状态，仅记录日志用于排查。
     */
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

    /* ===== 第五类事件：安全会话（PROTOCOMM_SECURITY_SESSION_EVENT） =====
     * 反映 PoP（Proof of Possession）身份校验的结果。
     * PoP 是配网时的"门禁"，防止附近陌生人给设备乱配网。
     */
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

/*
 * 启动 Wi-Fi STA 模式 —— 用于"已配过网"的设备直接连路由器。
 * 流程：设置 STA 模式 → 启动 Wi-Fi 子系统（esp_wifi_start）→ 状态切到 CONNECTING。
 * 注意：esp_wifi_start() 只是"打开无线功能"，真正的连接动作由 WIFI_EVENT_STA_START 回调中的 esp_wifi_connect() 发起。
 */
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
 * 启动 BLE 配网 —— 用于"首次上电"或"配网信息被清除"的设备。
 *
 * 流程：
 *   1. 生成服务名（PROV_ + MAC 后三字节）
 *   2. 设置自定义 UUID（告诉 BLE 层用哪个 UUID 广播配网服务）
 *   3. 标记配网状态为 PROVISIONING
 *   4. 调用 network_prov_mgr_start_provisioning() 开始 BLE 广播，等待手机连接
 *   5. 打印二维码信息供 ESP RainMaker App 扫码
 *
 * 安全机制：使用 NETWORK_PROV_SECURITY_1 + PoP，手机端必须知道正确的 PoP 才能建立安全会话。
 * 失败时会回退清理（deinit）并把状态设为 FAILED。
 */
static esp_err_t start_ble_provisioning(void)
{
    char service_name[16] = {0};
    get_device_service_name(service_name, sizeof(service_name));

    /* 自定义 BLE 服务 UUID，手机通过此 UUID 识别这是配网服务而非其他蓝牙设备 */
    uint8_t custom_service_uuid[] = {
        0xb4, 0xdf, 0x5a, 0x1c, 0x3f, 0x6b, 0xf4, 0xbf,
        0xea, 0x4a, 0x82, 0x03, 0x04, 0x90, 0x1a, 0x02,
    };
    network_prov_scheme_ble_set_service_uuid(custom_service_uuid);

    s_provisioning_active = true;
    set_state(WIFI_MANAGER_STATE_PROVISIONING);

    /* 真正启动配网：系统开始 BLE 广播，手机可以扫描、连接、发送凭据 */
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

/*
 * WiFiManager 初始化 —— 完成从 NVS 到事件注册的 6 层初始化，然后根据"是否已配过网"做分支决策。
 *
 * 按代码顺序执行 6 层初始化 + 1 次判断 + 1 次分支：
 *   ① NVS 初始化        — 为后续读/写配网凭据做准备
 *   ② netif 初始化      — 为创建网络接口打底
 *   ③ 事件循环初始化     — 让系统能收发异步事件
 *   ④ 创建 STA netif    — 建立设备的"无线网卡对象"
 *   ⑤ 初始化 Wi-Fi 驱动  — 把 Wi-Fi 硬件/驱动子系统准备好
 *   ⑥ 注册 5 类事件回调  — 把 wifi_event_handler 绑定到 5 类事件源
 *   ⑦ 判断是否已配网     — network_prov_mgr_is_wifi_provisioned()
 *   ⑧ 分支决策          — 已配网 → start_wifi_sta() 直接连；未配网 → start_ble_provisioning() 走 BLE 配网
 *
 * 幂等：重复调用直接返回 ESP_OK。
 */
esp_err_t WiFiManager_Init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    /* ---- ① NVS 初始化：非易失性存储，保存 Wi-Fi 凭据使断电后能自动重连 ---- */
    esp_err_t ret = init_nvs_once();
    if (ret != ESP_OK) {
        set_state(WIFI_MANAGER_STATE_FAILED);
        return ret;
    }

    /* ---- ② 网络接口框架初始化：为后续创建 Wi-Fi STA 接口、获取 IP 打底 ---- */
    ret = init_netif_once();
    if (ret != ESP_OK) {
        set_state(WIFI_MANAGER_STATE_FAILED);
        return ret;
    }

    /* ---- ③ 默认事件循环初始化：让 wifi_event_handler 能收到异步事件通知 ---- */
    ret = init_event_loop_once();
    if (ret != ESP_OK) {
        set_state(WIFI_MANAGER_STATE_FAILED);
        return ret;
    }

    /* ---- ④ 创建 Wi-Fi STA 网络接口：相当于创建设备的"客户端无线网卡" ---- */
    if (s_sta_netif == NULL) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
        if (s_sta_netif == NULL) {
            set_state(WIFI_MANAGER_STATE_FAILED);
            return ESP_FAIL;
        }
    }

    /* ---- ⑤ 初始化 Wi-Fi 驱动：cfg 由宏生成默认配置（TX/RX buffer 大小、NVS 开关等） ---- */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret == ESP_ERR_INVALID_STATE) {
        ret = ESP_OK;
    }
    if (ret != ESP_OK) {
        set_state(WIFI_MANAGER_STATE_FAILED);
        return ret;
    }

    /* ---- ⑥ 注册 5 类事件回调：所有事件统一由 wifi_event_handler 处理 ---- */

    /* 配网流程事件：BLE 配网开始、收到凭据、凭据成功/失败、配网结束 */
    ret = esp_event_handler_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    if (ret != ESP_OK) {
        set_state(WIFI_MANAGER_STATE_FAILED);
        return ret;
    }

    /* BLE 传输层事件：手机和设备之间的 BLE 物理链路连接/断开 */
    ret = esp_event_handler_register(PROTOCOMM_TRANSPORT_BLE_EVENT,
                                     ESP_EVENT_ANY_ID,
                                     &wifi_event_handler,
                                     NULL);
    if (ret != ESP_OK) {
        set_state(WIFI_MANAGER_STATE_FAILED);
        return ret;
    }

    /* 安全会话事件：PoP 身份校验成功/失败 */
    ret = esp_event_handler_register(PROTOCOMM_SECURITY_SESSION_EVENT,
                                     ESP_EVENT_ANY_ID,
                                     &wifi_event_handler,
                                     NULL);
    if (ret != ESP_OK) {
        set_state(WIFI_MANAGER_STATE_FAILED);
        return ret;
    }

    /* Wi-Fi 驱动事件：STA 启动、断开连接 */
    ret = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    if (ret != ESP_OK) {
        set_state(WIFI_MANAGER_STATE_FAILED);
        return ret;
    }

    /* IP 事件：仅注册 IP_EVENT_STA_GOT_IP（拿到 IP = 真正联网成功的关键标志） */
    ret = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);
    if (ret != ESP_OK) {
        set_state(WIFI_MANAGER_STATE_FAILED);
        return ret;
    }

    /* ---- ⑦ 判断是否已配过网：这是整个流程的"分水岭" ---- */
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

    /* ---- ⑧ 分支决策：已配网 → 直接连 Wi-Fi；未配网 → 走 BLE 配网 ---- */
    s_ip_string[0] = '\0';
    if (provisioned) {
        /* NVS 里有凭据，无需配网，直接切到 STA 模式去连路由器 */
        ESP_LOGI(TAG, "Wi-Fi already provisioned, starting station");
        network_prov_mgr_deinit();
        ret = start_wifi_sta();
    } else {
        /* 首次上电或配网信息被清过，必须走 BLE 配网让用户把凭据送进来 */
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

/*
 * 清除配网信息并重启 —— 三步走：
 *   ① 清除 NVS 中保存的 Wi-Fi 凭据（SSID/密码）
 *   ② 等待 200ms 确保 flash 写操作完成
 *   ③ 重启设备
 *
 * 重启后 WiFiManager_Init() 会重新执行，此时 network_prov_mgr_is_wifi_provisioned()
 * 返回 false，设备将进入 BLE 配网流程。
 * 典型触发场景：用户长按确认键 8 秒。
 */
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

/* 获取当前联网状态原始枚举值，供上层做细粒度判断 */
wifi_manager_state_t WiFiManager_GetState(void)
{
    return s_state;
}

/*
 * 判断设备是否已联网 —— 上层最常用的查询接口。
 * 内部只做一件事：返回 s_state == CONNECTED。
 * CONNECTED 状态只有收到 IP_EVENT_STA_GOT_IP 时才会被设置，因此该判断等价于"设备已拿到 IP"。
 */
bool WiFiManager_IsConnected(void)
{
    return s_state == WIFI_MANAGER_STATE_CONNECTED;
}

/* 状态全称字符串，用于日志输出 */
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

/* 状态简称，用于 OLED 等小屏显示空间有限的场景 */
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

/*
 * 获取当前 IP 地址字符串，如 "192.168.1.103"。
 * 未联网时 s_ip_string 为空，返回 "0.0.0.0" 作为安全默认值。
 */
const char *WiFiManager_GetIpString(void)
{
    return s_ip_string[0] != '\0' ? s_ip_string : "0.0.0.0";
}
