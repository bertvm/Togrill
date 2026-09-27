#include "settings.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "mqtt_link.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "wifi_sta.h"

static const char *TAG = "settings";
static app_settings_t s_live;

static void copy_default(char *dst, size_t dst_len, const char *fallback)
{
    snprintf(dst, dst_len, "%s", fallback != NULL ? fallback : "");
}

static void load_str(nvs_handle_t handle, const char *key, char *dst, size_t dst_len)
{
    size_t length = dst_len;
    if (nvs_get_str(handle, key, dst, &length) != ESP_OK) {
        return;
    }
}

void settings_init(void)
{
    memset(&s_live, 0, sizeof(s_live));
    copy_default(s_live.wifi_ssid, sizeof(s_live.wifi_ssid), CONFIG_TOGRILL_WIFI_SSID);
    copy_default(s_live.wifi_pass, sizeof(s_live.wifi_pass), CONFIG_TOGRILL_WIFI_PASSWORD);
    copy_default(s_live.mqtt_uri, sizeof(s_live.mqtt_uri), CONFIG_TOGRILL_MQTT_URI);
    copy_default(s_live.mqtt_user, sizeof(s_live.mqtt_user), CONFIG_TOGRILL_MQTT_USERNAME);
    copy_default(s_live.mqtt_pass, sizeof(s_live.mqtt_pass), CONFIG_TOGRILL_MQTT_PASSWORD);

    nvs_handle_t handle;
    if (nvs_open("togrill", NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    load_str(handle, "ssid", s_live.wifi_ssid, sizeof(s_live.wifi_ssid));
    load_str(handle, "pass", s_live.wifi_pass, sizeof(s_live.wifi_pass));
    load_str(handle, "uri", s_live.mqtt_uri, sizeof(s_live.mqtt_uri));
    load_str(handle, "user", s_live.mqtt_user, sizeof(s_live.mqtt_user));
    load_str(handle, "mpass", s_live.mqtt_pass, sizeof(s_live.mqtt_pass));
    nvs_close(handle);
    ESP_LOGI(TAG, "ssid=%s mqtt=%s", s_live.wifi_ssid, s_live.mqtt_uri);
}

const app_settings_t *settings_get(void)
{
    return &s_live;
}

static void store_str(nvs_handle_t handle, const char *key, const char *value)
{
    ESP_ERROR_CHECK(nvs_set_str(handle, key, value != NULL ? value : ""));
}

void settings_save(const app_settings_t *next)
{
    s_live = *next;
    nvs_handle_t handle;
    ESP_ERROR_CHECK(nvs_open("togrill", NVS_READWRITE, &handle));
    store_str(handle, "ssid", s_live.wifi_ssid);
    store_str(handle, "pass", s_live.wifi_pass);
    store_str(handle, "uri", s_live.mqtt_uri);
    store_str(handle, "user", s_live.mqtt_user);
    store_str(handle, "mpass", s_live.mqtt_pass);
    ESP_ERROR_CHECK(nvs_commit(handle));
    nvs_close(handle);
    wifi_sta_apply();
    mqtt_link_apply();
}
