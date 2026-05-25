idf_component_register(
    SRCS
        "main.c"
        "relay_seq.c"
        "lcd.c"
        "ui.c"
        "led.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        driver
        esp_rom
        nvs_flash
        espressif__esp-zigbee-lib
        espressif__esp-zboss-lib
        espressif__led_strip
)
