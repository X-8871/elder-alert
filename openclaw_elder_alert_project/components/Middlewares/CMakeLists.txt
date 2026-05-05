set(src_dirs
        SensorHub
        RiskEngine
        AlertController
        InputController
        AppController
        DisplayController
        EventLog
        WiFiManager
        HttpAlertReporter
        RainMakerReporter)

set(include_dirs
        SensorHub
        RiskEngine
        AlertController
        InputController
        AppController
        DisplayController
        EventLog
        WiFiManager
        HttpAlertReporter
        RainMakerReporter)

set(requires BSP esp_wifi esp_event esp_netif nvs_flash esp_http_client esp-tls bt espressif__network_provisioning espressif__esp_rainmaker)

idf_component_register(SRC_DIRS ${src_dirs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})
