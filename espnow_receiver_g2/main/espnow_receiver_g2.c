#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

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

static const char* TAG = "ESP_NOW_RECEIVER";

/* ---------- Config ---------- */
#define MY_NODE_ID   "NODE_G2_001"
#define MY_GROUP_ID  2  // 1 หรือ 2 ตามกลุ่ม

// ใส่ MAC ของ Broadcaster (Master) ให้ถูกต้อง
static uint8_t broadcaster_mac[6] = {0x24, 0x6F, 0x28, 0xAA, 0xBB, 0xCC};

/* ---------- ข้อความโปรโตคอล ---------- */
typedef struct {
    char sender_id[20];
    char message[180];
    uint8_t message_type;  // 1=Info, 2=Command, 3=Alert
    uint8_t group_id;      // 0=All, 1=Group1, 2=Group2
    uint32_t sequence_num;
    uint32_t timestamp;    // ms
} broadcast_data_t;

/* ---------- กันซ้ำแบบง่าย ---------- */
static uint32_t last_sequence = 0;

/* ---------- ฟอร์เวิร์ดโปรโตไทป์ ---------- */
static void send_reply(const uint8_t* target_mac, const char* reply_message);

/* ---------- ตัวช่วย ---------- */
static const char* msg_type_to_str(uint8_t t) {
    switch (t) {
        case 1: return "INFO";
        case 2: return "COMMAND";
        case 3: return "ALERT";
        default: return "UNKNOWN";
    }
}

static bool is_for_me(uint8_t msg_group_id) {
    return (msg_group_id == 0) || (msg_group_id == MY_GROUP_ID);
}

/* ---------- ESPNOW Callbacks (ESP-IDF v5.x) ---------- */
static void on_data_recv(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len) {
    if (!recv_info || !data) return;

    if (len < (int)sizeof(broadcast_data_t)) {
        ESP_LOGW(TAG, "⚠️  Invalid payload size: %d (need >= %u)", len, (unsigned)sizeof(broadcast_data_t));
        return;
    }

    const uint8_t *mac_addr = recv_info->src_addr;

    broadcast_data_t msg;
    memcpy(&msg, data, sizeof(msg));
    msg.sender_id[sizeof(msg.sender_id)-1] = '\0';
    msg.message[sizeof(msg.message)-1]     = '\0';

    // กันซ้ำ
    if (msg.sequence_num <= last_sequence) {
        ESP_LOGW(TAG, "⚠️  Duplicate ignored (seq %lu <= %lu)",
                 (unsigned long)msg.sequence_num, (unsigned long)last_sequence);
        return;
    }
    last_sequence = msg.sequence_num;

    // ตรวจกลุ่ม
    if (!is_for_me(msg.group_id)) {
        ESP_LOGI(TAG, "📋 Message for Group %u (not for me)", msg.group_id);
        return;
    }

    // Log
    ESP_LOGI(TAG, "📥 From %02X:%02X:%02X:%02X:%02X:%02X / %s",
             mac_addr[0], mac_addr[1], mac_addr[2],
             mac_addr[3], mac_addr[4], mac_addr[5],
             msg.sender_id);
    ESP_LOGI(TAG, "   📨 %s", msg.message);
    ESP_LOGI(TAG, "   🏷️  Type: %s", msg_type_to_str(msg.message_type));
    ESP_LOGI(TAG, "   👥 Group: %u", msg.group_id);
    ESP_LOGI(TAG, "   📊 Sequence: %lu", (unsigned long)msg.sequence_num);

    // ประมวลผล
    if (msg.message_type == 2) {              // COMMAND
        ESP_LOGI(TAG, "🔧 Processing command...");
        // TODO: ใส่ logic คำสั่งที่นี่

        send_reply(mac_addr, "Command received and processed");
    } else if (msg.message_type == 3) {       // ALERT
        ESP_LOGW(TAG, "🚨 ALERT: %s", msg.message);
        // TODO: จัดการเหตุฉุกเฉิน
    }

    ESP_LOGI(TAG, "--------------------------------");
}

static void on_data_sent(const wifi_tx_info_t *tx_info, esp_now_send_status_t status) {
    (void)tx_info; // v5.5.1 ไม่มี MAC ในโครงสร้างนี้
    ESP_LOGI(TAG, "Reply sent: %s", (status == ESP_NOW_SEND_SUCCESS) ? "✅ SUCCESS" : "❌ FAIL");
}

/* ---------- ส่ง Reply ---------- */
static void send_reply(const uint8_t* target_mac, const char* reply_message) {
    broadcast_data_t r = {0};
    snprintf(r.sender_id, sizeof(r.sender_id), "%s", MY_NODE_ID);
    snprintf(r.message,   sizeof(r.message),   "%s", reply_message);
    r.message_type = 1;   // INFO
    r.group_id     = MY_GROUP_ID;
    r.sequence_num = 0;   // ไม่ track ลำดับสำหรับ reply
    r.timestamp    = (uint32_t)(esp_timer_get_time() / 1000ULL);

    ESP_LOGI(TAG, "📤 Sending reply: %s", reply_message);
    esp_err_t err = esp_now_send(target_mac, (uint8_t*)&r, sizeof(r));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_send failed: %s", esp_err_to_name(err));
    }
}

/* ---------- Init ---------- */
static void init_nvs(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(ret);
    }
}

static void init_wifi_espnow(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE)); // ลด latency
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_data_recv));
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_data_sent));

    // เพิ่ม peer ของ Broadcaster เพื่อส่ง reply กลับ
    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, broadcaster_mac, 6);
    peer.ifidx   = WIFI_IF_STA;
    peer.channel = 0;           // ตาม channel ปัจจุบัน
    peer.encrypt = false;

    esp_err_t add_res = esp_now_add_peer(&peer);
    if (add_res != ESP_OK && add_res != ESP_ERR_ESPNOW_EXIST) {
        ESP_ERROR_CHECK(add_res);
    }

    ESP_LOGI(TAG, "ESP-NOW Receiver initialized");
}

/* ---------- app_main ---------- */
void app_main(void) {
    init_nvs();
    init_wifi_espnow();

    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, mac));
    ESP_LOGI(TAG, "📍 Node ID : %s", MY_NODE_ID);
    ESP_LOGI(TAG, "📍 Group ID: %d", MY_GROUP_ID);
    ESP_LOGI(TAG, "📍 MAC     : %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    ESP_LOGI(TAG, "🎯 ESP-NOW Receiver ready - Waiting for broadcasts...");
    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
}
