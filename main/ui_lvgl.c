#include <stdio.h>
#include <string.h>
#include <math.h>
#include "bsp/esp-bsp.h"
#include "app_state.h"

static const uint32_t CORES_SLOTS[MAX_SLOTS] = {0x38BDF8, 0x2DD4BF, 0xFBBF24, 0xF472B6};
static int divisao_hist_ativa = -1;

static lv_obj_t *pag_dash, *pag_hist, *pag_conf;
static lv_obj_t *cards[MAX_SLOTS];
static lv_obj_t *lbl_nome_card[MAX_SLOTS], *lbl_temp[MAX_SLOTS], *lbl_hum[MAX_SLOTS], *lbl_bat[MAX_SLOTS], *lbl_mac[MAX_SLOTS];
static lv_obj_t *lbl_titulo_hist, *grafico_temp;
static lv_chart_series_t *series[MAX_SLOTS];
static lv_obj_t *dd_descobertos, *dd_slot, *ta_nome_div, *teclado, *lbl_status_conf;

static void timer_piscar_cb(lv_timer_t *timer) {
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (slots[i].ciclos_piscar > 0) {
            lv_obj_set_style_bg_color(cards[i], lv_color_hex((slots[i].ciclos_piscar % 2 == 0) ? 0xFFFFFF : 0x1E293B), 0);
            slots[i].ciclos_piscar--;
            if (slots[i].ciclos_piscar == 0) lv_obj_set_style_bg_color(cards[i], lv_color_hex(0x1E293B), 0);
        }
    }
}

static void atualizar_dropdown_divisoes(void) {
    int sel = lv_dropdown_get_selected(dd_slot);
    if (sel < 0 || sel >= MAX_SLOTS) sel = 0;
    char opcoes[TAM_OPCOES] = "";
    for (int i = 0; i < MAX_SLOTS; i++) {
        char item[TAM_BUF];
        snprintf(item, sizeof(item), "%d: %s", i + 1, slots[i].nome);
        if (i > 0) strcat(opcoes, "\n");
        strcat(opcoes, item);
    }
    lv_dropdown_set_options(dd_slot, opcoes);
    lv_dropdown_set_selected(dd_slot, sel);
    lv_textarea_set_text(ta_nome_div, slots[sel].nome);
}

void ui_atualizar_lista_dropdown_ble(void) {
    char opcoes[TAM_OPCOES] = "";
    int count = 0;
    for (int i = 0; i < MAX_DESCOBERTOS; i++) {
        if (descobertos[i].valido) {
            char linha[TAM_BUF];
            if (!isnan(descobertos[i].ultima_temp)) snprintf(linha, sizeof(linha), "%s (%.1f°C)", descobertos[i].mac, descobertos[i].ultima_temp);
            else snprintf(linha, sizeof(linha), "%s", descobertos[i].mac);
            if (count > 0) strcat(opcoes, "\n");
            strcat(opcoes, linha);
            count++;
        }
    }
    lv_dropdown_set_options(dd_descobertos, count == 0 ? "A procurar Shelly BLE..." : opcoes);
}

void ui_atualizar_cards_dashboard(void) {
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
            if (!isnan(slots[i].temp)) { snprintf(buf, sizeof(buf), "%.1f °C", slots[i].temp); lv_label_set_text(lbl_temp[i], buf); }
            if (!isnan(slots[i].hum)) { snprintf(buf, sizeof(buf), "%.0f %%", slots[i].hum); lv_label_set_text(lbl_hum[i], buf); }
            if (!isnan(slots[i].bat)) { snprintf(buf, sizeof(buf), "Bat: %.0f%%", slots[i].bat); lv_label_set_text(lbl_bat[i], buf); }
        }
    }
}

static void abrir_historico_divisao(int slot_idx) {
    divisao_hist_ativa = slot_idx;
    char tit[TAM_BUF];
    if (slot_idx == -1) {
        snprintf(tit, sizeof(tit), "HISTORICO 24H (Todas as Divisoes)");
        for (int i = 0; i < MAX_SLOTS; i++) lv_chart_hide_series(grafico_temp, series[i], false);
    } else {
        snprintf(tit, sizeof(tit), "HISTORICO 24H - %s", slots[slot_idx].nome);
        for (int i = 0; i < MAX_SLOTS; i++) lv_chart_hide_series(grafico_temp, series[i], (i != slot_idx));
    }
    lv_label_set_text(lbl_titulo_hist, tit);
    lv_scr_load(pag_hist);
}

void ui_alternar_historico_botao(int btn_idx) {
    if (btn_idx < 0 || btn_idx >= MAX_SLOTS) return;
    bsp_display_lock(0);
    if (lv_scr_act() == pag_hist && divisao_hist_ativa == btn_idx) {
        divisao_hist_ativa = -1;
        ui_atualizar_cards_dashboard();
        lv_scr_load(pag_dash);
    } else {
        abrir_historico_divisao(btn_idx);
    }
    bsp_display_unlock();
}

void ui_registar_ponto_historico(void) {
    bsp_display_lock(0);
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (slots[i].ativo && !isnan(slots[i].temp)) {
            lv_chart_set_next_value(grafico_temp, series[i], (int32_t)slots[i].temp);
        }
    }
    bsp_display_unlock();
}

static void ir_para_hist_todas(lv_event_t *e) { abrir_historico_divisao(-1); }
static void ir_para_conf(lv_event_t *e) { ui_atualizar_lista_dropdown_ble(); atualizar_dropdown_divisoes(); lv_obj_add_flag(teclado, LV_OBJ_FLAG_HIDDEN); lv_scr_load(pag_conf); }
static void ir_para_dash(lv_event_t *e) { divisao_hist_ativa = -1; lv_obj_add_flag(teclado, LV_OBJ_FLAG_HIDDEN); ui_atualizar_cards_dashboard(); lv_scr_load(pag_dash); }
static void dd_slot_alterado_cb(lv_event_t *e) { int sel = lv_dropdown_get_selected(dd_slot); if (sel >= 0 && sel < MAX_SLOTS) lv_textarea_set_text(ta_nome_div, slots[sel].nome); }
static void ta_focado_cb(lv_event_t *e) { lv_keyboard_set_textarea(teclado, ta_nome_div); lv_obj_clear_flag(teclado, LV_OBJ_FLAG_HIDDEN); }

static void btn_guardar_nome_cb(lv_event_t *e) {
    lv_obj_add_flag(teclado, LV_OBJ_FLAG_HIDDEN);
    int idx = lv_dropdown_get_selected(dd_slot);
    const char *txt = lv_textarea_get_text(ta_nome_div);
    if (strlen(txt) > 0 && idx >= 0 && idx < MAX_SLOTS) {
        strncpy(slots[idx].nome, txt, sizeof(slots[idx].nome) - 1);
        slots[idx].nome[sizeof(slots[idx].nome) - 1] = '\0';
        storage_guardar_str("nome", idx, slots[idx].nome);
        atualizar_dropdown_divisoes();
        ui_atualizar_cards_dashboard();
        char msg[TAM_MSG];
        snprintf(msg, sizeof(msg), "Nome guardado: %s", slots[idx].nome);
        lv_label_set_text(lbl_status_conf, msg);
    }
}

static void teclado_evento_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        lv_obj_add_flag(teclado, LV_OBJ_FLAG_HIDDEN);
        if (code == LV_EVENT_READY) btn_guardar_nome_cb(NULL);
    }
}

static void btn_emparelhar_cb(lv_event_t *e) {
    lv_obj_add_flag(teclado, LV_OBJ_FLAG_HIDDEN);
    char sel[TAM_BUF];
    lv_dropdown_get_selected_str(dd_descobertos, sel, sizeof(sel));
    if (strncmp(sel, "A procurar", 10) == 0) { lv_label_set_text(lbl_status_conf, "Prima o botao do Shelly primeiro!"); return; }
    char mac[TAM_MAC];
    strncpy(mac, sel, IDX_17); mac[IDX_17] = '\0';
    int idx = lv_dropdown_get_selected(dd_slot);
    if (idx < 0 || idx >= MAX_SLOTS) idx = 0;
    strcpy(slots[idx].mac, mac);
    slots[idx].ativo = true; slots[idx].temp = NAN; slots[idx].hum = NAN; slots[idx].bat = NAN; slots[idx].tem_pkt_id = false;
    for (int d = 0; d < MAX_DESCOBERTOS; d++) if (descobertos[d].valido && strcmp(descobertos[d].mac, mac) == 0) slots[idx].temp = descobertos[d].ultima_temp;
    storage_guardar_str("mac", idx, mac);
    ui_atualizar_cards_dashboard();
    char msg[TAM_MSG];
    snprintf(msg, sizeof(msg), "Emparelhado %s -> %s", mac, slots[idx].nome);
    lv_label_set_text(lbl_status_conf, msg);
}

static void btn_desemparelhar_cb(lv_event_t *e) { storage_limpar_emparelhamentos(); ui_atualizar_cards_dashboard(); lv_label_set_text(lbl_status_conf, "Sensores desemparelhados."); }
static void limpar_grafico_cb(lv_event_t *e) { for (int i = 0; i < MAX_SLOTS; i++) lv_chart_set_all_values(grafico_temp, series[i], LV_CHART_POINT_NONE); lv_label_set_text(lbl_status_conf, "Historico limpo."); }

void ui_criar_interface(void) {
    bsp_display_lock(0);
    pag_dash = lv_obj_create(NULL); lv_obj_set_style_bg_color(pag_dash, lv_color_hex(0x0F172A), 0);
    lv_obj_t *tit = lv_label_create(pag_dash); lv_label_set_text(tit, "GATEWAY SHELLY H&T (BLE)"); lv_obj_set_style_text_color(tit, lv_color_hex(0x38BDF8), 0); lv_obj_set_pos(tit, 20, 18);

    lv_obj_t *bh = lv_btn_create(pag_dash); lv_obj_set_size(bh, 120, 40); lv_obj_set_pos(bh, 520, 10); lv_obj_add_event_cb(bh, ir_para_hist_todas, LV_EVENT_CLICKED, NULL); lv_label_set_text(lv_label_create(bh), "Historico");
    lv_obj_t *bc = lv_btn_create(pag_dash); lv_obj_set_size(bc, 130, 40); lv_obj_set_pos(bc, 650, 10); lv_obj_add_event_cb(bc, ir_para_conf, LV_EVENT_CLICKED, NULL); lv_label_set_text(lv_label_create(bc), "Configurar");

    for (int i = 0; i < MAX_SLOTS; i++) {
        cards[i] = lv_obj_create(pag_dash);
        lv_obj_set_size(cards[i], 370, 185); lv_obj_set_pos(cards[i], 20 + (i % 2) * 390, 68 + (i / 2) * 200);
        lv_obj_set_style_bg_color(cards[i], lv_color_hex(0x1E293B), 0); lv_obj_set_style_border_color(cards[i], lv_color_hex(CORES_SLOTS[i]), 0); lv_obj_set_style_border_width(cards[i], 2, 0);
        lbl_nome_card[i] = lv_label_create(cards[i]); lv_obj_set_style_text_color(lbl_nome_card[i], lv_color_hex(0xF8FAFC), 0); lv_obj_set_pos(lbl_nome_card[i], 5, 0);
        lbl_mac[i] = lv_label_create(cards[i]); lv_obj_set_style_text_color(lbl_mac[i], lv_color_hex(0x64748B), 0); lv_obj_set_pos(lbl_mac[i], 155, 0);
        lbl_temp[i] = lv_label_create(cards[i]); lv_obj_set_style_text_color(lbl_temp[i], lv_color_hex(CORES_SLOTS[i]), 0); lv_obj_set_pos(lbl_temp[i], 5, 45);
        lbl_hum[i] = lv_label_create(cards[i]); lv_obj_set_style_text_color(lbl_hum[i], lv_color_hex(0x2DD4BF), 0); lv_obj_set_pos(lbl_hum[i], 180, 45);
        lbl_bat[i] = lv_label_create(cards[i]); lv_obj_set_style_text_color(lbl_bat[i], lv_color_hex(0x94A3B8), 0); lv_obj_set_pos(lbl_bat[i], 5, 115);
    }

    pag_hist = lv_obj_create(NULL); lv_obj_set_style_bg_color(pag_hist, lv_color_hex(0x0F172A), 0);
    lbl_titulo_hist = lv_label_create(pag_hist); lv_obj_set_style_text_color(lbl_titulo_hist, lv_color_hex(0x38BDF8), 0); lv_obj_set_pos(lbl_titulo_hist, 20, 18);
lv_obj_t *bv1 = lv_btn_create(pag_hist); lv_obj_set_size(bv1, 125, 40); lv_obj_set_pos(bv1, 655, 10); lv_obj_add_event_cb(bv1, ir_para_dash, LV_EVENT_CLICKED, NULL); lv_label_set_text(lv_label_create(bv1), "Voltar");
grafico_temp = lv_chart_create(pag_hist); lv_obj_set_size(grafico_temp, 760, 385); lv_obj_set_pos(grafico_temp, 20, 70);
lv_chart_set_type(grafico_temp, LV_CHART_TYPE_LINE); lv_chart_set_point_count(grafico_temp, 96); lv_chart_set_axis_range(grafico_temp, LV_CHART_AXIS_PRIMARY_Y, 0, 45);
for (int i = 0; i < MAX_SLOTS; i++) series[i] = lv_chart_add_series(grafico_temp, lv_color_hex(CORES_SLOTS[i]), LV_CHART_AXIS_PRIMARY_Y);
pag_conf = lv_obj_create(NULL); lv_obj_set_style_bg_color(pag_conf, lv_color_hex(0x0F172A), 0);
lv_obj_t *tc = lv_label_create(pag_conf); lv_label_set_text(tc, "CONFIGURACAO DE SENSORES E DIVISOES"); lv_obj_set_style_text_color(tc, lv_color_hex(0x38BDF8), 0); lv_obj_set_pos(tc, 20, 18);
lv_obj_t *bv2 = lv_btn_create(pag_conf); lv_obj_set_size(bv2, 125, 40); lv_obj_set_pos(bv2, 655, 10); lv_obj_add_event_cb(bv2, ir_para_dash, LV_EVENT_CLICKED, NULL); lv_label_set_text(lv_label_create(bv2), "Voltar");
lv_obj_t *p = lv_obj_create(pag_conf); lv_obj_set_size(p, 760, 390); lv_obj_set_pos(p, 20, 65); lv_obj_set_style_bg_color(p, lv_color_hex(0x1E293B), 0);
lv_obj_t *ld = lv_label_create(p); lv_label_set_text(ld, "1. Divisao:"); lv_obj_set_pos(ld, 10, 10);
dd_slot = lv_dropdown_create(p); lv_obj_set_size(dd_slot, 220, 42); lv_obj_set_pos(dd_slot, 10, 35); lv_obj_add_event_cb(dd_slot, dd_slot_alterado_cb, LV_EVENT_VALUE_CHANGED, NULL);
lv_obj_t *ln = lv_label_create(p); lv_label_set_text(ln, "Nome Personalizado:"); lv_obj_set_pos(ln, 250, 10);
ta_nome_div = lv_textarea_create(p); lv_obj_set_size(ta_nome_div, 280, 42); lv_obj_set_pos(ta_nome_div, 250, 35); lv_textarea_set_one_line(ta_nome_div, true); lv_textarea_set_max_length(ta_nome_div, 20); lv_obj_add_event_cb(ta_nome_div, ta_focado_cb, LV_EVENT_CLICKED, NULL);
lv_obj_t *bsn = lv_btn_create(p); lv_obj_set_size(bsn, 180, 42); lv_obj_set_pos(bsn, 545, 35); lv_obj_set_style_bg_color(bsn, lv_color_hex(0x0D9488), 0); lv_obj_add_event_cb(bsn, btn_guardar_nome_cb, LV_EVENT_CLICKED, NULL); lv_label_set_text(lv_label_create(bsn), "Guardar Nome");
lv_obj_t *lb = lv_label_create(p); lv_label_set_text(lb, "2. Emparelhar Shelly H&T detetado:"); lv_obj_set_pos(lb, 10, 95);
dd_descobertos = lv_dropdown_create(p); lv_obj_set_size(dd_descobertos, 420, 42); lv_obj_set_pos(dd_descobertos, 10, 122);
lv_obj_t *bp = lv_btn_create(p); lv_obj_set_size(bp, 280, 42); lv_obj_set_pos(bp, 445, 122); lv_obj_set_style_bg_color(bp, lv_color_hex(0x0284C7), 0); lv_obj_add_event_cb(bp, btn_emparelhar_cb, LV_EVENT_CLICKED, NULL); lv_label_set_text(lv_label_create(bp), "Emparelhar Sensor");
lbl_status_conf = lv_label_create(p); lv_label_set_text(lbl_status_conf, "Botoes 1-4 da placa abrem o historico de cada divisao."); lv_obj_set_style_text_color(lbl_status_conf, lv_color_hex(0x38BDF8), 0); lv_obj_set_pos(lbl_status_conf, 10, 185);
lv_obj_t *bu = lv_btn_create(p); lv_obj_set_size(bu, 260, 45); lv_obj_set_pos(bu, 10, 300); lv_obj_set_style_bg_color(bu, lv_color_hex(0xD97706), 0); lv_obj_add_event_cb(bu, btn_desemparelhar_cb, LV_EVENT_CLICKED, NULL); lv_label_set_text(lv_label_create(bu), "Desemparelhar Todos");
lv_obj_t *bl = lv_btn_create(p); lv_obj_set_size(bl, 250, 45); lv_obj_set_pos(bl, 290, 300); lv_obj_set_style_bg_color(bl, lv_color_hex(0xDC2626), 0); lv_obj_add_event_cb(bl, limpar_grafico_cb, LV_EVENT_CLICKED, NULL); lv_label_set_text(lv_label_create(bl), "Limpar Historico");
teclado = lv_keyboard_create(pag_conf); lv_obj_set_size(teclado, 800, 230); lv_obj_align(teclado, LV_ALIGN_BOTTOM_MID, 0, 0); lv_obj_add_flag(teclado, LV_OBJ_FLAG_HIDDEN); lv_obj_add_event_cb(teclado, teclado_evento_cb, LV_EVENT_ALL, NULL);
lv_timer_create(timer_piscar_cb, 250, NULL);
atualizar_dropdown_divisoes();
ui_atualizar_cards_dashboard();
lv_scr_load(pag_dash);
bsp_display_unlock();
}
