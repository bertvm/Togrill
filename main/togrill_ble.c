#include "togrill_ble.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "grill.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_npl_os.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "wifi_sta.h"

void ble_store_config_init(void);

static const char *TAG = "togrill";

static uint8_t s_own_addr_type;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_svc_start;
static uint16_t s_svc_end;
static uint16_t s_write_handle;
static uint16_t s_notify_handle;
static uint16_t s_cccd_handle;
static uint8_t s_notify_props;
static bool s_write_no_rsp;
static bool s_connecting;
static bool s_ready;
static bool s_got_notify;
static ble_addr_t s_peer_addr;
static bool s_wifi_up;
static bool s_wifi_task;
static int s_connect_tries;
static bool s_callouts;
static struct ble_npl_callout s_poll_co;
static struct ble_npl_callout s_retry_co;

static void start_wifi_task(void *arg)
{
    (void)arg;
    wifi_sta_start();
    vTaskDelete(NULL);
}

static void start_wifi_after_link(void)
{
    if (s_wifi_task) {
        return;
    }
    s_wifi_task = true;
    if (xTaskCreate(start_wifi_task, "wifi_boot", 6144, NULL, 3, NULL) != pdPASS) {
        s_wifi_task = false;
        ESP_LOGE(TAG, "wifi task failed");
    }
}

void togrill_ble_note_wifi(void)
{
    s_wifi_up = true;
}

static bool uuid_is(const ble_uuid_t *uuid, uint16_t want)
{
    if (uuid->type == BLE_UUID_TYPE_16) {
        return ble_uuid_u16(uuid) == want;
    }
    if (uuid->type == BLE_UUID_TYPE_128) {
        const uint8_t *value = BLE_UUID128(uuid)->value;
        static const uint8_t base[12] = {0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00};
        return memcmp(value, base, sizeof(base)) == 0 && value[12] == (uint8_t)(want & 0xff) &&
               value[13] == (uint8_t)(want >> 8) && value[14] == 0 && value[15] == 0;
    }
    return false;
}

static int gap_event(struct ble_gap_event *event, void *arg);
static void try_connect(void);

static void format_addr(const ble_addr_t *addr, char *out, size_t out_len)
{
    snprintf(out, out_len, "%02X:%02X:%02X:%02X:%02X:%02X", addr->val[5], addr->val[4], addr->val[3],
             addr->val[2], addr->val[1], addr->val[0]);
}

static bool address_wanted(const ble_addr_t *addr)
{
    if (CONFIG_TOGRILL_PEER_ADDRESS[0] == '\0') {
        return true;
    }
    char printed[18];
    format_addr(addr, printed, sizeof(printed));
    return strcasecmp(printed, CONFIG_TOGRILL_PEER_ADDRESS) == 0;
}

static bool contains_ignore_case(const uint8_t *data, uint8_t len, const char *needle)
{
    size_t nlen = strlen(needle);
    if (nlen == 0 || len < nlen) {
        return false;
    }
    for (uint8_t i = 0; i + nlen <= len; i++) {
        size_t j = 0;
        for (; j < nlen; j++) {
            char a = (char)data[i + j];
            char b = needle[j];
            if (a >= 'A' && a <= 'Z') {
                a = (char)(a - 'A' + 'a');
            }
            if (b >= 'A' && b <= 'Z') {
                b = (char)(b - 'A' + 'a');
            }
            if (a != b) {
                break;
            }
        }
        if (j == nlen) {
            return true;
        }
    }
    return false;
}

static bool advertisement_matches(const struct ble_hs_adv_fields *fields, const ble_addr_t *addr)
{
    if (!address_wanted(addr)) {
        return false;
    }
    if (CONFIG_TOGRILL_PEER_ADDRESS[0] != '\0') {
        return true;
    }
    for (int i = 0; i < fields->num_uuids16; i++) {
        if (ble_uuid_u16(&fields->uuids16[i].u) == 0xCEE0) {
            return true;
        }
    }
    for (int i = 0; i < fields->num_uuids128; i++) {
        if (uuid_is(&fields->uuids128[i].u, 0xCEE0)) {
            return true;
        }
    }
    if (fields->mfg_data_len >= 2) {
        uint16_t company = (uint16_t)(fields->mfg_data[0] | (fields->mfg_data[1] << 8));
        if (company == 0x879A) {
            return true;
        }
    }
    if (fields->name != NULL && contains_ignore_case(fields->name, fields->name_len, "grill")) {
        return true;
    }
    return false;
}

static uint8_t frame_checksum(const uint8_t *data, size_t len)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum ^= data[i];
    }
    return sum;
}

static int write_done(uint16_t conn_handle, const struct ble_gatt_error *error, struct ble_gatt_attr *attr, void *arg)
{
    (void)conn_handle;
    (void)attr;
    (void)arg;
    if (error->status != 0) {
        ESP_LOGW(TAG, "write failed: %d", error->status);
    }
    return 0;
}

static void send_payload(const uint8_t *payload, size_t len)
{
    if (!s_ready || s_write_handle == 0 || len > 20) {
        return;
    }
    uint8_t frame[26];
    frame[0] = 0x55;
    frame[1] = 0xAA;
    frame[2] = (uint8_t)((len >> 8) & 0xFF);
    frame[3] = (uint8_t)(len & 0xFF);
    memcpy(frame + 4, payload, len);
    frame[4 + len] = frame_checksum(frame, 4 + len);
    int rc;
    if (s_write_no_rsp) {
        rc = ble_gattc_write_no_rsp_flat(s_conn, s_write_handle, frame, 5 + len);
    } else {
        rc = ble_gattc_write_flat(s_conn, s_write_handle, frame, 5 + len, write_done, NULL);
    }
    if (rc != 0) {
        ESP_LOGW(TAG, "queue write failed: %d", rc);
    }
}

static void request_status(void)
{
    const uint8_t payload[] = {0xA0, 0x00, 0x00};
    send_payload(payload, sizeof(payload));
}

static void request_temperatures(void)
{
    const uint8_t payload[] = {0xA1, 0x00};
    send_payload(payload, sizeof(payload));
}

static void poll_ev(struct ble_npl_event *ev)
{
    (void)ev;
    ble_npl_callout_reset(&s_poll_co, ble_npl_time_ms_to_ticks32(3000));
    if (!s_ready) {
        return;
    }
    if (!s_got_notify) {
        grill_set_detail("linked, no data");
    }
    request_status();
    request_temperatures();
}

static bool decode_temp(uint16_t raw, float *out)
{
    if (raw == 0xFFFF) {
        return false;
    }
    if (raw > 32768) {
        *out = (float)(raw - 32768) / 10.0f;
    } else {
        *out = (float)raw / 10.0f;
    }
    return true;
}

static void handle_packet(const uint8_t *packet, size_t len)
{
    if (len < 1) {
        return;
    }
    if (packet[0] == 0xA0 && len >= 6) {
        uint8_t bitfield = packet[5];
        int probe_count = (bitfield >> 4) & 0x07;
        bool ambient = (bitfield & 0x80) != 0;
        grill_set_device(packet[1], probe_count, ambient);
        ESP_LOGI(TAG, "device battery=%u probes=%d ambient=%d", packet[1], probe_count, ambient);
        return;
    }
    if (packet[0] != 0xA1) {
        return;
    }
    float temps[TOGRILL_MAX_SLOTS] = {0};
    bool valid[TOGRILL_MAX_SLOTS] = {0};
    int count = 0;
    for (size_t i = 1; i + 1 < len && count < TOGRILL_MAX_SLOTS; i += 2) {
        uint16_t raw = (uint16_t)((packet[i] << 8) | packet[i + 1]);
        valid[count] = decode_temp(raw, &temps[count]);
        count++;
    }
    grill_set_temperatures(temps, valid, count);
    if (!s_got_notify) {
        s_got_notify = true;
        grill_set_detail("live");
    }
    ESP_LOGI(TAG, "temperatures %d slots", count);
}

static void handle_notify(const uint8_t *data, uint16_t len)
{
    if (len < 5 || data[0] != 0x55 || data[1] != 0xAA) {
        return;
    }
    uint16_t payload_len = (uint16_t)((data[2] << 8) | data[3]);
    if ((size_t)payload_len + 5 != len) {
        ESP_LOGW(TAG, "length mismatch %u vs %u", payload_len, len);
        return;
    }
    if (frame_checksum(data, len - 1) != data[len - 1]) {
        ESP_LOGW(TAG, "bad checksum");
        return;
    }
    handle_packet(data + 4, payload_len);
}

static int subscribe_done(uint16_t conn_handle, const struct ble_gatt_error *error, struct ble_gatt_attr *attr, void *arg)
{
    (void)conn_handle;
    (void)attr;
    (void)arg;
    if (error->status != 0) {
        ESP_LOGE(TAG, "subscribe failed: %d", error->status);
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }
    s_ready = true;
    s_got_notify = false;
    grill_set_detail("linked");
    start_wifi_after_link();
    request_status();
    request_temperatures();
    ble_npl_callout_reset(&s_poll_co, ble_npl_time_ms_to_ticks32(3000));
    return 0;
}

static int dsc_cb(uint16_t conn_handle, const struct ble_gatt_error *error, uint16_t chr_val_handle,
                  const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)chr_val_handle;
    (void)arg;
    if (error->status == BLE_HS_EDONE) {
        if (s_cccd_handle == 0) {
            ESP_LOGE(TAG, "notify characteristic has no CCCD");
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        uint8_t value[2] = {0x01, 0x00};
        if ((s_notify_props & BLE_GATT_CHR_PROP_NOTIFY) == 0 && (s_notify_props & BLE_GATT_CHR_PROP_INDICATE) != 0) {
            value[0] = 0x02;
        }
        int rc = ble_gattc_write_flat(conn_handle, s_cccd_handle, value, sizeof(value), subscribe_done, NULL);
        if (rc != 0) {
            ESP_LOGE(TAG, "cccd write failed: %d", rc);
        }
        return 0;
    }
    if (error->status != 0) {
        return error->status;
    }
    if (ble_uuid_u16(&dsc->uuid.u) == BLE_GATT_DSC_CLT_CFG_UUID16) {
        s_cccd_handle = dsc->handle;
    }
    return 0;
}

static int chr_cb(uint16_t conn_handle, const struct ble_gatt_error *error, const struct ble_gatt_chr *chr, void *arg)
{
    (void)arg;
    if (error->status == BLE_HS_EDONE) {
        if (s_write_handle == 0 || s_notify_handle == 0) {
            ESP_LOGE(TAG, "missing ToGrill characteristics");
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        s_cccd_handle = 0;
        int rc = ble_gattc_disc_all_dscs(conn_handle, s_notify_handle, s_svc_end, dsc_cb, NULL);
        if (rc != 0) {
            ESP_LOGE(TAG, "descriptor discovery failed: %d", rc);
        }
        return 0;
    }
    if (error->status != 0) {
        return error->status;
    }
    char printed[40];
    ble_uuid_to_str(&chr->uuid.u, printed);
    ESP_LOGI(TAG, "char %s handle %u props 0x%02x", printed, chr->val_handle, chr->properties);
    if (uuid_is(&chr->uuid.u, 0xCEE1)) {
        s_write_handle = chr->val_handle;
        s_write_no_rsp = (chr->properties & BLE_GATT_CHR_PROP_WRITE_NO_RSP) != 0;
    } else if (uuid_is(&chr->uuid.u, 0xCEE2)) {
        s_notify_handle = chr->val_handle;
        s_notify_props = chr->properties;
    }
    return 0;
}

static int svc_cb(uint16_t conn_handle, const struct ble_gatt_error *error, const struct ble_gatt_svc *service, void *arg)
{
    (void)arg;
    if (error->status == BLE_HS_EDONE) {
        if (s_svc_start == 0 || s_svc_end == 0) {
            ESP_LOGW(TAG, "ToGrill service not found");
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            return 0;
        }
        int rc = ble_gattc_disc_all_chrs(conn_handle, s_svc_start, s_svc_end, chr_cb, NULL);
        if (rc != 0) {
            ESP_LOGE(TAG, "characteristic discovery failed: %d", rc);
        }
        return 0;
    }
    if (error->status != 0) {
        return error->status;
    }
    if (!uuid_is(&service->uuid.u, 0xCEE0)) {
        return 0;
    }
    s_svc_start = service->start_handle;
    s_svc_end = service->end_handle;
    return 0;
}

static void start_scan(void)
{
    if (s_conn != BLE_HS_CONN_HANDLE_NONE || s_connecting) {
        return;
    }
    struct ble_gap_disc_params params = {
        .itvl = 160,
        .window = s_wifi_up ? 48 : 80,
        .filter_duplicates = 1,
        .passive = 0,
    };
    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "scan start failed: %d", rc);
    } else {
        grill_set_detail("scanning");
    }
}

static void connect_retry_ev(struct ble_npl_event *ev)
{
    (void)ev;
    try_connect();
}

static void try_connect(void)
{
    ESP_LOGI(TAG, "connecting");
    int rc = ble_gap_connect(s_own_addr_type, &s_peer_addr, 30000, NULL, gap_event, NULL);
    if (rc == 0) {
        s_connect_tries = 0;
        return;
    }
    ESP_LOGW(TAG, "connect failed: %d", rc);
    if ((rc == BLE_HS_EBUSY || rc == BLE_HS_EALREADY) && s_connect_tries < 5) {
        s_connect_tries++;
        ble_npl_callout_reset(&s_retry_co, ble_npl_time_ms_to_ticks32(200));
        return;
    }
    s_connect_tries = 0;
    s_connecting = false;
    start_scan();
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        struct ble_hs_adv_fields fields;
        if (ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data) != 0) {
            return 0;
        }
        if (!advertisement_matches(&fields, &event->disc.addr)) {
            return 0;
        }
        if (s_connecting || s_conn != BLE_HS_CONN_HANDLE_NONE) {
            return 0;
        }
        char addr[18];
        format_addr(&event->disc.addr, addr, sizeof(addr));
        ESP_LOGI(TAG, "found %s", addr);
        grill_set_ble(false, addr);
        grill_set_detail("connecting");
        s_peer_addr = event->disc.addr;
        s_connecting = true;
        int rc = ble_gap_disc_cancel();
        if (rc != 0 && rc != BLE_HS_EALREADY) {
            ESP_LOGW(TAG, "scan cancel: %d", rc);
        }
        try_connect();
        return 0;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        ESP_LOGI(TAG, "scan done reason=%d", event->disc_complete.reason);
        if (!s_connecting && s_conn == BLE_HS_CONN_HANDLE_NONE) {
            start_scan();
        }
        return 0;
    case BLE_GAP_EVENT_CONNECT:
        s_connecting = false;
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "connection failed: %d", event->connect.status);
            grill_set_detail("connect failed");
            start_scan();
            return 0;
        }
        s_conn = event->connect.conn_handle;
        s_ready = false;
        s_write_handle = 0;
        s_notify_handle = 0;
        s_cccd_handle = 0;
        s_svc_start = 0;
        s_svc_end = 0;
        grill_set_ble(true, NULL);
        grill_set_detail("discovering");
        if (ble_gattc_disc_all_svcs(s_conn, svc_cb, NULL) != 0) {
            ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected, reason=%d", event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_ready = false;
        s_got_notify = false;
        s_connecting = false;
        ble_npl_callout_stop(&s_poll_co);
        grill_set_ble(false, NULL);
        grill_set_detail("scanning");
        start_scan();
        return 0;
    case BLE_GAP_EVENT_NOTIFY_RX: {
        uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
        uint8_t buf[48];
        if (len > sizeof(buf)) {
            len = sizeof(buf);
        }
        os_mbuf_copydata(event->notify_rx.om, 0, len, buf);
        handle_notify(buf, len);
        return 0;
    }
    default:
        return 0;
    }
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "nimble reset, reason=%d", reason);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure address failed: %d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer address type failed: %d", rc);
        return;
    }
    if (!s_callouts) {
        ble_npl_callout_init(&s_poll_co, nimble_port_get_dflt_eventq(), poll_ev, NULL);
        ble_npl_callout_init(&s_retry_co, nimble_port_get_dflt_eventq(), connect_retry_ev, NULL);
        s_callouts = true;
    }
    start_scan();
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void wifi_fallback_cb(void *arg)
{
    (void)arg;
    if (s_wifi_task) {
        return;
    }
    ESP_LOGW(TAG, "thermometer not linked yet, starting WiFi");
    grill_set_detail("wifi without probe");
    start_wifi_after_link();
}

void togrill_ble_start(void)
{
    const esp_timer_create_args_t fallback = {
        .callback = wifi_fallback_cb,
        .name = "wifi_fallback",
    };
    esp_timer_handle_t fallback_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&fallback, &fallback_timer));
    ESP_ERROR_CHECK(esp_timer_start_once(fallback_timer, 45 * 1000 * 1000));
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_store_config_init();
    nimble_port_freertos_init(host_task);
}
