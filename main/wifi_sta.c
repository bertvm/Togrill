#include "wifi_sta.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "grill.h"
#include "mqtt_link.h"
#include "settings.h"
#include "togrill_ble.h"

static const char *TAG = "wifi";
static bool s_inited;
static bool s_running;

static void wifi_events(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        grill_set_wifi(false);
        grill_set_mqtt(false);
        if (settings_get()->wifi_ssid[0] != '\0') {
            ESP_LOGW(TAG, "disconnected, retrying");
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        grill_set_wifi(true);
        ESP_LOGI(TAG, "connected");
        mqtt_link_start();
    }
}

void wifi_sta_apply(void)
{
    if (!s_inited) {
        return;
    }
    const app_settings_t *settings = settings_get();
    wifi_config_t wifi = {0};
    strncpy((char *)wifi.sta.ssid, settings->wifi_ssid, sizeof(wifi.sta.ssid) - 1);
    strncpy((char *)wifi.sta.password, settings->wifi_pass, sizeof(wifi.sta.password) - 1);
    wifi.sta.threshold.authmode = settings->wifi_pass[0] == '\0' ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi));

    if (settings->wifi_ssid[0] == '\0') {
        grill_set_detail("open Setup");
        if (s_running) {
            esp_wifi_disconnect();
        }
        return;
    }
    grill_set_detail("joining WiFi");
    if (!s_running) {
        ESP_ERROR_CHECK(esp_wifi_start());
        s_running = true;
        return;
    }
    esp_wifi_disconnect();
    esp_wifi_connect();
}

void wifi_sta_start(void)
{
    if (s_inited) {
        wifi_sta_apply();
        return;
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_events, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_events, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));
    s_inited = true;
    togrill_ble_note_wifi();
    wifi_sta_apply();
}
