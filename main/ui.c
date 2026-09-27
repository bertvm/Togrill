#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "esp_lvgl_port.h"
#include "grill.h"
#include "settings.h"

#define COL_BG 0x0C0E10
#define COL_CARD 0x202428
#define COL_FG 0xEBE8DC
#define COL_DIM 0x6E7478
#define COL_OK 0x46B45A
#define COL_WARN 0xDC8C28
#define COL_BAD 0xBE4637
#define COL_KEY 0x30363C

typedef enum {
    FIELD_SSID,
    FIELD_WIFI_PASS,
    FIELD_MQTT_URI,
    FIELD_MQTT_USER,
    FIELD_MQTT_PASS,
    FIELD_COUNT,
} field_t;

static app_settings_t s_draft;
static field_t s_field;
static uint32_t s_seen = UINT32_MAX;

static lv_obj_t *s_temps;
static lv_obj_t *s_menu;
static lv_obj_t *s_edit;
static lv_obj_t *s_wifi;
static lv_obj_t *s_mqtt;
static lv_obj_t *s_ble;
static lv_obj_t *s_status;
static lv_obj_t *s_probe_value[4];
static lv_obj_t *s_ambient;
static lv_obj_t *s_battery;
static lv_obj_t *s_menu_value[FIELD_COUNT];
static lv_obj_t *s_edit_title;
static lv_obj_t *s_edit_text;
static lv_obj_t *s_keyboard;

static void style_screen(lv_obj_t *scr)
{
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(scr, lv_color_hex(COL_FG), 0);
    lv_obj_set_style_pad_all(scr, 6, 0);
    lv_obj_set_style_pad_row(scr, 6, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
}

static void style_card(lv_obj_t *obj)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 8, 0);
    lv_obj_set_style_pad_all(obj, 6, 0);
    lv_obj_set_style_pad_row(obj, 0, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static const char *field_label(field_t field)
{
    switch (field) {
    case FIELD_SSID:
        return "WiFi SSID";
    case FIELD_WIFI_PASS:
        return "WiFi password";
    case FIELD_MQTT_URI:
        return "MQTT broker";
    case FIELD_MQTT_USER:
        return "MQTT user";
    case FIELD_MQTT_PASS:
        return "MQTT password";
    default:
        return "";
    }
}

static char *field_buffer(field_t field)
{
    switch (field) {
    case FIELD_SSID:
        return s_draft.wifi_ssid;
    case FIELD_WIFI_PASS:
        return s_draft.wifi_pass;
    case FIELD_MQTT_URI:
        return s_draft.mqtt_uri;
    case FIELD_MQTT_USER:
        return s_draft.mqtt_user;
    case FIELD_MQTT_PASS:
        return s_draft.mqtt_pass;
    default:
        return s_draft.wifi_ssid;
    }
}

static size_t field_max(field_t field)
{
    switch (field) {
    case FIELD_SSID:
        return SETTINGS_SSID_LEN;
    case FIELD_WIFI_PASS:
    case FIELD_MQTT_PASS:
        return SETTINGS_PASS_LEN;
    case FIELD_MQTT_URI:
        return SETTINGS_URI_LEN;
    case FIELD_MQTT_USER:
        return SETTINGS_USER_LEN;
    default:
        return 0;
    }
}

static void menu_text(field_t field, char *out, size_t out_len)
{
    const char *value = field_buffer(field);
    if ((field == FIELD_WIFI_PASS || field == FIELD_MQTT_PASS) && value[0] == '\0') {
        snprintf(out, out_len, "empty");
        return;
    }
    if ((field == FIELD_WIFI_PASS || field == FIELD_MQTT_PASS)) {
        snprintf(out, out_len, "set");
        return;
    }
    if (value[0] == '\0') {
        snprintf(out, out_len, "empty");
        return;
    }
    snprintf(out, out_len, "%s", value);
}

static void refresh_menu(void)
{
    for (int i = 0; i < FIELD_COUNT; i++) {
        char shown[40];
        menu_text((field_t)i, shown, sizeof(shown));
        lv_label_set_text(s_menu_value[i], shown);
    }
}

static void show_temps(void)
{
    lv_screen_load(s_temps);
}

static void show_temps_event(lv_event_t *e)
{
    (void)e;
    show_temps();
}

static void show_menu(lv_event_t *e)
{
    (void)e;
    s_draft = *settings_get();
    refresh_menu();
    lv_screen_load(s_menu);
}

static void show_edit(field_t field)
{
    s_field = field;
    lv_label_set_text(s_edit_title, field_label(field));
    lv_textarea_set_max_length(s_edit_text, (uint32_t)field_max(field));
    lv_textarea_set_text(s_edit_text, field_buffer(field));
    lv_keyboard_set_textarea(s_keyboard, s_edit_text);
    lv_screen_load(s_edit);
}

static void menu_row_cb(lv_event_t *e)
{
    field_t field = (field_t)(uintptr_t)lv_event_get_user_data(e);
    show_edit(field);
}

static void save_cb(lv_event_t *e)
{
    (void)e;
    settings_save(&s_draft);
    show_temps();
}

static void edit_ready_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        snprintf(field_buffer(s_field), field_max(s_field) + 1, "%s", lv_textarea_get_text(s_edit_text));
        refresh_menu();
    }
    lv_screen_load(s_menu);
}

static void paint_link(lv_obj_t *label, bool up, bool alarm_down)
{
    lv_color_t color = lv_color_hex(COL_DIM);
    if (up) {
        color = lv_color_hex(COL_OK);
    } else if (alarm_down) {
        color = lv_color_hex(COL_BAD);
    }
    lv_obj_set_style_text_color(label, color, 0);
}

static void paint_reading(lv_obj_t *label, bool valid, float celsius)
{
    if (!valid) {
        lv_label_set_text(label, "--");
        lv_obj_set_style_text_color(label, lv_color_hex(COL_DIM), 0);
        return;
    }
    lv_label_set_text_fmt(label, "%.1f", celsius);
    lv_obj_set_style_text_color(label, lv_color_hex(celsius >= 80.0f ? COL_WARN : COL_FG), 0);
}

static int ambient_index(const grill_view_t *view)
{
    if (view->slot_count <= 0) {
        return -1;
    }
    if (view->has_ambient || (!view->has_device_info && view->slot_count > 4)) {
        return view->slot_count - 1;
    }
    return -1;
}

static void refresh_cb(lv_timer_t *timer)
{
    (void)timer;
    grill_view_t view;
    grill_snapshot(&view);
    if (view.generation == s_seen) {
        return;
    }
    s_seen = view.generation;

    paint_link(s_wifi, view.wifi_up, false);
    paint_link(s_mqtt, view.mqtt_up, false);
    paint_link(s_ble, view.ble_up, true);

    bool any = false;
    for (int i = 0; i < 4; i++) {
        bool valid = i < view.slot_count && view.slot_valid[i];
        any = any || valid;
        paint_reading(s_probe_value[i], valid, view.slot_c[i]);
    }

    if (any) {
        lv_obj_add_flag(s_status, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_status, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_status, view.detail[0] != '\0' ? view.detail : "scanning");
    }

    int amb = ambient_index(&view);
    if (amb >= 0) {
        paint_reading(s_ambient, view.slot_valid[amb], view.slot_c[amb]);
    } else {
        paint_reading(s_ambient, false, 0);
    }
    if (view.has_device_info) {
        lv_label_set_text_fmt(s_battery, "%d%%", view.battery);
    } else {
        lv_label_set_text(s_battery, "");
    }
}

static lv_obj_t *make_probe(lv_obj_t *parent, const char *name, lv_obj_t **value)
{
    lv_obj_t *card = lv_obj_create(parent);
    style_card(card);
    lv_obj_set_flex_grow(card, 1);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_min_width(card, 140, 0);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, name);
    lv_obj_set_style_text_color(title, lv_color_hex(COL_DIM), 0);

    *value = lv_label_create(card);
    lv_obj_set_style_text_font(*value, &lv_font_montserrat_28, 0);
    lv_label_set_text(*value, "--");
    return card;
}

static void build_temps(void)
{
    s_temps = lv_obj_create(NULL);
    style_screen(s_temps);

    lv_obj_t *header = lv_obj_create(s_temps);
    lv_obj_set_width(header, LV_PCT(100));
    lv_obj_set_height(header, 28);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "TOGRILL");
    lv_obj_set_flex_grow(title, 1);

    s_wifi = lv_label_create(header);
    lv_label_set_text(s_wifi, "WIFI");
    s_mqtt = lv_label_create(header);
    lv_label_set_text(s_mqtt, "MQTT");
    lv_obj_set_style_pad_left(s_mqtt, 8, 0);
    s_ble = lv_label_create(header);
    lv_label_set_text(s_ble, "BLE");
    lv_obj_set_style_pad_left(s_ble, 8, 0);

    s_status = lv_label_create(s_temps);
    lv_obj_set_style_text_color(s_status, lv_color_hex(COL_DIM), 0);
    lv_label_set_text(s_status, "starting");

    lv_obj_t *grid = lv_obj_create(s_temps);
    lv_obj_set_width(grid, LV_PCT(100));
    lv_obj_set_flex_grow(grid, 1);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_set_style_pad_column(grid, 6, 0);
    lv_obj_set_style_pad_row(grid, 6, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    make_probe(grid, "P1", &s_probe_value[0]);
    make_probe(grid, "P2", &s_probe_value[1]);
    make_probe(grid, "P3", &s_probe_value[2]);
    make_probe(grid, "P4", &s_probe_value[3]);

    lv_obj_t *bottom = lv_obj_create(s_temps);
    lv_obj_set_width(bottom, LV_PCT(100));
    lv_obj_set_height(bottom, 48);
    lv_obj_set_style_bg_opa(bottom, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bottom, 0, 0);
    lv_obj_set_style_pad_all(bottom, 0, 0);
    lv_obj_set_style_pad_column(bottom, 6, 0);
    lv_obj_set_flex_flow(bottom, LV_FLEX_FLOW_ROW);
    lv_obj_remove_flag(bottom, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *amb = lv_obj_create(bottom);
    style_card(amb);
    lv_obj_set_flex_grow(amb, 1);
    lv_obj_set_height(amb, LV_PCT(100));
    lv_obj_set_flex_flow(amb, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(amb, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *amb_name = lv_label_create(amb);
    lv_label_set_text(amb_name, "AMB");
    lv_obj_set_style_text_color(amb_name, lv_color_hex(COL_DIM), 0);
    lv_obj_set_style_pad_right(amb_name, 8, 0);
    s_ambient = lv_label_create(amb);
    lv_obj_set_style_text_font(s_ambient, &lv_font_montserrat_28, 0);
    lv_label_set_text(s_ambient, "--");
    lv_obj_set_flex_grow(s_ambient, 1);
    s_battery = lv_label_create(amb);
    lv_label_set_text(s_battery, "");

    lv_obj_t *setup = lv_button_create(bottom);
    lv_obj_set_width(setup, 78);
    lv_obj_set_height(setup, LV_PCT(100));
    lv_obj_set_style_bg_color(setup, lv_color_hex(COL_KEY), 0);
    lv_obj_set_style_shadow_width(setup, 0, 0);
    lv_obj_add_event_cb(setup, show_menu, LV_EVENT_CLICKED, NULL);
    lv_obj_t *setup_label = lv_label_create(setup);
    lv_label_set_text(setup_label, "Setup");
    lv_obj_center(setup_label);
}

static lv_obj_t *make_row(lv_obj_t *parent, field_t field)
{
    lv_obj_t *row = lv_button_create(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, 32);
    lv_obj_set_style_bg_color(row, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_shadow_width(row, 0, 0);
    lv_obj_set_style_pad_hor(row, 8, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(row, menu_row_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)field);

    lv_obj_t *name = lv_label_create(row);
    lv_label_set_text(name, field_label(field));
    lv_obj_set_style_text_color(name, lv_color_hex(COL_DIM), 0);

    s_menu_value[field] = lv_label_create(row);
    lv_label_set_long_mode(s_menu_value[field], LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_menu_value[field], 150);
    lv_obj_set_style_text_align(s_menu_value[field], LV_TEXT_ALIGN_RIGHT, 0);
    return row;
}

static void build_menu(void)
{
    s_menu = lv_obj_create(NULL);
    style_screen(s_menu);

    lv_obj_t *header = lv_obj_create(s_menu);
    lv_obj_set_width(header, LV_PCT(100));
    lv_obj_set_height(header, 32);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *title = lv_label_create(header);
    lv_label_set_text(title, "Setup");
    lv_obj_t *back = lv_button_create(header);
    lv_obj_set_style_bg_color(back, lv_color_hex(COL_KEY), 0);
    lv_obj_set_style_shadow_width(back, 0, 0);
    lv_obj_add_event_cb(back, show_temps_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_label = lv_label_create(back);
    lv_label_set_text(back_label, "Back");

    for (int i = 0; i < FIELD_COUNT; i++) {
        make_row(s_menu, (field_t)i);
    }

    lv_obj_t *save = lv_button_create(s_menu);
    lv_obj_set_width(save, LV_PCT(100));
    lv_obj_set_height(save, 36);
    lv_obj_set_style_bg_color(save, lv_color_hex(COL_OK), 0);
    lv_obj_set_style_shadow_width(save, 0, 0);
    lv_obj_add_event_cb(save, save_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *save_label = lv_label_create(save);
    lv_label_set_text(save_label, "Save");
    lv_obj_set_style_text_color(save_label, lv_color_hex(COL_BG), 0);
    lv_obj_center(save_label);
}

static void build_edit(void)
{
    s_edit = lv_obj_create(NULL);
    style_screen(s_edit);

    s_edit_title = lv_label_create(s_edit);
    lv_label_set_text(s_edit_title, "");

    s_edit_text = lv_textarea_create(s_edit);
    lv_obj_set_width(s_edit_text, LV_PCT(100));
    lv_textarea_set_one_line(s_edit_text, true);
    lv_obj_set_style_bg_color(s_edit_text, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_border_width(s_edit_text, 0, 0);

    s_keyboard = lv_keyboard_create(s_edit);
    lv_obj_set_width(s_keyboard, LV_PCT(100));
    lv_obj_set_flex_grow(s_keyboard, 1);
    lv_obj_add_event_cb(s_keyboard, edit_ready_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_keyboard, edit_ready_cb, LV_EVENT_CANCEL, NULL);
}

void ui_start(void)
{
    s_draft = *settings_get();
    build_temps();
    build_menu();
    build_edit();
    refresh_menu();
    show_temps();
    lv_timer_create(refresh_cb, 200, NULL);
}
