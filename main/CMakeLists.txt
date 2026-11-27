idf_component_register(SRCS "main.c" "storage.c" "ble_shelly.c" "ui_lvgl.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES bt nvs_flash)

