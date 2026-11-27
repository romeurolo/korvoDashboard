#ifndef APP_STATE_H
#define APP_STATE_H

#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

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

extern SlotSensor slots[MAX_SLOTS];
extern SensorDescoberto descobertos[MAX_DESCOBERTOS];
extern volatile bool piscar_led_req;

// Módulo Storage (NVS)
void storage_carregar_config(void);
void storage_guardar_str(const char *prefixo, int idx, const char *val);
void storage_limpar_emparelhamentos(void);

// Módulo Interface Gráfica (LVGL)
void ui_criar_interface(void);
void ui_atualizar_cards_dashboard(void);
void ui_atualizar_lista_dropdown_ble(void);
void ui_alternar_historico_botao(int btn_idx);
void ui_registar_ponto_historico(void);

// Módulo Bluetooth (NimBLE + BTHome)
void ble_shelly_iniciar(void);

#endif
