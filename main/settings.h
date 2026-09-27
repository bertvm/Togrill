#pragma once

#define SETTINGS_SSID_LEN 32
#define SETTINGS_PASS_LEN 64
#define SETTINGS_URI_LEN 80
#define SETTINGS_USER_LEN 32

typedef struct {
    char wifi_ssid[SETTINGS_SSID_LEN + 1];
    char wifi_pass[SETTINGS_PASS_LEN + 1];
    char mqtt_uri[SETTINGS_URI_LEN + 1];
    char mqtt_user[SETTINGS_USER_LEN + 1];
    char mqtt_pass[SETTINGS_PASS_LEN + 1];
} app_settings_t;

void settings_init(void);
const app_settings_t *settings_get(void);
void settings_save(const app_settings_t *next);
