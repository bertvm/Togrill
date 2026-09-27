#include "grill.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_lock;
static grill_view_t s_view;

void grill_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    memset(&s_view, 0, sizeof(s_view));
    snprintf(s_view.detail, sizeof(s_view.detail), "starting");
}

static void bump(void)
{
    s_view.generation++;
}

static int tenths(float celsius)
{
    float scaled = celsius * 10.0f;
    return (int)(scaled + (scaled >= 0.0f ? 0.5f : -0.5f));
}

void grill_set_wifi(bool up)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_view.wifi_up != up) {
        s_view.wifi_up = up;
        bump();
    }
    xSemaphoreGive(s_lock);
}

void grill_set_mqtt(bool up)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_view.mqtt_up != up) {
        s_view.mqtt_up = up;
        bump();
    }
    xSemaphoreGive(s_lock);
}

void grill_set_ble(bool up, const char *peer)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    char next_peer[sizeof(s_view.peer)];
    snprintf(next_peer, sizeof(next_peer), "%s", s_view.peer);
    if (peer != NULL) {
        snprintf(next_peer, sizeof(next_peer), "%s", peer);
    } else if (!up) {
        next_peer[0] = '\0';
    }
    if (s_view.ble_up != up || strcmp(s_view.peer, next_peer) != 0) {
        s_view.ble_up = up;
        snprintf(s_view.peer, sizeof(s_view.peer), "%s", next_peer);
        bump();
    }
    xSemaphoreGive(s_lock);
}

void grill_set_detail(const char *detail)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const char *text = detail != NULL ? detail : "";
    if (strcmp(s_view.detail, text) != 0) {
        snprintf(s_view.detail, sizeof(s_view.detail), "%s", text);
        bump();
    }
    xSemaphoreGive(s_lock);
}

void grill_set_device(int battery, int probe_count, bool has_ambient)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_view.has_device_info || s_view.battery != battery || s_view.probe_count != probe_count ||
        s_view.has_ambient != has_ambient) {
        s_view.has_device_info = true;
        s_view.battery = battery;
        s_view.probe_count = probe_count;
        s_view.has_ambient = has_ambient;
        bump();
    }
    xSemaphoreGive(s_lock);
}

void grill_set_temperatures(const float *celsius, const bool *valid, int count)
{
    if (count > TOGRILL_MAX_SLOTS) {
        count = TOGRILL_MAX_SLOTS;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool changed = s_view.slot_count != count;
    for (int i = 0; i < count && !changed; i++) {
        changed = s_view.slot_valid[i] != valid[i] || tenths(s_view.slot_c[i]) != tenths(celsius[i]);
    }
    if (!changed) {
        xSemaphoreGive(s_lock);
        return;
    }
    s_view.slot_count = count;
    for (int i = 0; i < TOGRILL_MAX_SLOTS; i++) {
        s_view.slot_valid[i] = false;
        s_view.slot_c[i] = 0;
    }
    for (int i = 0; i < count; i++) {
        s_view.slot_valid[i] = valid[i];
        s_view.slot_c[i] = celsius[i];
    }
    bump();
    xSemaphoreGive(s_lock);
}

void grill_snapshot(grill_view_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_view;
    xSemaphoreGive(s_lock);
}
