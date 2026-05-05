#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t BSP_BH1750_Init(void);
esp_err_t BSP_BH1750_Read(uint16_t *lux);
bool BSP_BH1750_IsInitialized(void);
