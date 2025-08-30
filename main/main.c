// main.c
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_http_server.h"
#include "driver/gpio.h"
#include "lwip/inet.h"
#include <lwip/sockets.h>

static const char *TAG = "simple_http_led";

/* ----- USER CONFIG: change these ----- */
#define WIFI_SSID "Jph"
#define WIFI_PASS "rntdmf83"
/* ------------------------------------ */

#define LED_GPIO GPIO_NUM_2

/* Default blink times (ms) */
static uint32_t time_on_ms  = 500;
static uint32_t time_off_ms = 500;

static uint32_t counter = 0;

/* Protect access to time_on_ms / time_off_ms */
static SemaphoreHandle_t time_mutex;

/* Forward declarations */
static esp_err_t index_get_handler(httpd_req_t *req);
static esp_err_t set_time_post_handler(httpd_req_t *req);
static esp_err_t gauge_counter_get_handler(httpd_req_t *req);
static esp_err_t counter_get_handler(httpd_req_t *req);
static void led_task(void *arg);
static void wifi_init_sta(void);

/* HTML form for GET "/" */
static const char index_html[] =
  "<!doctype html>"
  "<html><head><meta charset='utf-8' http-equiv='refresh' content='2'><title>LED timer</title></head>"
  "<body>"
  "<h2>LED Timer</h2>"
  "<form method='POST' action='/set-time'>"
  "time_on (ms): <input name='time_on' type='number' min='0' value='%u'><br>"
  "time_off (ms): <input name='time_off' type='number' min='0' value='%u'><br>"
  "<h2>Contator ESP32</h2>"
  "<p>Contador: %u</p>" 
  "<input type='submit' value='Set'>"
  "</form>"
  "</body></html>";


static const char *HTML_GAUGE =
    "<!DOCTYPE html>"
    "<html>"
    "<head>"
    "<meta charset=\"UTF-8\">"
    "<title>Contador ESP32</title>"
    "<script src=\"https://cdn.jsdelivr.net/npm/chart.js\"></script>"
    "<style>"
    "body { font-family: Arial; text-align: center; margin-top: 50px; background: #f8f8f8; }"
    "canvas { max-width: 300px; margin: auto; }"
    "h1 { color: #333; }"
    "</style>"
    "</head>"
    "<body>"
    "<h1>Contador em Tempo Real</h1>"
    "<canvas id=\"gauge\"></canvas>"
    "<p>Valor atual: <span id=\"valor\">0</span></p>"
    "<script>"
    "const ctx = document.getElementById('gauge').getContext('2d');"
    "const gauge = new Chart(ctx, {"
    "  type: 'doughnut',"
    "  data: {"
    "    labels: ['Contador', 'Restante'],"
    "    datasets: [{"
    "      data: [0, 100],"
    "      backgroundColor: ['#00aaff', '#e0e0e0'],"
    "      borderWidth: 0"
    "    }]"
    "  },"
    "  options: {"
    "    rotation: -90,"
    "    circumference: 180,"
    "    plugins: { legend: { display: false } },"
    "    cutout: '75%'"
    "  }"
    "});"
    "async function atualizarGauge() {"
    "  const res = await fetch('/counter');"
    "  const valor = await res.text();"
    "  const v = parseInt(valor);"
    "  gauge.data.datasets[0].data = [v, 100 - v];"
    "  document.getElementById('valor').innerText = v;"
    "  gauge.update();"
    "}"
    "setInterval(atualizarGauge, 2000);"
    "atualizarGauge();"
    "</script>"
    "</body>"
    "</html>";

/* Utility: get current times (thread-safe) */
static void get_times(uint32_t *on, uint32_t *off, uint32_t *ctr)
{
    if (!time_mutex) { *on = time_on_ms; *off = time_off_ms; *ctr = counter; return; }
    xSemaphoreTake(time_mutex, portMAX_DELAY);
    *on = time_on_ms;
    *off = time_off_ms;
    *ctr = counter;
    xSemaphoreGive(time_mutex);
}

/* Utility: set times (thread-safe) */
static void set_times(uint32_t on, uint32_t off)
{
    if (!time_mutex) { time_on_ms = on; time_off_ms = off; return; }
    xSemaphoreTake(time_mutex, portMAX_DELAY);
    time_on_ms = on;
    time_off_ms = off;
    xSemaphoreGive(time_mutex);
}

static void increment_counter(void)
{
    xSemaphoreTake(time_mutex, portMAX_DELAY);
    if(counter == 100){
        counter = 0;
    }
    else{   
        counter++;
    }
    xSemaphoreGive(time_mutex);
}


/* ---------- HTTP handlers ---------- */

static esp_err_t index_get_handler(httpd_req_t *req)
{
    uint32_t on, off, ctr;
    get_times(&on, &off, &ctr);

    /* Allocate a small buffer, fill with the form HTML with current values. */
    char *page;
    int needed = snprintf(NULL, 0, index_html, (unsigned)on, (unsigned)off, (unsigned)ctr) + 1;
    page = malloc(needed);
    if (!page) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory error");
        return ESP_FAIL;
    }
    snprintf(page, needed, index_html, (unsigned)on, (unsigned)off, (unsigned)ctr);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, page);
    free(page);
    return ESP_OK;
}

/* Very small urlencoded parser for numbers (we only expect digits).
   Accepts bodies like: "time_on=500&time_off=1000"
*/
static void parse_and_apply_times(char *body, size_t len, bool *applied)
{
    *applied = false;
    if (!body) return;

    // Work on a nul-terminated copy to use strtok
    body[len] = '\0';

    char *saveptr = NULL;
    char *pair = strtok_r(body, "&", &saveptr);
    uint32_t new_on = time_on_ms;
    uint32_t new_off = time_off_ms;
    bool seen_on = false, seen_off = false;

    while (pair) {
        char *eq = strchr(pair, '=');
        if (eq) {
            *eq = '\0';
            char *key = pair;
            char *val = eq + 1;
            if (strcmp(key, "time_on") == 0) {
                long parsed = strtol(val, NULL, 10);
                if (parsed >= 0) { new_on = (uint32_t)parsed; seen_on = true; }
            } else if (strcmp(key, "time_off") == 0) {
                long parsed = strtol(val, NULL, 10);
                if (parsed >= 0) { new_off = (uint32_t)parsed; seen_off = true; }
            }
        }
        pair = strtok_r(NULL, "&", &saveptr);
    }

    if (seen_on || seen_off) {
        set_times(new_on, new_off);
        *applied = true;
    }
}

static esp_err_t set_time_post_handler(httpd_req_t *req)
{
    int content_len = req->content_len;
    if (content_len <= 0 || content_len > 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid content length");
        return ESP_FAIL;
    }

    char *buf = malloc(content_len + 1 + 1); // +1 for safety nul, +1 extra
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory error");
        return ESP_FAIL;
    }

    int received = 0;
    while (received < content_len) {
        int ret = httpd_req_recv(req, buf + received, content_len - received);
        if (ret <= 0) {
            free(buf);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to receive");
            return ESP_FAIL;
        }
        received += ret;
    }
    buf[received] = '\0';

    bool applied = false;
    parse_and_apply_times(buf, received, &applied);
    free(buf);

    if (!applied) {
        httpd_resp_sendstr(req, "No valid time values found.");
        return ESP_OK;
    }

    /* Redirect back to "/" (simple response) */
    const char *resp = "<html><body>Updated. <a href=\"/\">Back</a></body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, resp);
    ESP_LOGI(TAG, "Updated times via HTTP POST");
    return ESP_OK;
}

static esp_err_t gauge_counter_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, HTML_GAUGE, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t counter_get_handler(httpd_req_t *req) {
    char buffer[16];
    uint32_t value;
    xSemaphoreTake(time_mutex, portMAX_DELAY);
    value = counter;
    xSemaphoreGive(time_mutex);

    snprintf(buffer, sizeof(buffer), "%u", (unsigned)value);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, buffer, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* Register URIs and start http server */
static httpd_handle_t start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG(); // uses port 80 by default
    config.stack_size = 4096; // increase a bit if needed

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return NULL;
    }

    httpd_uri_t index_uri = {
        .uri       = "/",
        .method    = HTTP_GET,
        .handler   = index_get_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &index_uri);

    httpd_uri_t set_uri = {
        .uri       = "/set-time",
        .method    = HTTP_POST,
        .handler   = set_time_post_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &set_uri);

    httpd_uri_t gauge_uri = {
        .uri       = "/gauge-counter",
        .method    = HTTP_GET,
        .handler   = gauge_counter_get_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &gauge_uri);

    httpd_uri_t counter_uri = {
        .uri       = "/counter",
        .method    = HTTP_GET,
        .handler   = counter_get_handler,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(server, &counter_uri);

    ESP_LOGI(TAG, "HTTP server started");
    return server;
}

/* ---------- LED FreeRTOS task ---------- */
static void led_task(void *arg)
{
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);

    for (;;) {
        uint32_t on_ms, off_ms, dummy;
        get_times(&on_ms, &off_ms, &dummy);

        /* Turn on */
        gpio_set_level(LED_GPIO, 1);
        if (on_ms == 0) {
            // If zero, yield once to avoid busy loop
            vTaskDelay(pdMS_TO_TICKS(1));
        } else {
            vTaskDelay(pdMS_TO_TICKS(on_ms));
        }

        /* Turn off */
        gpio_set_level(LED_GPIO, 0);
        if (off_ms == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
        } else {
            vTaskDelay(pdMS_TO_TICKS(off_ms));
        }
    }
}

static void counter_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(3000)); // a cada 3s
        increment_counter();
        uint32_t on, off, cnt;
        get_times(&on, &off, &cnt);
        ESP_LOGI(TAG, "Counter = %u", cnt);
    }
}

/* ---------- WiFi event handlers ---------- */

static void on_wifi_event(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            esp_wifi_connect();
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            ESP_LOGW(TAG, "Disconnected. Reconnecting...");
            esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t*) event_data;
        char ipstr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &event->ip_info.ip, ipstr, sizeof(ipstr));
        ESP_LOGI(TAG, "Got IP: %s", ipstr);
    }
}

static void wifi_init_sta(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Register handlers for WiFi and IP events */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &on_wifi_event,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &on_wifi_event,
                                                        NULL,
                                                        NULL));

    wifi_config_t wifi_config = { 0 };
    strncpy((char*)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid)-1);
    strncpy((char*)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password)-1);

    ESP_LOGI(TAG, "Setting WiFi SSID %s", WIFI_SSID);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

/* ---------- app_main ---------- */
void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    time_mutex = xSemaphoreCreateMutex();
    if (!time_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return;
    }

    ESP_LOGI(TAG, "Initializing WiFi...");
    wifi_init_sta();

    /* Start LED task */
    xTaskCreatePinnedToCore(counter_task, "counter_task", 2048, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(led_task, "led_task", 2048, NULL, tskIDLE_PRIORITY + 1, NULL, tskNO_AFFINITY);

    /* Start HTTP server (runs in its own task(s)) */
    start_webserver();

    /* app_main returns and FreeRTOS keeps running */
}
