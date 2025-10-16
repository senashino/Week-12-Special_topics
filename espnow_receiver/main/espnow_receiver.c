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

static const char* TAG = "ESP_NOW_RECEIVER";

// ✅ MAC ผู้ส่งที่อนุญาต
static const uint8_t SENDER_MAC[6] = {0x9C, 0x9C, 0x1F, 0xD6, 0x8B, 0x34};

// โครงสร้างข้อมูลที่รับ (ต้องตรงกับ Sender)
typedef struct {
    char  message[200];
    int   counter;
    float sensor_value;
} esp_now_data_t;

// === Callback แบบใหม่ใน ESP-IDF v5.x ===
static void on_data_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    const uint8_t *src = info->src_addr; // MAC ผู้ส่ง (6 ไบต์)

    // ✅ กรองเฉพาะแพ็กเก็ตจาก SENDER_MAC
    if (memcmp(src, SENDER_MAC, 6) != 0) {
        ESP_LOGW(TAG, "Drop frame from %02X:%02X:%02X:%02X:%02X:%02X (not whitelisted)",
                 src[0], src[1], src[2], src[3], src[4], src[5]);
        return;
    }

    int ch   = -1;
    int rssi = 0;
    if (info->rx_ctrl) {                  // rx_ctrl เป็น pointer ใน v5.x
        ch   = info->rx_ctrl->channel;
        rssi = (int)info->rx_ctrl->rssi;
    }

    ESP_LOGI(TAG, "📥 From %02X:%02X:%02X:%02X:%02X:%02X len=%d ch=%d rssi=%d",
             src[0], src[1], src[2], src[3], src[4], src[5], len, ch, rssi);

    if (len < (int)sizeof(esp_now_data_t)) {
        ESP_LOGW(TAG, "Payload too short: %d < %zu", len, sizeof(esp_now_data_t));
        return;
    }

    esp_now_data_t pkt;
    memcpy(&pkt, data, sizeof(pkt));
    pkt.message[sizeof(pkt.message) - 1] = '\0'; // กันกรณีไม่มี '\0'

    ESP_LOGI(TAG, "📨 Message: %s", pkt.message);
    ESP_LOGI(TAG, "🔢 Counter: %d", pkt.counter);
    ESP_LOGI(TAG, "🌡️  Sensor Value: %.2f", pkt.sensor_value);
    ESP_LOGI(TAG, "--------------------------------");
}

static void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();              // ✅ จำเป็นใน v5.x

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));   // ปิดประหยัดพลังงานเพื่อความเสถียร
    ESP_ERROR_CHECK(esp_wifi_start());

    // 📌 ถ้ารู้ว่าผู้ส่งอยู่ช่องไหน ให้ตั้ง channel ให้ตรงกัน เช่น ch1:
    // ESP_ERROR_CHECK(esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE));

    ESP_LOGI(TAG, "WiFi initialized");
}

static void espnow_init(void)
{
    ESP_ERROR_CHECK(esp_now_init());

    // ✅ เพิ่ม peer สำหรับ MAC ที่อนุญาต (ไม่เข้ารหัส)
    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, SENDER_MAC, 6);
    peer.ifidx   = WIFI_IF_STA;   // ใช้ STA interface
    peer.channel = 0;             // 0 = ตาม channel ปัจจุบันของ Wi-Fi
    peer.encrypt = false;         // ถ้าจะเข้ารหัส ปรับเป็น true และตั้ง PMK/LMK

    esp_err_t perr = esp_now_add_peer(&peer);
    if (perr == ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGI(TAG, "Peer already exists");
    } else {
        ESP_ERROR_CHECK(perr);
        ESP_LOGI(TAG, "Peer added: %02X:%02X:%02X:%02X:%02X:%02X",
                 SENDER_MAC[0], SENDER_MAC[1], SENDER_MAC[2],
                 SENDER_MAC[3], SENDER_MAC[4], SENDER_MAC[5]);
    }

    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_data_recv)); // ใช้ callback แบบใหม่
    ESP_LOGI(TAG, "ESP-NOW initialized and ready to receive");
}

static void print_mac_address(void)
{
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, mac));
    ESP_LOGI(TAG, "📍 My MAC Address: %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "⚠️  Copy this MAC to Sender code!");
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    wifi_init();
    print_mac_address();
    espnow_init();

    ESP_LOGI(TAG, "🎯 ESP-NOW Receiver started - Waiting for data...");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
