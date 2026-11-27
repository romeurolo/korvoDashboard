#include <stdio.h>
#include <string.h>
#include <math.h>
#include "bsp/esp-bsp.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "app_state.h"

static void processar_pacote_ble(const uint8_t *addr, const uint8_t *data, uint8_t len) {
    size_t pos = 0;
    while (pos + 2 < len) {
        uint8_t field_len = data[pos];
        if (field_len == 0 || pos + field_len >= len) break;

        if (data[pos + IDX_1] == 0x16 && field_len >= 5 && data[pos + IDX_2] == 0xD2 && data[pos + IDX_3] == 0xFC) {
            const uint8_t *bt = &data[pos + IDX_4];
            size_t bt_len = field_len - 3;
            if (bt_len < 2 || (bt[IDX_0] & 0x01)) return;

            float temp = NAN, hum = NAN, bat = NAN;
            uint8_t pkt_id = 0;
            bool has_pkt = false, btn_ev = false;

            size_t i = 1;
            while (i < bt_len) {
                uint8_t id = bt[i++];
                if (id == 0x00 && i < bt_len) {
                    pkt_id = bt[i++];
                    has_pkt = true;
                } else if (id == 0x01 && i < bt_len) {
                    bat = (float)bt[i++];
                } else if (id == 0x02 && i + 1 < bt_len) {
                    temp = ((int16_t)(bt[i] | (bt[i + IDX_1] << 8))) * 0.01f;
                    i += 2;
                } else if (id == 0x45 && i + 1 < bt_len) {
                    temp = ((int16_t)(bt[i] | (bt[i + IDX_1] << 8))) * 0.1f;
                    i += 2;
                } else if (id == 0x03 && i + 1 < bt_len) {
                    hum = ((uint16_t)(bt[i] | (bt[i + IDX_1] << 8))) * 0.01f;
                    i += 2;
                } else if (id == 0x2E && i < bt_len) {
                    hum = (float)bt[i++];
                } else if (id == 0x3A && i < bt_len) {
                    if (bt[i++] > 0) btn_ev = true;
                } else {
                    break;
                }
            }

            char mac_str[TAM_MAC];
            snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                     addr[IDX_5], addr[IDX_4], addr[IDX_3], addr[IDX_2], addr[IDX_1], addr[IDX_0]);

            bsp_display_lock(0);

            bool novo = false;
            int idx_d = -1;
            for (int d = 0; d < MAX_DESCOBERTOS; d++) {
                if (descobertos[d].valido && strcmp(descobertos[d].mac, mac_str) == 0) {
                    idx_d = d;
                    break;
                }
                if (!descobertos[d].valido && idx_d == -1) idx_d = d;
            }
            if (idx_d != -1) {
                if (!descobertos[idx_d].valido) novo = true;
                strcpy(descobertos[idx_d].mac, mac_str);
                if (!isnan(temp)) descobertos[idx_d].ultima_temp = temp;
                descobertos[idx_d].valido = true;
                if (novo) ui_atualizar_lista_dropdown_ble();
            }

            for (int s = 0; s < MAX_SLOTS; s++) {
                if (slots[s].ativo && strcmp(slots[s].mac, mac_str) == 0) {
                    if (has_pkt && slots[s].tem_pkt_id && slots[s].ultimo_pkt_id == pkt_id) continue;
                    if (has_pkt) {
                        slots[s].ultimo_pkt_id = pkt_id;
                        slots[s].tem_pkt_id = true;
                    }

                    piscar_led_req = true;

                    if (!isnan(temp)) slots[s].temp = temp;
                    if (!isnan(hum)) slots[s].hum = hum;
                    if (!isnan(bat)) slots[s].bat = bat;
                    if (btn_ev) slots[s].ciclos_piscar = 8;

                    ui_atualizar_cards_dashboard();
                }
            }

            bsp_display_unlock();
        }
        pos += field_len + 1;
    }
}

static int ble_gap_event(struct ble_gap_event *event, void *arg) {
    if (event->type == BLE_GAP_EVENT_DISC) {
        processar_pacote_ble(event->disc.addr.val, event->disc.data, event->disc.length_data);
    }
    return 0;
}

static void ble_on_sync(void) {
    struct ble_gap_disc_params dp = { .passive = 1, .itvl = 0x0060, .window = 0x0030 };
    ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &dp, ble_gap_event, NULL);
}

static void ble_host_task(void *param) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_shelly_iniciar(void) {
    nimble_port_init();
    ble_hs_cfg.sync_cb = ble_on_sync;
    nimble_port_freertos_init(ble_host_task);
}
