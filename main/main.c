#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "bsp/esp-bsp.h"
#include "iot_button.h"
#include "lvgl.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"

static const char *TAG = "SHELLY_GW";

#define MAX_SLOTS 4
#define MAX_DESCOBERTOS 12
#define TAM_NOME 24
#define TAM_MAC 20
#define TAM_KEY 16
#define TAM_BUF 64
#define TAM_MSG 96
#define TAM_OPCOES 512

#define IDX_0 0
#define IDX_1 1
#define IDX_2 2
#define IDX_3 3
#define IDX_4 4
#define IDX_5 5
#define IDX_17 17

static const uint32_t CORES_SLOTS[MAX_SLOTS] = {0x38BDF8, 0x2DD4BF, 0xFBBF24, 0xF472B6};

typedef struct {
    char nome[TAM_NOME];
    char mac[TAM_MAC];
    float temp;
    float hum;
    float bat;
    bool ativo;
    uint8_t ultimo_pkt_id;
    bool tem_pkt_id;
    int ciclos_piscar;
} SlotSensor;

typedef struct {
    char mac[TAM_MAC];
    float ultima_temp;
    bool valido;
} SensorDescoberto;

static SlotSensor slots[MAX_SLOTS];
static SensorDescoberto descobertos[MAX_DESCOBERTOS];

// Hardware Onboard (Botões e LED)
static button_handle_t btns[BSP_BUTTON_NUM];
static volatile bool piscar_led_azul_req = false;

// Estado de navegação
static int divisao_hist_ativa = -1;

// Objetos LVGL globais
static lv_obj_t *pag_dash, *pag_hist, *pag_conf;
static lv_obj_t *cards[MAX_SLOTS];
static lv_obj_t *lbl_nome_card[MAX_SLOTS], *lbl_temp[MAX_SLOTS], *lbl_hum[MAX_SLOTS], *lbl_bat[MAX_SLOTS], *lbl_mac[MAX_SLOTS];
static lv_obj_t *lbl_titulo_hist, *grafico_temp;
static lv_chart_series_t *series[MAX_SLOTS];
static lv_obj_t *dd_descobertos, *dd_slot, *ta_nome_div, *teclado, *lbl_status_conf;

// --- GESTÃO DE MEMÓRIA PERMANENTE (NVS FLASH) ---
static void carregar_config_nvs(void) {
    for (int i = 0; i < MAX_SLOTS; i++) {
        snprintf(slots[i].nome, sizeof(slots[i].nome), "Divisao %d", i + 1);
    }

    nvs_handle_t h;
    if (nvs_open("shelly_cfg", NVS_READONLY, &h) == ESP_OK) {
        for (int i = 0; i < MAX_SLOTS; i++) {
            char key_mac[TAM_KEY];
            char key_nome[TAM_KEY];
            snprintf(key_mac, sizeof(key_mac), "mac_%d", i);
            snprintf(key_nome, sizeof(key_nome), "nome_%d", i);

            size_t len_mac = sizeof(slots[i].mac);
            if (nvs_get_str(h, key_mac, slots[i].mac, &len_mac) == ESP_OK && strlen(slots[i].mac) > 0) {
                slots[i].ativo = true;
                ESP_LOGI(TAG, "Slot %d carregado: %s", i, slots[i].mac);
            }

            size_t len_nome = sizeof(slots[i].nome);
            nvs_get_str(h, key_nome, slots[i].nome, &len_nome);
        }
        nvs_close(h);
    }
}

static void guardar_mac_nvs(int slot_idx, const char *mac) {
    nvs_handle_t h;
    if (nvs_open("shelly_cfg", NVS_READWRITE, &h) == ESP_OK) {
        char key[TAM_KEY];
        snprintf(key, sizeof(key), "mac_%d", slot_idx);
        nvs_set_str(h, key, mac);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void guardar_nome_nvs(int slot_idx, const char *novo_nome) {
    nvs_handle_t h;
    if (nvs_open("shelly_cfg", NVS_READWRITE, &h) == ESP_OK) {
        char key[TAM_KEY];
        snprintf(key, sizeof(key), "nome_%d", slot_idx);
        nvs_set_str(h, key, novo_nome);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void limpar_emparelhamentos_nvs(void) {
    nvs_handle_t h;
    if (nvs_open("shelly_cfg", NVS_READWRITE, &h) == ESP_OK) {
        for (int i = 0; i < MAX_SLOTS; i++) {
            char key_mac[TAM_KEY];
            snprintf(key_mac, sizeof(key_mac), "mac_%d", i);
            nvs_erase_key(h, key_mac);
        }
        nvs_commit(h);
        nvs_close(h);
    }
    for (int i = 0; i < MAX_SLOTS; i++) {
        slots[i].mac[IDX_0] = '\0';
        slots[i].ativo = false;
        slots[i].temp = NAN;
        slots[i].hum = NAN;
        slots[i].bat = NAN;
        slots[i].tem_pkt_id = false;
    }
}

// --- TAREFA DO LED ONBOARD VIA BSP OFICIAL ---
static void tarefa_led_rgb(void *param) {
    bool led_ok = (bsp_leds_init() == ESP_OK);
    if (led_ok) {
        bsp_led_set(BSP_LED_GREEN, false);
    }

    while (1) {
        if (piscar_led_azul_req && led_ok) {
            piscar_led_azul_req = false;
            bsp_led_set(BSP_LED_GREEN, true);
            vTaskDelay(pdMS_TO_TICKS(150));
            bsp_led_set(BSP_LED_GREEN, false);
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

// --- TEMPORIZADOR PARA PISCAR CARTÕES QUANDO O BOTÃO DO SHELLY É PREMIDO ---
static void timer_piscar_cb(lv_timer_t *timer) {
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (slots[i].ciclos_piscar > 0) {
            if (slots[i].ciclos_piscar % 2 == 0) {
                lv_obj_set_style_bg_color(cards[i], lv_color_hex(0xFFFFFF), 0);
            } else {
                lv_obj_set_style_bg_color(cards[i], lv_color_hex(0x1E293B), 0);
            }
            slots[i].ciclos_piscar--;
            if (slots[i].ciclos_piscar == 0) {
                lv_obj_set_style_bg_color(cards[i], lv_color_hex(0x1E293B), 0);
            }
        }
    }
}

// --- ATUALIZAÇÃO DA UI ---
static void atualizar_dropdown_divisoes(void) {
    int sel_atual = lv_dropdown_get_selected(dd_slot);
    if (sel_atual < 0 || sel_atual >= MAX_SLOTS) sel_atual = 0;

    char opcoes[TAM_OPCOES] = "";
    for (int i = 0; i < MAX_SLOTS; i++) {
        char item[TAM_BUF];
        snprintf(item, sizeof(item), "%d: %s", i + 1, slots[i].nome);
        if (i > 0) strcat(opcoes, "\n");
        strcat(opcoes, item);
    }
    lv_dropdown_set_options(dd_slot, opcoes);
    lv_dropdown_set_selected(dd_slot, sel_atual);
    lv_textarea_set_text(ta_nome_div, slots[sel_atual].nome);
}

static void atualizar_lista_dropdown_ble(void) {
    char opcoes[TAM_OPCOES] = "";
    int count = 0;
    for (int i = 0; i < MAX_DESCOBERTOS; i++) {
        if (descobertos[i].valido) {
            char linha[TAM_BUF];
            if (!isnan(descobertos[i].ultima_temp)) {
                snprintf(linha, sizeof(linha), "%s (%.1f°C)", descobertos[i].mac, descobertos[i].ultima_temp);
            } else {
                snprintf(linha, sizeof(linha), "%s", descobertos[i].mac);
            }
            if (count > 0) strcat(opcoes, "\n");
            strcat(opcoes, linha);
            count++;
        }
    }
    if (count == 0) {
        lv_dropdown_set_options(dd_descobertos, "A procurar Shelly BLE...");
    } else {
        lv_dropdown_set_options(dd_descobertos, opcoes);
    }
}

static void atualizar_cards_dashboard(void) {
    char buf[TAM_BUF];
    for (int i = 0; i < MAX_SLOTS; i++) {
        lv_label_set_text(lbl_nome_card[i], slots[i].nome);
        if (!slots[i].ativo) {
            lv_label_set_text(lbl_mac[i], "Nao emparelhado");
            lv_label_set_text(lbl_temp[i], "--.- °C");
            lv_label_set_text(lbl_hum[i], "-- %");
            lv_label_set_text(lbl_bat[i], "Bat: --%");
        } else {
            lv_label_set_text(lbl_mac[i], slots[i].mac);
            if (!isnan(slots[i].temp)) {
                snprintf(buf, sizeof(buf), "%.1f °C", slots[i].temp);
                lv_label_set_text(lbl_temp[i], buf);
            }
            if (!isnan(slots[i].hum)) {
                snprintf(buf, sizeof(buf), "%.0f %%", slots[i].hum);
                lv_label_set_text(lbl_hum[i], buf);
            }
            if (!isnan(slots[i].bat)) {
                snprintf(buf, sizeof(buf), "Bat: %.0f%%", slots[i].bat);
                lv_label_set_text(lbl_bat[i], buf);
            }
        }
    }
}

static void abrir_historico_divisao(int slot_idx) {
    divisao_hist_ativa = slot_idx;
    char tit[TAM_BUF];
    if (slot_idx == -1) {
        snprintf(tit, sizeof(tit), "HISTORICO 24H (Todas as Divisoes)");
        for (int i = 0; i < MAX_SLOTS; i++) {
            lv_chart_hide_series(grafico_temp, series[i], false);
        }
    } else {
        snprintf(tit, sizeof(tit), "HISTORICO 24H - %s", slots[slot_idx].nome);
        for (int i = 0; i < MAX_SLOTS; i++) {
            lv_chart_hide_series(grafico_temp, series[i], (i != slot_idx));
        }
    }
    lv_label_set_text(lbl_titulo_hist, tit);
    lv_scr_load(pag_hist);
}

// --- CALLBACK DOS 4 BOTÕES FÍSICOS DA PLACA KORVO-1 ---
static void botao_fisico_cb(void *arg, void *usr_data) {
    int btn_idx = (int)(intptr_t)usr_data;
    if (btn_idx < 0 || btn_idx >= MAX_SLOTS) return;

    bsp_display_lock(0);
    lv_obj_t *ecra_atual = lv_scr_act();

    if (ecra_atual == pag_hist && divisao_hist_ativa == btn_idx) {
        divisao_hist_ativa = -1;
        atualizar_cards_dashboard();
        lv_scr_load(pag_dash);
    } else {
        abrir_historico_divisao(btn_idx);
    }
    bsp_display_unlock();
}

// --- DESCODIFICADOR BTHOME V2 COMPLETO (SHELLY H&T GEN3) ---
static void processar_pacote_ble(const uint8_t *addr, const uint8_t *data, uint8_t len) {
    size_t pos = 0;
    while (pos + 2 < len) {
        uint8_t field_len = data[pos];
        if (field_len == 0 || pos + field_len >= len) break;
        uint8_t field_type = data[pos + IDX_1];

        if (field_type == 0x16 && field_len >= 5 && data[pos + IDX_2] == 0xD2 && data[pos + IDX_3] == 0xFC) {
            const uint8_t *bt = &data[pos + IDX_4];
            size_t bt_len = field_len - 3;

            if (bt_len < 2 || (bt[IDX_0] & 0x01)) return;

            float temp = NAN, hum = NAN, bat = NAN;
            uint8_t pkt_id = 0;
            bool has_pkt_id = false;
            bool botao_premido = false;

            size_t i = 1;
            while (i < bt_len) {
                uint8_t obj_id = bt[i++];
                if (obj_id == 0x00 && i < bt_len) {
                    pkt_id = bt[i++];
                    has_pkt_id = true;
                } else if (obj_id == 0x01 && i < bt_len) {
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "bsp/esp-bsp.h"
#include "iot_button.h"
#include "lvgl.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"

static const char *TAG = "SHELLY_GW";

#define MAX_SLOTS 4
#define MAX_DESCOBERTOS 12
#define TAM_NOME 24
#define TAM_MAC 20
#define TAM_KEY 16
#define TAM_BUF 64
#define TAM_MSG 96
#define TAM_OPCOES 512

#define IDX_0 0
#define IDX_1 1
#define IDX_2 2
#define IDX_3 3
#define IDX_4 4
#define IDX_5 5
#define IDX_17 17

static const uint32_t CORES_SLOTS[MAX_SLOTS] = {0x38BDF8, 0x2DD4BF, 0xFBBF24, 0xF472B6};

typedef struct {
    char nome[TAM_NOME];
    char mac[TAM_MAC];
    float temp;
    float hum;
    float bat;
    bool ativo;
    uint8_t ultimo_pkt_id;
    bool tem_pkt_id;
    int ciclos_piscar;
} SlotSensor;

typedef struct {
    char mac[TAM_MAC];
    float ultima_temp;
    bool valido;
} SensorDescoberto;

static SlotSensor slots[MAX_SLOTS];
static SensorDescoberto descobertos[MAX_DESCOBERTOS];

// Hardware Onboard (Botões e LED)
static button_handle_t btns[BSP_BUTTON_NUM];
static volatile bool piscar_led_azul_req = false;

// Estado de navegação
static int divisao_hist_ativa = -1;

// Objetos LVGL globais
static lv_obj_t *pag_dash, *pag_hist, *pag_conf;
static lv_obj_t *cards[MAX_SLOTS];
static lv_obj_t *lbl_nome_card[MAX_SLOTS], *lbl_temp[MAX_SLOTS], *lbl_hum[MAX_SLOTS], *lbl_bat[MAX_SLOTS], *lbl_mac[MAX_SLOTS];
static lv_obj_t *lbl_titulo_hist, *grafico_temp;
static lv_chart_series_t *series[MAX_SLOTS];
static lv_obj_t *dd_descobertos, *dd_slot, *ta_nome_div, *teclado, *lbl_status_conf;

// --- GESTÃO DE MEMÓRIA PERMANENTE (NVS FLASH) ---
static void carregar_config_nvs(void) {
    for (int i = 0; i < MAX_SLOTS; i++) {
        snprintf(slots[i].nome, sizeof(slots[i].nome), "Divisao %d", i + 1);
    }

    nvs_handle_t h;
    if (nvs_open("shelly_cfg", NVS_READONLY, &h) == ESP_OK) {
        for (int i = 0; i < MAX_SLOTS; i++) {
            char key_mac[TAM_KEY];
            char key_nome[TAM_KEY];
            snprintf(key_mac, sizeof(key_mac), "mac_%d", i);
            snprintf(key_nome, sizeof(key_nome), "nome_%d", i);

            size_t len_mac = sizeof(slots[i].mac);
            if (nvs_get_str(h, key_mac, slots[i].mac, &len_mac) == ESP_OK && strlen(slots[i].mac) > 0) {
                slots[i].ativo = true;
                ESP_LOGI(TAG, "Slot %d carregado: %s", i, slots[i].mac);
            }

            size_t len_nome = sizeof(slots[i].nome);
            nvs_get_str(h, key_nome, slots[i].nome, &len_nome);
        }
        nvs_close(h);
    }
}

static void guardar_mac_nvs(int slot_idx, const char *mac) {
    nvs_handle_t h;
    if (nvs_open("shelly_cfg", NVS_READWRITE, &h) == ESP_OK) {
        char key[TAM_KEY];
        snprintf(key, sizeof(key), "mac_%d", slot_idx);
        nvs_set_str(h, key, mac);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void guardar_nome_nvs(int slot_idx, const char *novo_nome) {
    nvs_handle_t h;
    if (nvs_open("shelly_cfg", NVS_READWRITE, &h) == ESP_OK) {
        char key[TAM_KEY];
        snprintf(key, sizeof(key), "nome_%d", slot_idx);
        nvs_set_str(h, key, novo_nome);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void limpar_emparelhamentos_nvs(void) {
    nvs_handle_t h;
    if (nvs_open("shelly_cfg", NVS_READWRITE, &h) == ESP_OK) {
        for (int i = 0; i < MAX_SLOTS; i++) {
            char key_mac[TAM_KEY];
            snprintf(key_mac, sizeof(key_mac), "mac_%d", i);
            nvs_erase_key(h, key_mac);
        }
        nvs_commit(h);
        nvs_close(h);
    }
    for (int i = 0; i < MAX_SLOTS; i++) {
        slots[i].mac[IDX_0] = '\0';
        slots[i].ativo = false;
        slots[i].temp = NAN;
        slots[i].hum = NAN;
        slots[i].bat = NAN;
        slots[i].tem_pkt_id = false;
    }
}

// --- TAREFA DO LED ONBOARD VIA BSP OFICIAL ---
static void tarefa_led_rgb(void *param) {
    bool led_ok = (bsp_leds_init() == ESP_OK);
    if (led_ok) {
        bsp_led_set(BSP_LED_GREEN, false);
    }

    while (1) {
        if (piscar_led_azul_req && led_ok) {
            piscar_led_azul_req = false;
            bsp_led_set(BSP_LED_GREEN, true);
            vTaskDelay(pdMS_TO_TICKS(150));
            bsp_led_set(BSP_LED_GREEN, false);
        }
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

// --- TEMPORIZADOR PARA PISCAR CARTÕES QUANDO O BOTÃO DO SHELLY É PREMIDO ---
static void timer_piscar_cb(lv_timer_t *timer) {
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (slots[i].ciclos_piscar > 0) {
            if (slots[i].ciclos_piscar % 2 == 0) {
                lv_obj_set_style_bg_color(cards[i], lv_color_hex(0xFFFFFF), 0);
            } else {
                lv_obj_set_style_bg_color(cards[i], lv_color_hex(0x1E293B), 0);
            }
            slots[i].ciclos_piscar--;
            if (slots[i].ciclos_piscar == 0) {
                lv_obj_set_style_bg_color(cards[i], lv_color_hex(0x1E293B), 0);
            }
        }
    }
}

// --- ATUALIZAÇÃO DA UI ---
static void atualizar_dropdown_divisoes(void) {
    int sel_atual = lv_dropdown_get_selected(dd_slot);
    if (sel_atual < 0 || sel_atual >= MAX_SLOTS) sel_atual = 0;

    char opcoes[TAM_OPCOES] = "";
    for (int i = 0; i < MAX_SLOTS; i++) {
        char item[TAM_BUF];
        snprintf(item, sizeof(item), "%d: %s", i + 1, slots[i].nome);
        if (i > 0) strcat(opcoes, "\n");
        strcat(opcoes, item);
    }
    lv_dropdown_set_options(dd_slot, opcoes);
    lv_dropdown_set_selected(dd_slot, sel_atual);
    lv_textarea_set_text(ta_nome_div, slots[sel_atual].nome);
}

static void atualizar_lista_dropdown_ble(void) {
    char opcoes[TAM_OPCOES] = "";
    int count = 0;
    for (int i = 0; i < MAX_DESCOBERTOS; i++) {
        if (descobertos[i].valido) {
            char linha[TAM_BUF];
            if (!isnan(descobertos[i].ultima_temp)) {
                snprintf(linha, sizeof(linha), "%s (%.1f°C)", descobertos[i].mac, descobertos[i].ultima_temp);
            } else {
                snprintf(linha, sizeof(linha), "%s", descobertos[i].mac);
            }
            if (count > 0) strcat(opcoes, "\n");
            strcat(opcoes, linha);
            count++;
        }
    }
    if (count == 0) {
        lv_dropdown_set_options(dd_descobertos, "A procurar Shelly BLE...");
    } else {
        lv_dropdown_set_options(dd_descobertos, opcoes);
    }
}

static void atualizar_cards_dashboard(void) {
    char buf[TAM_BUF];
    for (int i = 0; i < MAX_SLOTS; i++) {
        lv_label_set_text(lbl_nome_card[i], slots[i].nome);
        if (!slots[i].ativo) {
            lv_label_set_text(lbl_mac[i], "Nao emparelhado");
            lv_label_set_text(lbl_temp[i], "--.- °C");
            lv_label_set_text(lbl_hum[i], "-- %");
            lv_label_set_text(lbl_bat[i], "Bat: --%");
        } else {
            lv_label_set_text(lbl_mac[i], slots[i].mac);
            if (!isnan(slots[i].temp)) {
                snprintf(buf, sizeof(buf), "%.1f °C", slots[i].temp);
                lv_label_set_text(lbl_temp[i], buf);
            }
            if (!isnan(slots[i].hum)) {
                snprintf(buf, sizeof(buf), "%.0f %%", slots[i].hum);
                lv_label_set_text(lbl_hum[i], buf);
            }
            if (!isnan(slots[i].bat)) {
                snprintf(buf, sizeof(buf), "Bat: %.0f%%", slots[i].bat);
                lv_label_set_text(lbl_bat[i], buf);
            }
        }
    }
}

static void abrir_historico_divisao(int slot_idx) {
    divisao_hist_ativa = slot_idx;
    char tit[TAM_BUF];
    if (slot_idx == -1) {
        snprintf(tit, sizeof(tit), "HISTORICO 24H (Todas as Divisoes)");
        for (int i = 0; i < MAX_SLOTS; i++) {
            lv_chart_hide_series(grafico_temp, series[i], false);
        }
    } else {
        snprintf(tit, sizeof(tit), "HISTORICO 24H - %s", slots[slot_idx].nome);
        for (int i = 0; i < MAX_SLOTS; i++) {
            lv_chart_hide_series(grafico_temp, series[i], (i != slot_idx));
        }
    }
    lv_label_set_text(lbl_titulo_hist, tit);
    lv_scr_load(pag_hist);
}

// --- CALLBACK DOS 4 BOTÕES FÍSICOS DA PLACA KORVO-1 ---
static void botao_fisico_cb(void *arg, void *usr_data) {
    int btn_idx = (int)(intptr_t)usr_data;
    if (btn_idx < 0 || btn_idx >= MAX_SLOTS) return;

    bsp_display_lock(0);
    lv_obj_t *ecra_atual = lv_scr_act();

    if (ecra_atual == pag_hist && divisao_hist_ativa == btn_idx) {
        divisao_hist_ativa = -1;
        atualizar_cards_dashboard();
        lv_scr_load(pag_dash);
    } else {
        abrir_historico_divisao(btn_idx);
    }
    bsp_display_unlock();
}

// --- DESCODIFICADOR BTHOME V2 COMPLETO (SHELLY H&T GEN3) ---
static void processar_pacote_ble(const uint8_t *addr, const uint8_t *data, uint8_t len) {
    size_t pos = 0;
    while (pos + 2 < len) {
        uint8_t field_len = data[pos];
        if (field_len == 0 || pos + field_len >= len) break;
        uint8_t field_type = data[pos + IDX_1];

        if (field_type == 0x16 && field_len >= 5 && data[pos + IDX_2] == 0xD2 && data[pos + IDX_3] == 0xFC) {
            const uint8_t *bt = &data[pos + IDX_4];
            size_t bt_len = field_len - 3;

            if (bt_len < 2 || (bt[IDX_0] & 0x01)) return;

            float temp = NAN, hum = NAN, bat = NAN;
            uint8_t pkt_id = 0;
            bool has_pkt_id = false;
            bool botao_premido = false;

            size_t i = 1;
            while (i < bt_len) {
                uint8_t obj_id = bt[i++];
                if (obj_id == 0x00 && i < bt_len) {
                    pkt_id = bt[i++];
                    has_pkt_id = true;
                } else if (obj_id == 0x01 && i < bt_len) {
