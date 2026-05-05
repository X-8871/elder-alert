set(src_dirs 
        
                I2C
                KEY
                LED
                AHT20
                BMP280
                BH1750
                MQ2
                AM312
                Alert
                OLED)
set(include_dirs 

                I2C
                KEY
                LED
                AHT20
                BMP280
                BH1750
                MQ2
                AM312
                Alert
                OLED)
set(requires driver esp_adc i2cdev aht bmp280 bh1750 esp_lcd)

idf_component_register(SRC_DIRS ${src_dirs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires})
