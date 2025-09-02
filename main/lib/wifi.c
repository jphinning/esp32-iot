#include "wifi.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"


#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include <lwip/inet.h>
#include <lwip/sockets.h>
#include <string.h> 

/* ----- USER CONFIG: ----------- ----- */
// #define WIFI_SSID "Galaxy S2002c7"
// #define WIFI_PASS "hfmp1104"
#define WIFI_SSID "VIVOFIBRA-55C0"
#define WIFI_PASS "6FCBCC76C2"
#define WIFI_MAXIMUM_RETRY 5 
/* ------------------------------------ */

static const char *TAG = "wifi.c";


static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0 // Bit for successful connection and IP
#define WIFI_FAIL_BIT      BIT1 // Bit for failure after max retries
/* ----------------------------------------------------- */

static int s_retry_num = 0;

/* ---------- WiFi event handlers (Modified) ---------- */
static void on_wifi_event(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        // MODIFIED: Retry a limited number of times
        if (s_retry_num < WIFI_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retrying to connect to the AP...");
        } else {
            // After max retries, set the failure bit
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGW(TAG, "Failed to connect to the AP");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t*) event_data;
        char ipstr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &event->ip_info.ip, ipstr, sizeof(ipstr));
        ESP_LOGI(TAG, "Got IP: %s", ipstr);
        
        // MODIFIED: Connection successful, reset retry counter and set success bit
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// This function is now fully synchronous
static void wifi_init_sta(void)
{
    // NEW: Create the event group
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Register handlers for WiFi and IP events */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi_event, NULL, NULL));

    wifi_config_t wifi_config = { 0 }; // Use a designated initializer
    strncpy((char*)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strncpy((char*)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password));
    // Ensure null termination
    wifi_config.sta.ssid[sizeof(wifi_config.sta.ssid)-1] = '\0';
    wifi_config.sta.password[sizeof(wifi_config.sta.password)-1] = '\0';


    ESP_LOGI(TAG, "Setting WiFi SSID %s...", WIFI_SSID);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished. Waiting for connection...");

    /* --- NEW: Wait here until the connection is established or fails --- */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE, // Don't clear bits on exit
            pdFALSE, // Wait for ANY bit
            portMAX_DELAY); // Wait forever

    /* --- NEW: Check the result --- */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Successfully connected to SSID:%s", WIFI_SSID);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGW(TAG, "Failed to connect to SSID:%s", WIFI_SSID);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
}

// This is now your main, synchronous entry point for Wi-Fi
void connect_to_wifi(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "Initializing WiFi...");
    wifi_init_sta();
}