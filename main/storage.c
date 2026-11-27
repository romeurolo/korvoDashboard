#include <stdio.h>
#include <string.h>
#include <math.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "app_state.h"

void storage_carregar_config(void) {
    for (int i = 0; i < MAX_SLOTS; i++) {
        snprintf(slots[i].nome, sizeof(slots[i].nome), "Divisao %d", i + 1);
    }

    nvs_handle_t h;
    if (nvs_open("shelly_cfg", NVS_READONLY, &h) == ESP_OK) {
        for (int i = 0; i < MAX_SLOTS; i++) {
            char km[TAM_KEY], kn[TAM_KEY];
            snprintf(km, sizeof(km), "mac_%d", i);
            snprintf(kn, sizeof(kn), "nome_%d", i);

            size_t lm = sizeof(slots[i].mac);
            size_t ln = sizeof(slots[i].nome);

            if (nvs_get_str(h, km, slots[i].mac, &lm) == ESP_OK && strlen(slots[i].mac) > 0) {
                slots[i].ativo = true;
            }
            nvs_get_str(h, kn, slots[i].nome, &ln);
        }
        nvs_close(h);
    }
}

void storage_guardar_str(const char *prefixo, int idx, const char *val) {
    nvs_handle_t h;
    if (nvs_open("shelly_cfg", NVS_READWRITE, &h) == ESP_OK) {
        char key[TAM_KEY];
        snprintf(key, sizeof(key), "%s_%d", prefixo, idx);
        nvs_set_str(h, key, val);
        nvs_commit(h);
        nvs_close(h);
    }
}

void storage_limpar_emparelhamentos(void) {
    nvs_handle_t h;
    if (nvs_open("shelly_cfg", NVS_READWRITE, &h) == ESP_OK) {
        for (int i = 0; i < MAX_SLOTS; i++) {
            char km[TAM_KEY];
            snprintf(km, sizeof(km), "mac_%d", i);
            nvs_erase_key(h, km);
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
