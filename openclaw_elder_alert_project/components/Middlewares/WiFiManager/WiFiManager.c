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

#define WIFI_MANAGER_PROV_QR_VERSION "v1"
#define WIFI_MANAGER_PROV_TRANSPORT  "ble"
#define WIFI_MANAGER_PROV_QR_BASE_URL "https://espressif.github.io/esp-jumpstart/qrcode.html"
#define WIFI_MANAGER_PROV_POP        "eldercare1234"
#define WIFI_MANAGER_PROV_SERVICE_PREFIX "PROV_"

static const char *TAG = "WiFiManager";

static bool s_initialized = false;
static bool s_provisioning_active = false;
static wifi_manager_state_t s_state = WIFI_MANAGER_STATE_IDLE;
static char s_ip_string[16] = "0.0.0.0";
static esp_netif_t *s_sta_netif = NULL;

static void set_state(wifi_manager_state_t state)
{
    if (s_state == state) {
        return;
    }

    s_state = state;
    ESP_LOGI(TAG, "wifi_state=%s", WiFiManager_GetStatusString());
}

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

static esp_err_t init_netif_once(void)
{
    esp_err_t ret = esp_netif_init();
    if (ret == ESP_ERR_INVALID_STATE) {
        return ESP_OK;
    }

    return ret;
}

static esp_err_t init_event_loop_once(void)
{
    esp_err_t ret = esp_event_loop_create_default();
    if (ret == ESP_ERR_INVALID_STATE) {
        return ESP_OK;
    }

    return ret;
}

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

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;

    if (event_base == NETWORK_PROV_EVENT) {
        switch (event_id) {
        case NETWORK_PROV_START:
            s_provisioning_active = true;
            set_state(WIFI_MANAGER_STATE_PROVISIONING);
            ESP_LOGI(TAG, "BLE provisioning started");
            break;

        case NETWORK_PROV_WIFI_CRED_RECV: {
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
            ESP_LOGI(TAG, "provisioning successful");
            if (s_state != WIFI_MANAGER_STATE_CONNECTED) {
                set_state(WIFI_MANAGER_STATE_CONNECTING);
            }
            break;

        case NETWORK_PROV_END:
            s_provisioning_active = false;
            ESP_LOGI(TAG, "provisioning finished");
            network_prov_mgr_deinit();
            break;

        default:
            break;
        }
        return;
    }

    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            if (!s_provisioning_active) {
                set_state(WIFI_MANAGER_STATE_CONNECTING);
            }
            ESP_LOGI(TAG, "wifi station started");
            esp_wifi_connect();
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *disconnected = (wifi_event_sta_disconnected_t *)event_data;
            s_ip_string[0] = '\0';
            if (s_initialized) {
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

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *got_ip = (ip_event_got_ip_t *)event_data;
        if (got_ip == NULL) {
            set_state(WIFI_MANAGER_STATE_FAILED);
            return;
        }

        snprintf(s_ip_string,
                 sizeof(s_ip_string),
                 IPSTR,
                 IP2STR(&got_ip->ip_info.ip));
        set_state(WIFI_MANAGER_STATE_CONNECTED);
        ESP_LOGI(TAG, "wifi connected, ip=%s", s_ip_string);
        return;
    }

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

    if (event_base == PROTOCOMM_SECURITY_SESSION_EVENT) {
        switch (event_id) {
        case PROTOCOMM_SECURITY_SESSION_SETUP_OK:
            ESP_LOGI(TAG, "BLE provisioning secure session established");
            break;
        case PROTOCOMM_SECURITY_SESSION_INVALID_SECURITY_PARAMS:
            ESP_LOGW(TAG, "BLE provisioning invalid security params");
            break;
        case PROTOCOMM_SECURITY_SESSION_CREDENTIALS_MISMATCH:
            ESP_LOGW(TAG, "BLE provisioning PoP mismatch");
            break;
        default:
            break;
        }
    }
}

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

static esp_err_t start_ble_provisioning(void)
{
    char service_name[16] = {0};
    get_device_service_name(service_name, sizeof(service_name));

    uint8_t custom_service_uuid[] = {
        0xb4, 0xdf, 0x5a, 0x1c, 0x3f, 0x6b, 0xf4, 0xbf,
        0xea, 0x4a, 0x82, 0x03, 0x04, 0x90, 0x1a, 0x02,
    };
    network_prov_scheme_ble_set_service_uuid(custom_service_uuid);

    s_provisioning_active = true;
    set_state(WIFI_MANAGER_STATE_PROVISIONING);

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

esp_err_t WiFiManager_Init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t ret = init_nvs_once();
    if (ret != ESP_OK) {
        set_state(WIFI_MANAGER_STATE_FAILED);
        return ret;
    }

    ret = init_netif_once();
    if (ret != ESP_OK) {
        set_state(WIFI_MANAGER_STATE_FAILED);
        return ret;
    }

    ret = init_event_loop_once();
    if (ret != ESP_OK) {
        set_state(WIFI_MANAGER_STATE_FAILED);
        return ret;
    }

    if (s_sta_netif == NULL) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
        if (s_sta_netif == NULL) {
            set_state(WIFI_MANAGER_STATE_FAILED);
            return ESP_FAIL;
        }
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret == ESP_ERR_INVALID_STATE) {
        ret = ESP_OK;
    }
    if (ret != ESP_OK) {
        set_state(WIFI_MANAGER_STATE_FAILED);
        return ret;
    }

    ret = esp_event_handler_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    if (ret != ESP_OK) {
        set_state(WIFI_MANAGER_STATE_FAILED);
        return ret;
    }

    ret = esp_event_handler_register(PROTOCOMM_TRANSPORT_BLE_EVENT,
                                     ESP_EVENT_ANY_ID,
                                     &wifi_event_handler,
                                     NULL);
    if (ret != ESP_OK) {
        set_state(WIFI_MANAGER_STATE_FAILED);
        return ret;
    }

    ret = esp_event_handler_register(PROTOCOMM_SECURITY_SESSION_EVENT,
                                     ESP_EVENT_ANY_ID,
                                     &wifi_event_handler,
                                     NULL);
    if (ret != ESP_OK) {
        set_state(WIFI_MANAGER_STATE_FAILED);
        return ret;
    }

    ret = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    if (ret != ESP_OK) {
        set_state(WIFI_MANAGER_STATE_FAILED);
        return ret;
    }

    ret = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);
    if (ret != ESP_OK) {
        set_state(WIFI_MANAGER_STATE_FAILED);
        return ret;
    }

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

    s_ip_string[0] = '\0';
    if (provisioned) {
        ESP_LOGI(TAG, "Wi-Fi already provisioned, starting station");
        network_prov_mgr_deinit();
        ret = start_wifi_sta();
    } else {
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

wifi_manager_state_t WiFiManager_GetState(void)
{
    return s_state;
}

bool WiFiManager_IsConnected(void)
{
    return s_state == WIFI_MANAGER_STATE_CONNECTED;
}

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

const char *WiFiManager_GetIpString(void)
{
    return s_ip_string[0] != '\0' ? s_ip_string : "0.0.0.0";
}
