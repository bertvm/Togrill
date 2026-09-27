#pragma once

#include <stdbool.h>
#include <stdint.h>

#define TOGRILL_MAX_SLOTS 8

typedef struct {
    bool wifi_up;
    bool mqtt_up;
    bool ble_up;
    bool has_device_info;
    int battery;
    int probe_count;
    bool has_ambient;
    int slot_count;
    bool slot_valid[TOGRILL_MAX_SLOTS];
    float slot_c[TOGRILL_MAX_SLOTS];
    char peer[18];
    char detail[40];
    uint32_t generation;
} grill_view_t;

void grill_init(void);
void grill_set_wifi(bool up);
void grill_set_mqtt(bool up);
void grill_set_ble(bool up, const char *peer);
void grill_set_detail(const char *detail);
void grill_set_device(int battery, int probe_count, bool has_ambient);
void grill_set_temperatures(const float *celsius, const bool *valid, int count);
void grill_snapshot(grill_view_t *out);
