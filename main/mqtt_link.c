#include "mqtt_link.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "grill.h"
#include "mqtt_client.h"
#include "sdkconfig.h"
#include "settings.h"

static const char *TAG = "mqtt";
static esp_mqtt_client_handle_t s_client;
static bool s_connected;
static bool s_task_started;
static app_settings_t s_cfg;
static SemaphoreHandle_t s_lock;

static void ensure_lock(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
}

static void publish_number(const char *topic, bool valid, float value)
{
    char payload[16];
    if (!valid) {
        snprintf(payload, sizeof(payload), "none");
    } else {
        snprintf(payload, sizeof(payload), "%.1f", value);
    }
    esp_mqtt_client_publish(s_client, topic, payload, 0, 0, 1);
}

static void publish_view(const grill_view_t *view)
{
    if (s_lock == NULL || xSemaphoreTake(s_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }
    if (!s_connected || s_client == NULL) {
        xSemaphoreGive(s_lock);
        return;
    }

    char topic[64];
    char body[320];
    int probes = view->has_device_info ? view->probe_count : view->slot_count;
    if (probes < 0) {
        probes = 0;
    }
    if (probes > TOGRILL_MAX_SLOTS) {
        probes = TOGRILL_MAX_SLOTS;
    }

    int n = snprintf(body, sizeof(body),
                     "{\"ble\":%s,\"battery\":%d,\"probe_count\":%d,\"probes\":[",
                     view->ble_up ? "true" : "false",
                     view->has_device_info ? view->battery : -1, probes);
    for (int i = 0; i < probes && n > 0 && n < (int)sizeof(body) - 1; i++) {
        if (view->slot_valid[i]) {
            n += snprintf(body + n, sizeof(body) - (size_t)n, "%s%.1f", i == 0 ? "" : ",", view->slot_c[i]);
        } else {
            n += snprintf(body + n, sizeof(body) - (size_t)n, "%snull", i == 0 ? "" : ",");
        }
    }
    if (n < 0 || n >= (int)sizeof(body)) {
        xSemaphoreGive(s_lock);
        return;
    }
    if (view->has_ambient && view->slot_count > 0 && view->slot_valid[view->slot_count - 1]) {
        snprintf(body + n, sizeof(body) - (size_t)n, "],\"ambient\":%.1f}", view->slot_c[view->slot_count - 1]);
    } else {
        snprintf(body + n, sizeof(body) - (size_t)n, "],\"ambient\":null}");
    }

    snprintf(topic, sizeof(topic), "%s/state", CONFIG_TOGRILL_MQTT_PREFIX);
    esp_mqtt_client_publish(s_client, topic, body, 0, 0, 1);

    for (int i = 0; i < probes; i++) {
        snprintf(topic, sizeof(topic), "%s/probe/%d", CONFIG_TOGRILL_MQTT_PREFIX, i + 1);
        publish_number(topic, view->slot_valid[i], view->slot_c[i]);
    }
    if (view->has_ambient && view->slot_count > 0) {
        snprintf(topic, sizeof(topic), "%s/ambient", CONFIG_TOGRILL_MQTT_PREFIX);
        int amb = view->slot_count - 1;
        publish_number(topic, view->slot_valid[amb], view->slot_c[amb]);
    }
    if (view->has_device_info) {
        snprintf(topic, sizeof(topic), "%s/battery", CONFIG_TOGRILL_MQTT_PREFIX);
        char payload[8];
        snprintf(payload, sizeof(payload), "%d", view->battery);
        esp_mqtt_client_publish(s_client, topic, payload, 0, 0, 1);
    }
    xSemaphoreGive(s_lock);
}

static void mqtt_events(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    esp_mqtt_event_handle_t event = data;
    if (id == MQTT_EVENT_CONNECTED) {
        s_connected = true;
        grill_set_mqtt(true);
        char topic[64];
        snprintf(topic, sizeof(topic), "%s/status", CONFIG_TOGRILL_MQTT_PREFIX);
        esp_mqtt_client_publish(s_client, topic, "online", 0, 1, 1);
        ESP_LOGI(TAG, "connected");
    } else if (id == MQTT_EVENT_DISCONNECTED) {
        s_connected = false;
        grill_set_mqtt(false);
        ESP_LOGW(TAG, "disconnected");
    } else if (id == MQTT_EVENT_ERROR) {
        ESP_LOGW(TAG, "error type %d", event->error_handle->error_type);
    }
}

static void mqtt_task(void *arg)
{
    (void)arg;
    uint32_t seen = 0;
    while (true) {
        grill_view_t view;
        grill_snapshot(&view);
        if (s_connected && view.generation != seen) {
            publish_view(&view);
            seen = view.generation;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

void mqtt_link_apply(void)
{
    ensure_lock();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg = *settings_get();
    if (s_cfg.mqtt_uri[0] == '\0') {
        return;
    }
    if (s_client != NULL) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        s_connected = false;
        grill_set_mqtt(false);
    }

    static char will_topic[64];
    snprintf(will_topic, sizeof(will_topic), "%s/status", CONFIG_TOGRILL_MQTT_PREFIX);

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = s_cfg.mqtt_uri,
        .session.last_will.topic = will_topic,
        .session.last_will.msg = "offline",
        .session.last_will.qos = 1,
        .session.last_will.retain = true,
    };
    if (s_cfg.mqtt_user[0] != '\0') {
        cfg.credentials.username = s_cfg.mqtt_user;
        cfg.credentials.authentication.password = s_cfg.mqtt_pass;
    }

    s_client = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_events, NULL);
    esp_mqtt_client_start(s_client);
    if (!s_task_started) {
        s_task_started = true;
        xTaskCreate(mqtt_task, "mqtt", 4096, NULL, 3, NULL);
    }
    xSemaphoreGive(s_lock);
}

void mqtt_link_start(void)
{
    if (s_client != NULL) {
        return;
    }
    mqtt_link_apply();
}
