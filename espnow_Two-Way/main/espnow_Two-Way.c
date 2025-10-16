#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_now.h"
#include "esp_netif.h"
#include "esp_timer.h"

static const char* TAG = "ESP_NOW_BIDIRECTIONAL";

// MAC Address ของอีกตัว (แก้ตามที่ให้มา)
static uint8_t partner_mac[6] = {0x9C, 0x9C, 0x1F, 0xD6, 0x8B, 0x34};

// ข้อมูลที่ส่ง/รับ
typedef struct {
    char     device_name[50];
    char     message[150];
    int      counter;
    uint32_t timestamp;  // ms
} bidirectional_data_t;

/* ==== Callbacks (API v5.x) ============================================ */

// ส่งเสร็จ (v5.x): มี dest_addr ใน info
static void on_data_sent(const esp_now_send_info_t *info, esp_now_send_status_t status)
{
    const uint8_t *dst = (info && info->des_addr) ? info->des_addr : NULL;
    if (dst) {
        ESP_LOGI(TAG, "TX to %02X:%02X:%02X:%02X:%02X:%02X -> %s",
                 dst[0], dst[1], dst[2], dst[3], dst[4], dst[5],
                 status == ESP_NOW_SEND_SUCCESS ? "SUCCESS" : "FAIL");
    } else {
        ESP_LOGI(TAG, "TX status: %s (no dest_addr)",
                 status == ESP_NOW_SEND_SUCCESS ? "SUCCESS" : "FAIL");
    }
}

// รับข้อมูล (v5.x): มี src_addr และ rx_ctrl*
static void on_data_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (!info || !data || len < (int)sizeof(bidirectional_data_t)) {
        ESP_LOGW(TAG, "RX invalid: info=%p data=%p len=%d (need >= %zu)",
                 (void*)info, (void*)data, len, sizeof(bidirectional_data_t));
        return;
    }

    const uint8_t *src = info->src_addr;
    int ch = -1, rssi = 0;
    if (info->rx_ctrl) {
        ch   = info->rx_ctrl->channel;
        rssi = (int)info->rx_ctrl->rssi;
    }

    bidirectional_data_t pkt;
    memcpy(&pkt, data, sizeof(pkt));
    pkt.device_name[sizeof(pkt.device_name)-1] = '\0';
    pkt.message[sizeof(pkt.message)-1] = '\0';

    ESP_LOGI(TAG, "📥 From %02X:%02X:%02X:%02X:%02X:%02X len=%d ch=%d rssi=%d",
             src[0], src[1], src[2], src[3], src[4], src[5], len, ch, rssi);
    ESP_LOGI(TAG, "   👤 %s", pkt.device_name);
    ESP_LOGI(TAG, "   💬 %s", pkt.message);
    ESP_LOGI(TAG, "   🔢 %d", pkt.counter);
    ESP_LOGI(TAG, "   ⏰ %lu ms", (unsigned long)pkt.timestamp);

    // ตอบกลับไปยังผู้ส่งเดิม
    bidirectional_data_t reply = {0};
    strncpy(reply.device_name, "Device_B", sizeof(reply.device_name)-1);
    snprintf(reply.message, sizeof(reply.message), "Reply to #%d - Thanks!", pkt.counter);
    reply.counter   = pkt.counter;
    reply.timestamp = (uint32_t)(esp_timer_get_time() / 1000ULL);  // to ms

    vTaskDelay(pdMS_TO_TICKS(100)); // กันชนกับ log

    esp_err_t err = esp_now_send(src, (const uint8_t*)&reply, sizeof(reply));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_send(reply) failed: %s", esp_err_to_name(err));
    }
}

/* ==== Init Wi-Fi & ESPNOW ============================================ */

static void init_espnow(void)
{
    // Wi-Fi base
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));   // เพื่อความเสถียรของ ESP-NOW
    ESP_ERROR_CHECK(esp_wifi_start());

    // ถ้ารู้ channel ของอีกฝั่งให้ตั้งให้ตรงกัน เช่น ch1:
    // ESP_ERROR_CHECK(esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE));

    // ESP-NOW
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_data_sent));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_data_recv));

    // เพิ่ม peer (จำเป็นเมื่อจะส่งหาเป้าหมาย)
    esp_now_peer_info_t peer_info = {0};
    memcpy(peer_info.peer_addr, partner_mac, 6);
    peer_info.ifidx   = WIFI_IF_STA;  // สำคัญ
    peer_info.channel = 0;            // 0 = ใช้ช่องปัจจุบัน
    peer_info.encrypt = false;        // ถ้าจะเข้ารหัส ค่อยตั้ง true + ใส่ PMK/LMK

    esp_err_t perr = esp_now_add_peer(&peer_info);
    if (perr != ESP_OK && perr != ESP_ERR_ESPNOW_EXIST) {
        ESP_ERROR_CHECK(perr);
    } else {
        ESP_LOGI(TAG, "Peer %02X:%02X:%02X:%02X:%02X:%02X ready",
                 partner_mac[0], partner_mac[1], partner_mac[2],
                 partner_mac[3], partner_mac[4], partner_mac[5]);
    }

    ESP_LOGI(TAG, "ESP-NOW bidirectional communication initialized");
}

void app_main(void)
{
    // NVS init (เผื่อกรณีต้อง erase)
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    init_espnow();

    // แสดง MAC ตัวเอง
    uint8_t mac[6] = {0};
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, mac));
    ESP_LOGI(TAG, "📍 My MAC: %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // วนส่งข้อมูลหา partner ทุก 5 วินาที
    bidirectional_data_t send_data = {0};
    int counter = 0;

    while (1) {
        strncpy(send_data.device_name, "Device_A", sizeof(send_data.device_name)-1);
        snprintf(send_data.message, sizeof(send_data.message),
                 "Hello! This is message number %d", counter);
        send_data.counter   = counter++;
        send_data.timestamp = (uint32_t)(esp_timer_get_time() / 1000ULL);

        ESP_LOGI(TAG, "📤 Sending message #%d", send_data.counter);
        esp_err_t err = esp_now_send(partner_mac, (const uint8_t*)&send_data, sizeof(send_data));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_now_send() failed: %s", esp_err_to_name(err));
        }

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
