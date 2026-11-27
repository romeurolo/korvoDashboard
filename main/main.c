#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"

static const char *TAG = "SHELLY_GW";
#define MAX_SLOTS 4
#define MAX_DESCOBERTOS 12

static const uint32_t CORES_SLOTS[MAX_SLOTS] = {0x38BDF8, 0x2DD4BF, 0xFBBF24, 0xF472B6};

typedef struct {
    char nome[24];
    char mac[18];
    float temp;
    float hum;
    float bat;
    bool ativo;
} SlotSensor;

typedef struct {
    char mac[18];
    float ultima_temp;
    bool valido;
} SensorDescoberto;

static SlotSensor slots[MAX_SLOTS];
static SensorDescoberto descobertos[MAX_DESCOBERTOS];

// Objetos LVGL globais
static lv_obj_t *pag_dash, *pag_hist, *pag_conf;
static lv_obj_t *lbl_nome_card[MAX_SLOTS], *lbl_temp[MAX_SLOTS], *lbl_hum[MAX_SLOTS], *lbl_bat[MAX_SLOTS], *lbl_mac[MAX_SLOTS];
static lv_obj_t *grafico_temp;
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
            char key_mac[16], key_nome[16];
            snprintf(key_mac, sizeof(key_mac), "mac_%d", i);
            snprintf(key_nome, sizeof(key_nome), "nome_%d", i);

            size_t len_mac = sizeof(slots[i].mac);
            if (nvs_get_str(h, key_mac, slots[i].mac, &len_mac) == ESP_OK && strlen(slots[i].mac) > 0) {
                slots[i].ativo = true;
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
        char key[16];
        snprintf(key, sizeof(key), "mac_%d", slot_idx);
        nvs_set_str(h, key, mac);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void guardar_nome_nvs(int slot_idx, const char *novo_nome) {
    nvs_handle_t h;
    if (nvs_open("shelly_cfg", NVS_READWRITE, &h) == ESP_OK) {
        char key[16];
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
            char key_mac[16];
            snprintf(key_mac, sizeof(key_mac), "mac_%d", i);
            nvs_erase_key(h, key_mac);
        }
        nvs_commit(h);
        nvs_close(h);
    }
    for (int i = 0; i < MAX_SLOTS; i++) {
        slots[i].mac[0] = '\0';
        slots[i].ativo = false;
        slots[i].temp = NAN;
        slots[i].hum = NAN;
        slots[i].bat = NAN;
    }
}

// --- ATUALIZAÇÃO DA UI ---
static void atualizar_dropdown_divisoes(void) {
    int sel_atual = lv_dropdown_get_selected(dd_slot);
    if (sel_atual < 0 || sel_atual >= MAX_SLOTS) sel_atual = 0;

    char opcoes[256] = "";
    for (int i = 0; i < MAX_SLOTS; i++) {
        char item[40];
        snprintf(item, sizeof(item), "%d: %s", i + 1, slots[i].nome);
        if (i > 0) strcat(opcoes, "\n");
        strcat(opcoes, item);
    }
    lv_dropdown_set_options(dd_slot, opcoes);
    lv_dropdown_set_selected(dd_slot, sel_atual);
    lv_textarea_set_text(ta_nome_div, slots[sel_atual].nome);
}

static void atualizar_lista_dropdown_ble(void) {
    char opcoes[512] = "";
    int count = 0;
    for (int i = 0; i < MAX_DESCOBERTOS; i++) {
        if (descobertos[i].valido) {
            char linha[40];
            snprintf(linha, sizeof(linha), "%s (%.1f°C)", descobertos[i].mac, descobertos[i].ultima_temp);
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
    char buf[32];
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

// --- DESCODIFICADOR BTHOME V2 COMPLETO (SHELLY H&T GEN3) ---
static void processar_pacote_ble(const uint8_t *addr, const uint8_t *data, uint8_t len) {
    size_t pos = 0;
    while (pos + 2 < len) {
        uint8_t field_len = data[pos];
        if (field_len == 0 || pos + field_len >= len) break;
        uint8_t field_type = data[pos + 1];

        // 0x16 = Service Data 16-bit UUID; 0xD2, 0xFC = 0xFCD2 (BTHome)
        if (field_type == 0x16 && field_len >= 5 && data[pos + 2] == 0xD2 && data[pos + 3] == 0xFC) {
            const uint8_t *bt = &data[pos + 4];
            size_t bt_len = field_len - 3;

            if (bt_len < 2 || (bt & 0x01)) return; // Ignora se encriptado

            float temp = NAN, hum = NAN, bat = NAN;
            size_t i = 1; // Salta byte 0 (Device Info)

            while (i < bt_len) {
                uint8_t obj_id = bt[i++];
                if (obj_id == 0x00 && i < bt_len) { // Packet ID (uint8)
                    i += 1;
                } else if (obj_id == 0x01 && i < bt_len) { // Bateria (uint8, %)
                    bat = (float)bt[i++];
                } else if (obj_id == 0x02 && i + 1 < bt_len) { // Temperatura (sint16, 0.01 C)
                    int16_t raw = (int16_t)(bt[i] | (bt[i + 1] << 8));
                    temp = raw * 0.01f;
                    i += 2;
                } else if (obj_id == 0x45 && i + 1 < bt_len) { // Temperatura Shelly Gen3 (sint16, 0.1 C)
                    int16_t raw = (int16_t)(bt[i] | (bt[i + 1] << 8));
                    temp = raw * 0.1f;
                    i += 2;
                } else if (obj_id == 0x03 && i + 1 < bt_len) { // Humidade (uint16, 0.01 %)
                    uint16_t raw = (uint16_t)(bt[i] | (bt[i + 1] << 8));
                    hum = raw * 0.01f;
                    i += 2;
                } else if (obj_id == 0x2E && i < bt_len) { // Humidade Shelly Gen3 (uint8, 1 %)
                    hum = (float)bt[i++];
                } else if (obj_id == 0x3A && i < bt_len) { // Botão (uint8)
                    i += 1;
                } else {
                    ESP_LOGW(TAG, "Objeto BTHome desconhecido: 0x%02X no byte %d", obj_id, (int)i);
                    break;
                }
            }

            char mac_str;
            snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                     addr, addr, addr, addr, addr, addr);

            ESP_LOGI(TAG, "Shelly [%s] -> Temp: %.1f C | Hum: %.0f %% | Bat: %.0f %%", mac_str, temp, hum, bat);

            bsp_display_lock(0);

            bool novo_descoberto = false;
            int idx_desc = -1;
            for (int d = 0; d < MAX_DESCOBERTOS; d++) {
                if (descobertos[d].valido && strcmp(descobertos[d].mac, mac_str) == 0) {
                    idx_desc = d;
                    break;
                }
                if (!descobertos[d].valido && idx_desc == -1) {
                    idx_desc = d;
                }
            }
            if (idx_desc != -1) {
                if (!descobertos[idx_desc].valido) novo_descoberto = true;
                strcpy(descobertos[idx_desc].mac, mac_str);
                if (!isnan(temp)) descobertos[idx_desc].ultima_temp = temp;
                descobertos[idx_desc].valido = true;
                if (novo_descoberto) atualizar_lista_dropdown_ble();
            }

            for (int s = 0; s < MAX_SLOTS; s++) {
                if (slots[s].ativo && strcmp(slots[s].mac, mac_str) == 0) {
                    if (!isnan(temp)) slots[s].temp = temp;
                    if (!isnan(hum)) slots[s].hum = hum;
                    if (!isnan(bat)) slots[s].bat = bat;
                    atualizar_cards_dashboard();
                }
            }

            bsp_display_unlock();
        }
        pos += field_len + 1;
    }
}


// --- EVENTOS NIMBLE ---
static int ble_gap_event(struct ble_gap_event *event, void *arg) {
    if (event->type == BLE_GAP_EVENT_DISC) {
        processar_pacote_ble(event->disc.addr.val, event->disc.data, event->disc.length_data);
    }
    return 0;
}

static void ble_on_sync(void) {
    struct ble_gap_disc_params disc_params = {0};
    disc_params.passive = 1;
    disc_params.itvl = 0x0060;
    disc_params.window = 0x0030;
    ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &disc_params, ble_gap_event, NULL);
}

static void ble_host_task(void *param) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// --- CALLBACKS DOS BOTÕES E TECLADO LVGL ---
static void ir_para_hist(lv_event_t *e) { lv_scr_load(pag_hist); }
static void ir_para_conf(lv_event_t *e) {
    atualizar_lista_dropdown_ble();
    atualizar_dropdown_divisoes();
    lv_obj_add_flag(teclado, LV_OBJ_FLAG_HIDDEN);
    lv_scr_load(pag_conf);
}
static void ir_para_dash(lv_event_t *e) {
    lv_obj_add_flag(teclado, LV_OBJ_FLAG_HIDDEN);
    atualizar_cards_dashboard();
    lv_scr_load(pag_dash);
}

static void dd_slot_alterado_cb(lv_event_t *e) {
    int sel = lv_dropdown_get_selected(dd_slot);
    if (sel >= 0 && sel < MAX_SLOTS) {
        lv_textarea_set_text(ta_nome_div, slots[sel].nome);
    }
}

static void ta_focado_cb(lv_event_t *e) {
    lv_keyboard_set_textarea(teclado, ta_nome_div);
    lv_obj_clear_flag(teclado, LV_OBJ_FLAG_HIDDEN);
}

static void btn_guardar_nome_cb(lv_event_t *e) {
    lv_obj_add_flag(teclado, LV_OBJ_FLAG_HIDDEN);
int slot_idx = lv_dropdown_get_selected(dd_slot);
const char *txt = lv_textarea_get_text(ta_nome_div);
if (strlen(txt) > 0 && slot_idx >= 0 && slot_idx < MAX_SLOTS) {
strncpy(slots[slot_idx].nome, txt, sizeof(slots[slot_idx].nome) - 1);
slots[slot_idx].nome[sizeof(slots[slot_idx].nome) - 1] = '\0';
guardar_nome_nvs(slot_idx, slots[slot_idx].nome);
atualizar_dropdown_divisoes();
atualizar_cards_dashboard();
char msg[80];
snprintf(msg, sizeof(msg), "Nome da Divisao %d guardado: %s", slot_idx + 1, slots[slot_idx].nome);
lv_label_set_text(lbl_status_conf, msg);
}
}
static void teclado_evento_cb(lv_event_t *e) {
lv_event_code_t code = lv_event_get_code(e);
if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
lv_obj_add_flag(teclado, LV_OBJ_FLAG_HIDDEN);
if (code == LV_EVENT_READY) {
btn_guardar_nome_cb(NULL);
}
}
}
static void btn_emparelhar_cb(lv_event_t *e) {
lv_obj_add_flag(teclado, LV_OBJ_FLAG_HIDDEN);
char sel_str[64];
lv_dropdown_get_selected_str(dd_descobertos, sel_str, sizeof(sel_str));
if (strncmp(sel_str, "A procurar", 10) == 0) {
lv_label_set_text(lbl_status_conf, "Nenhum sensor detetado ainda. Prima o botao do Shelly!");
return;
}
char mac_escolhido[18];
strncpy(mac_escolhido, sel_str, 17);
mac_escolhido[17] = '\0';
int slot_idx = lv_dropdown_get_selected(dd_slot);
if (slot_idx < 0 || slot_idx >= MAX_SLOTS) slot_idx = 0;
strcpy(slots[slot_idx].mac, mac_escolhido);
slots[slot_idx].ativo = true;
slots[slot_idx].temp = NAN;
slots[slot_idx].hum = NAN;
slots[slot_idx].bat = NAN;
for (int d = 0; d < MAX_DESCOBERTOS; d++) {
if (descobertos[d].valido && strcmp(descobertos[d].mac, mac_escolhido) == 0) {
slots[slot_idx].temp = descobertos[d].ultima_temp;
}
}
guardar_mac_nvs(slot_idx, mac_escolhido);
atualizar_cards_dashboard();
char msg[80];
snprintf(msg, sizeof(msg), "Emparelhado %s -> %s!", mac_escolhido, slots[slot_idx].nome);
lv_label_set_text(lbl_status_conf, msg);
}
static void btn_desemparelhar_cb(lv_event_t *e) {
limpar_emparelhamentos_nvs();
atualizar_cards_dashboard();
lv_label_set_text(lbl_status_conf, "Todos os sensores foram desemparelhados.");
}
static void limpar_grafico_cb(lv_event_t *e) {
for (int i = 0; i < MAX_SLOTS; i++) {
#if LVGL_VERSION_MAJOR >= 9
lv_chart_set_all_values(grafico_temp, series[i], LV_CHART_POINT_NONE);
#else
lv_chart_set_all_value(grafico_temp, series[i], LV_CHART_POINT_NONE);
#endif
}
lv_label_set_text(lbl_status_conf, "Historico limpo com sucesso.");
}
// --- CRIAÇÃO DAS 3 PÁGINAS ---
static void criar_interface(void) {
bsp_display_lock(0);
// 1. PÁGINA DASHBOARD
pag_dash = lv_obj_create(NULL);
lv_obj_set_style_bg_color(pag_dash, lv_color_hex(0x0F172A), 0);
lv_obj_t *titulo = lv_label_create(pag_dash);
lv_label_set_text(titulo, "GATEWAY SHELLY H&T (BLE)");
lv_obj_set_style_text_color(titulo, lv_color_hex(0x38BDF8), 0);
lv_obj_set_pos(titulo, 20, 18);
lv_obj_t *btn_h = lv_btn_create(pag_dash);
lv_obj_set_size(btn_h, 120, 40);
lv_obj_set_pos(btn_h, 520, 10);
lv_obj_add_event_cb(btn_h, ir_para_hist, LV_EVENT_CLICKED, NULL);
lv_label_set_text(lv_label_create(btn_h), "Historico");
lv_obj_t *btn_c = lv_btn_create(pag_dash);
lv_obj_set_size(btn_c, 130, 40);
lv_obj_set_pos(btn_c, 650, 10);
lv_obj_add_event_cb(btn_c, ir_para_conf, LV_EVENT_CLICKED, NULL);
lv_label_set_text(lv_label_create(btn_c), "Configurar");
for (int i = 0; i < MAX_SLOTS; i++) {
int col = i % 2;
int row = i / 2;
lv_obj_t *card = lv_obj_create(pag_dash);
lv_obj_set_size(card, 370, 185);
lv_obj_set_pos(card, 20 + col * 390, 68 + row * 200);
lv_obj_set_style_bg_color(card, lv_color_hex(0x1E293B), 0);
lv_obj_set_style_border_color(card, lv_color_hex(CORES_SLOTS[i]), 0);
lv_obj_set_style_border_width(card, 2, 0);
lbl_nome_card[i] = lv_label_create(card);
lv_obj_set_style_text_color(lbl_nome_card[i], lv_color_hex(0xF8FAFC), 0);
lv_obj_set_pos(lbl_nome_card[i], 5, 0);
lbl_mac[i] = lv_label_create(card);
lv_obj_set_style_text_color(lbl_mac[i], lv_color_hex(0x64748B), 0);
lv_obj_set_pos(lbl_mac[i], 155, 0);
lbl_temp[i] = lv_label_create(card);
lv_obj_set_style_text_color(lbl_temp[i], lv_color_hex(CORES_SLOTS[i]), 0);
lv_obj_set_pos(lbl_temp[i], 5, 45);
lbl_hum[i] = lv_label_create(card);
lv_obj_set_style_text_color(lbl_hum[i], lv_color_hex(0x2DD4BF), 0);
lv_obj_set_pos(lbl_hum[i], 180, 45);
lbl_bat[i] = lv_label_create(card);
lv_obj_set_style_text_color(lbl_bat[i], lv_color_hex(0x94A3B8), 0);
lv_obj_set_pos(lbl_bat[i], 5, 115);
}
// 2. PÁGINA HISTÓRICO
pag_hist = lv_obj_create(NULL);
lv_obj_set_style_bg_color(pag_hist, lv_color_hex(0x0F172A), 0);
lv_obj_t *tit_h = lv_label_create(pag_hist);
lv_label_set_text(tit_h, "HISTORICO 24H (Azul | Verde | Amarelo | Rosa)");
lv_obj_set_style_text_color(tit_h, lv_color_hex(0x38BDF8), 0);
lv_obj_set_pos(tit_h, 20, 18);
lv_obj_t *btn_v1 = lv_btn_create(pag_hist);
lv_obj_set_size(btn_v1, 125, 40);
lv_obj_set_pos(btn_v1, 655, 10);
lv_obj_add_event_cb(btn_v1, ir_para_dash, LV_EVENT_CLICKED, NULL);
lv_label_set_text(lv_label_create(btn_v1), "Voltar");
grafico_temp = lv_chart_create(pag_hist);
lv_obj_set_size(grafico_temp, 760, 385);
lv_obj_set_pos(grafico_temp, 20, 70);
lv_chart_set_type(grafico_temp, LV_CHART_TYPE_LINE);
lv_chart_set_point_count(grafico_temp, 96);
#if LVGL_VERSION_MAJOR >= 9
lv_chart_set_axis_range(grafico_temp, LV_CHART_AXIS_PRIMARY_Y, 0, 45);
#else
lv_chart_set_range(grafico_temp, LV_CHART_AXIS_PRIMARY_Y, 0, 45);
#endif
for (int i = 0; i < MAX_SLOTS; i++) {
series[i] = lv_chart_add_series(grafico_temp, lv_color_hex(CORES_SLOTS[i]), LV_CHART_AXIS_PRIMARY_Y);
}
// 3. PÁGINA CONFIGURAÇÃO (EMPARELHAMENTO + EDIÇÃO DE NOMES)
pag_conf = lv_obj_create(NULL);
lv_obj_set_style_bg_color(pag_conf, lv_color_hex(0x0F172A), 0);
lv_obj_t *tit_c = lv_label_create(pag_conf);
lv_label_set_text(tit_c, "CONFIGURACAO DE SENSORES E DIVISOES");
lv_obj_set_style_text_color(tit_c, lv_color_hex(0x38BDF8), 0);
lv_obj_set_pos(tit_c, 20, 18);
lv_obj_t *btn_v2 = lv_btn_create(pag_conf);
lv_obj_set_size(btn_v2, 125, 40);
lv_obj_set_pos(btn_v2, 655, 10);
lv_obj_add_event_cb(btn_v2, ir_para_dash, LV_EVENT_CLICKED, NULL);
lv_label_set_text(lv_label_create(btn_v2), "Voltar");
lv_obj_t *painel = lv_obj_create(pag_conf);
lv_obj_set_size(painel, 760, 390);
lv_obj_set_pos(painel, 20, 65);
lv_obj_set_style_bg_color(painel, lv_color_hex(0x1E293B), 0);
// Secção 1: Escolher Divisão e Editar Nome
lv_obj_t *l_div = lv_label_create(painel);
lv_label_set_text(l_div, "1. Divisao:");
lv_obj_set_pos(l_div, 10, 10);
dd_slot = lv_dropdown_create(painel);
lv_obj_set_size(dd_slot, 220, 42);
lv_obj_set_pos(dd_slot, 10, 35);
lv_obj_add_event_cb(dd_slot, dd_slot_alterado_cb, LV_EVENT_VALUE_CHANGED, NULL);
lv_obj_t *l_nome = lv_label_create(painel);
lv_label_set_text(l_nome, "Nome Personalizado (Toque para editar):");
lv_obj_set_pos(l_nome, 250, 10);
ta_nome_div = lv_textarea_create(painel);
lv_obj_set_size(ta_nome_div, 280, 42);
lv_obj_set_pos(ta_nome_div, 250, 35);
lv_textarea_set_one_line(ta_nome_div, true);
lv_textarea_set_max_length(ta_nome_div, 20);
lv_obj_add_event_cb(ta_nome_div, ta_focado_cb, LV_EVENT_CLICKED, NULL);
lv_obj_t *btn_save_nome = lv_btn_create(painel);
lv_obj_set_size(btn_save_nome, 180, 42);
lv_obj_set_pos(btn_save_nome, 545, 35);
lv_obj_set_style_bg_color(btn_save_nome, lv_color_hex(0x0D9488), 0);
lv_obj_add_event_cb(btn_save_nome, btn_guardar_nome_cb, LV_EVENT_CLICKED, NULL);
lv_label_set_text(lv_label_create(btn_save_nome), "Guardar Nome");
// Secção 2: Emparelhar Shelly BLE com a Divisão selecionada acima
lv_obj_t *l_ble = lv_label_create(painel);
lv_label_set_text(l_ble, "2. Emparelhar Shelly H&T detetado com a divisao acima:");
lv_obj_set_pos(l_ble, 10, 95);
dd_descobertos = lv_dropdown_create(painel);
lv_obj_set_size(dd_descobertos, 420, 42);
lv_obj_set_pos(dd_descobertos, 10, 122);
lv_dropdown_set_options(dd_descobertos, "A procurar Shelly BLE...");
lv_obj_t *btn_pair = lv_btn_create(painel);
lv_obj_set_size(btn_pair, 280, 42);
lv_obj_set_pos(btn_pair, 445, 122);
lv_obj_set_style_bg_color(btn_pair, lv_color_hex(0x0284C7), 0);
lv_obj_add_event_cb(btn_pair, btn_emparelhar_cb, LV_EVENT_CLICKED, NULL);
lv_label_set_text(lv_label_create(btn_pair), "Emparelhar Sensor");
// Barra de estado
lbl_status_conf = lv_label_create(painel);
lv_label_set_text(lbl_status_conf, "Dica: Prima o botao atras do Shelly para o detetar rapidamente.");
lv_obj_set_style_text_color(lbl_status_conf, lv_color_hex(0x38BDF8), 0);
lv_obj_set_pos(lbl_status_conf, 10, 185);
// Botões de Manutenção
lv_obj_t *btn_unpair = lv_btn_create(painel);
lv_obj_set_size(btn_unpair, 260, 45);
lv_obj_set_pos(btn_unpair, 10, 300);
lv_obj_set_style_bg_color(btn_unpair, lv_color_hex(0xD97706), 0);
lv_obj_add_event_cb(btn_unpair, btn_desemparelhar_cb, LV_EVENT_CLICKED, NULL);
lv_label_set_text(lv_label_create(btn_unpair), "Desemparelhar Todos");
lv_obj_t *btn_limpar = lv_btn_create(painel);
lv_obj_set_size(btn_limpar, 250, 45);
lv_obj_set_pos(btn_limpar, 290, 300);
lv_obj_set_style_bg_color(btn_limpar, lv_color_hex(0xDC2626), 0);
lv_obj_add_event_cb(btn_limpar, limpar_grafico_cb, LV_EVENT_CLICKED, NULL);
lv_label_set_text(lv_label_create(btn_limpar), "Limpar Historico");
// Teclado Tátil Virtual
teclado = lv_keyboard_create(pag_conf);
lv_obj_set_size(teclado, 800, 230);
lv_obj_align(teclado, LV_ALIGN_BOTTOM_MID, 0, 0);
lv_obj_add_flag(teclado, LV_OBJ_FLAG_HIDDEN);
lv_obj_add_event_cb(teclado, teclado_evento_cb, LV_EVENT_ALL, NULL);
atualizar_dropdown_divisoes();
atualizar_cards_dashboard();
lv_scr_load(pag_dash);
bsp_display_unlock();
}
// --- TAREFA DE REGISTO PERIÓDICO NO GRÁFICO (15 MIN) ---
static void tarefa_historico(void *param) {
while (1) {
vTaskDelay(pdMS_TO_TICKS(15 * 60 * 1000));
bsp_display_lock(0);
for (int i = 0; i < MAX_SLOTS; i++) {
if (slots[i].ativo && !isnan(slots[i].temp)) {
lv_chart_set_next_value(grafico_temp, series[i], (int32_t)slots[i].temp);
}
}
bsp_display_unlock();
}
}
void app_main(void) {
nvs_flash_init();
for (int i = 0; i < MAX_SLOTS; i++) {
slots[i].temp = NAN;
slots[i].hum = NAN;
slots[i].bat = NAN;
}
carregar_config_nvs();
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
nimble_port_init();
ble_hs_cfg.sync_cb = ble_on_sync;
nimble_port_freertos_init(ble_host_task);
xTaskCreate(tarefa_historico, "hist_task", 4096, NULL, 5, NULL);
}
