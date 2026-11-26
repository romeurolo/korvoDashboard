#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"

static const char *TAG = "SHELLY_GW";

// Objetos LVGL globais
static lv_obj_t *pag_dash, *pag_hist, *pag_conf;
static lv_obj_t *lbl_temp_sala, *lbl_hum_sala, *lbl_bat_sala;
static lv_obj_t *lbl_temp_quarto, *lbl_hum_quarto, *lbl_bat_quarto;
static lv_obj_t *grafico_temp;
static lv_chart_series_t *serie_sala, *serie_quarto;

static float ultima_temp_sala = NAN;
static float ultima_temp_quarto = NAN;

// --- ATUALIZAÇÃO SEGURA DA INTERFACE LVGL ---
static void atualizar_ui_sensor(int sensor_idx, float temp, float hum, float bat) {
    bsp_display_lock(0);
    char buf[32];
    if (sensor_idx == 1) { // Sala
        if (!isnan(temp)) {
            ultima_temp_sala = temp;
            snprintf(buf, sizeof(buf), "%.1f °C", temp);
            lv_label_set_text(lbl_temp_sala, buf);
        }
        if (!isnan(hum)) {
            snprintf(buf, sizeof(buf), "%.0f %%", hum);
            lv_label_set_text(lbl_hum_sala, buf);
        }
        if (!isnan(bat)) {
            snprintf(buf, sizeof(buf), "Bat: %.0f%%", bat);
            lv_label_set_text(lbl_bat_sala, buf);
        }
    } else if (sensor_idx == 2) { // Quarto
        if (!isnan(temp)) {
            ultima_temp_quarto = temp;
            snprintf(buf, sizeof(buf), "%.1f °C", temp);
            lv_label_set_text(lbl_temp_quarto, buf);
        }
        if (!isnan(hum)) {
            snprintf(buf, sizeof(buf), "%.0f %%", hum);
            lv_label_set_text(lbl_hum_quarto, buf);
        }
        if (!isnan(bat)) {
            snprintf(buf, sizeof(buf), "Bat: %.0f%%", bat);
            lv_label_set_text(lbl_bat_quarto, buf);
        }
    }
    bsp_display_unlock();
}

// --- DESCODIFICADOR BTHOME V2 (SHELLY H&T GEN3) ---
static void processar_pacote_ble(const uint8_t *addr, const uint8_t *data, uint8_t len) {
    size_t pos = 0;
    while (pos + 2 < len) {
        uint8_t field_len = data[pos];
        if (field_len == 0 || pos + field_len >= len) break;
        uint8_t field_type = data[pos + 1];

        // 0x16 = Service Data 16-bit UUID; 0xD2, 0xFC = 0xFCD2 (Little Endian)
        if (field_type == 0x16 && field_len >= 5 && data[pos + 2] == 0xD2 && data[pos + 3] == 0xFC) {
            const uint8_t *bt = &data[pos + 4];
            size_t bt_len = field_len - 3;

            if (bt_len < 2 || (bt[0] & 0x01)) return; // Ignora se encriptado

            float temp = NAN, hum = NAN, bat = NAN;
            size_t i = 1;
            while (i < bt_len) {
                uint8_t obj_id = bt[i++];
                if (obj_id == 0x01 && i < bt_len) {
                    bat = bt[i++];
                } else if (obj_id == 0x02 && i + 1 < bt_len) {
                    int16_t raw = (int16_t)(bt[i] | (bt[i + 1] << 8));
                    temp = raw * 0.01f;
                    i += 2;
                } else if (obj_id == 0x03 && i + 1 < bt_len) {
                    uint16_t raw = (uint16_t)(bt[i] | (bt[i + 1] << 8));
                    hum = raw * 0.01f;
                    i += 2;
                } else if (obj_id == 0x00 && i < bt_len) {
                    i += 1;
                } else {
                    break;
                }
            }

            char mac_str[18];
            snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                     addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);

            ESP_LOGI(TAG, "Shelly BTHome [%s] -> T:%.1f H:%.0f B:%.0f", mac_str, temp, hum, bat);

            // SUBSTITUA PELOS MACS REAIS DOS SEUS SHELLY H&T GEN3:
            if (strcmp(mac_str, "AA:BB:CC:DD:EE:01") == 0) {
                atualizar_ui_sensor(1, temp, hum, bat);
            } else if (strcmp(mac_str, "AA:BB:CC:DD:EE:02") == 0) {
                atualizar_ui_sensor(2, temp, hum, bat);
            }
        }
        pos += field_len + 1;
    }
}

// --- EVENTOS NIMBLE SCAN ---
static int ble_gap_event(struct ble_gap_event *event, void *arg) {
    if (event->type == BLE_GAP_EVENT_DISC) {
        processar_pacote_ble(event->disc.addr.val, event->disc.data, event->disc.length_data);
    }
    return 0;
}

static void iniciar_scan_ble(void) {
    struct ble_gap_disc_params disc_params = {0};
    disc_params.passive = 1;
    disc_params.itvl = 0x0060;
    disc_params.window = 0x0030;
    ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &disc_params, ble_gap_event, NULL);
}

static void ble_on_sync(void) {
    iniciar_scan_ble();
}

static void ble_host_task(void *param) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// --- CALLBACKS DOS BOTÕES LVGL ---
static void ir_para_hist(lv_event_t *e) { lv_scr_load(pag_hist); }
static void ir_para_conf(lv_event_t *e) { lv_scr_load(pag_conf); }
static void ir_para_dash(lv_event_t *e) { lv_scr_load(pag_dash); }
static void limpar_grafico(lv_event_t *e) {
    lv_chart_set_all_value(grafico_temp, serie_sala, LV_CHART_POINT_NONE);
    lv_chart_set_all_value(grafico_temp, serie_quarto, LV_CHART_POINT_NONE);
}

// --- CRIAÇÃO DAS 3 PÁGINAS NO ECRÃ 800x480 ---
static void criar_interface(void) {
    bsp_display_lock(0);

    // 1. PÁGINA DASHBOARD
    pag_dash = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(pag_dash, lv_color_hex(0x0F172A), 0);

    lv_obj_t *titulo = lv_label_create(pag_dash);
    lv_label_set_text(titulo, "DASHBOARD SHELLY H&T");
    lv_obj_set_style_text_color(titulo, lv_color_hex(0x38BDF8), 0);
    lv_obj_set_pos(titulo, 20, 20);

    lv_obj_t *btn_h = lv_btn_create(pag_dash);
    lv_obj_set_size(btn_h, 120, 40);
    lv_obj_set_pos(btn_h, 520, 12);
    lv_obj_add_event_cb(btn_h, ir_para_hist, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(lv_label_create(btn_h), "Historico");

    lv_obj_t *btn_c = lv_btn_create(pag_dash);
    lv_obj_set_size(btn_c, 125, 40);
    lv_obj_set_pos(btn_c, 655, 12);
    lv_obj_add_event_cb(btn_c, ir_para_conf, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(lv_label_create(btn_c), "Configurar");

    // Cartão Sala
    lv_obj_t *card1 = lv_obj_create(pag_dash);
    lv_obj_set_size(card1, 370, 380);
    lv_obj_set_pos(card1, 20, 75);
    lv_obj_set_style_bg_color(card1, lv_color_hex(0x1E293B), 0);
    lv_label_set_text(lv_label_create(card1), "Sala de Estar");

    lbl_temp_sala = lv_label_create(card1);
    lv_label_set_text(lbl_temp_sala, "--.- °C");
    lv_obj_set_style_text_color(lbl_temp_sala, lv_color_hex(0x38BDF8), 0);
    lv_obj_set_pos(lbl_temp_sala, 10, 60);

    lbl_hum_sala = lv_label_create(card1);
    lv_label_set_text(lbl_hum_sala, "-- %");
    lv_obj_set_style_text_color(lbl_hum_sala, lv_color_hex(0x2DD4BF), 0);
    lv_obj_set_pos(lbl_hum_sala, 10, 130);

    lbl_bat_sala = lv_label_create(card1);
    lv_label_set_text(lbl_bat_sala, "Bat: --%");
    lv_obj_set_pos(lbl_bat_sala, 10, 280);

    // Cartão Quarto
    lv_obj_t *card2 = lv_obj_create(pag_dash);
    lv_obj_set_size(card2, 370, 380);
    lv_obj_set_pos(card2, 410, 75);
    lv_obj_set_style_bg_color(card2, lv_color_hex(0x1E293B), 0);
    lv_label_set_text(lv_label_create(card2), "Quarto");

    lbl_temp_quarto = lv_label_create(card2);
    lv_label_set_text(lbl_temp_quarto, "--.- °C");
    lv_obj_set_style_text_color(lbl_temp_quarto, lv_color_hex(0x38BDF8), 0);
    lv_obj_set_pos(lbl_temp_quarto, 10, 60);

    lbl_hum_quarto = lv_label_create(card2);
    lv_label_set_text(lbl_hum_quarto, "-- %");
    lv_obj_set_style_text_color(lbl_hum_quarto, lv_color_hex(0x2DD4BF), 0);
    lv_obj_set_pos(lbl_hum_quarto, 10, 130);

    lbl_bat_quarto = lv_label_create(card2);
    lv_label_set_text(lbl_bat_quarto, "Bat: --%");
    lv_obj_set_pos(lbl_bat_quarto, 10, 280);

    // 2. PÁGINA HISTÓRICO
    pag_hist = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(pag_hist, lv_color_hex(0x0F172A), 0);
    lv_obj_t *btn_v1 = lv_btn_create(pag_hist);
    lv_obj_set_size(btn_v1, 125, 40);
    lv_obj_set_pos(btn_v1, 655, 12);
    lv_obj_add_event_cb(btn_v1, ir_para_dash, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(lv_label_create(btn_v1), "Voltar");

    grafico_temp = lv_chart_create(pag_hist);
    lv_obj_set_size(grafico_temp, 760, 380);
    lv_obj_set_pos(grafico_temp, 20, 75);
    lv_chart_set_type(grafico_temp, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(grafico_temp, 96);
    lv_chart_set_range(grafico_temp, LV_CHART_AXIS_PRIMARY_Y, 0, 45);
    serie_sala = lv_chart_add_series(grafico_temp, lv_color_hex(0x38BDF8), LV_CHART_AXIS_PRIMARY_Y);
    serie_quarto = lv_chart_add_series(grafico_temp, lv_color_hex(0x2DD4BF), LV_CHART_AXIS_PRIMARY_Y);

    // 3. PÁGINA CONFIGURAÇÃO
    pag_conf = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(pag_conf, lv_color_hex(0x0F172A), 0);
    lv_obj_t *btn_v2 = lv_btn_create(pag_conf);
    lv_obj_set_size(btn_v2, 125, 40);
    lv_obj_set_pos(btn_v2, 655, 12);
    lv_obj_add_event_cb(btn_v2, ir_para_dash, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(lv_label_create(btn_v2), "Voltar");

    lv_obj_t *btn_limpar = lv_btn_create(pag_conf);
    lv_obj_set_size(btn_limpar, 250, 50);
    lv_obj_set_pos(btn_limpar, 40, 100);
    lv_obj_add_event_cb(btn_limpar, limpar_grafico, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(lv_label_create(btn_limpar), "Limpar Historico");

    lv_scr_load(pag_dash);
    bsp_display_unlock();
}

// --- TAREFA DE REGISTO PERIÓDICO NO GRÁFICO (A CADA 15 MIN) ---
static void tarefa_historico(void *param) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(15 * 60 * 1000));
        bsp_display_lock(0);
        if (!isnan(ultima_temp_sala)) {
            lv_chart_set_next_value(grafico_temp, serie_sala, (lv_coord_t)ultima_temp_sala);
        }
        if (!isnan(ultima_temp_quarto)) {
            lv_chart_set_next_value(grafico_temp, serie_quarto, (lv_coord_t)ultima_temp_quarto);
        }
bsp_display_unlock();
}
}
void app_main(void) {
    nvs_flash_init();

    // Inicializa Ecrã 800x480 e LVGL alocando o buffer na PSRAM de 16MB
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_H_RES * 50,
        .double_buffer = false,
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,
        }
    };
    bsp_display_start_with_config(&cfg);
    bsp_display_backlight_on();
    criar_interface();

    // Inicializa Bluetooth NimBLE
    nimble_port_init();
    ble_hs_cfg.sync_cb = ble_on_sync;
    nimble_port_freertos_init(ble_host_task);

    // Inicia gravação periódica no gráfico
    xTaskCreate(tarefa_historico, "hist_task", 4096, NULL, 5, NULL);
}

