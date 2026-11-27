#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "bsp/esp-bsp.h"
#include "iot_button.h"
#include "app_state.h"

SlotSensor slots[MAX_SLOTS];
SensorDescoberto descobertos[MAX_DESCOBERTOS];
volatile bool piscar_led_req = false;
static button_handle_t btns[BSP_BUTTON_NUM];

static void tarefa_led_rgb(void *param) {
    bool ok = (bsp_led_init() == ESP_OK);
    if (ok) bsp_led_rgb_set(0, 0, 0);
    while (1) {
        if (piscar_led_req && ok) {
            piscar_led_req = false;
            bsp_led_rgb_set(0, 40, 255); // Azul brilhante
            vTaskDelay(pdMS_TO_TICKS(150));
            bsp_led_rgb_set(0, 0, 0);
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static void botao_fisico_cb(void *arg, void *usr_data) {
    ui_alternar_historico_botao((int)(intptr_t)usr_data);
}

static void tarefa_historico(void *param) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(15 * 60 * 1000));
        ui_registar_ponto_historico();
    }
}

void app_main(void) {
    nvs_flash_init();
    for (int i = 0; i < MAX_SLOTS; i++) {
        slots[i].temp = NAN;
        slots[i].hum = NAN;
        slots[i].bat = NAN;
    }
    storage_carregar_config();

    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_H_RES * 50,
        .double_buffer = false,
        .flags = { .buff_dma = false, .buff_spiram = true }
    };
    bsp_display_start_with_config(&cfg);
    bsp_display_backlight_on();
    ui_criar_interface();

    int btn_cnt = 0;
    if (bsp_iot_button_create(btns, &btn_cnt, BSP_BUTTON_NUM) == ESP_OK) {
        for (int i = 0; i < btn_cnt && i < MAX_SLOTS; i++) {
            iot_button_register_cb(btns[i], BUTTON_SINGLE_CLICK, NULL, botao_fisico_cb, (void *)(intptr_t)i);
        }
    }

    xTaskCreate(tarefa_led_rgb, "led_task", 2048, NULL, 4, NULL);
    ble_shelly_iniciar();
    xTaskCreate(tarefa_historico, "hist_task", 4096, NULL, 5, NULL);
}
