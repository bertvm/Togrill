#include "display.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "touch.h"
#include "ui.h"

/* ESP32-2432S032, same wiring as the P2000 CYD port. */
#define LCD_HOST SPI2_HOST
#define PIN_SCLK 14
#define PIN_MOSI 13
#define PIN_MISO 12
#define PIN_DC 2
#define PIN_CS 15
#define PIN_BL 27
#define PIN_SD_CS 5

static const char *TAG = "display";
static lv_display_t *s_disp;

static void touch_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    int x = 0;
    int y = 0;
    if (touch_read(&x, &y)) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = x;
        data->point.y = y;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void ui_on_lvgl(void *arg)
{
    (void)arg;
    if (touch_init()) {
        lv_indev_t *indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, touch_cb);
        lv_indev_set_display(indev, s_disp);
    }
    ui_start();
    lv_refr_now(s_disp);
    gpio_set_level(PIN_BL, 1);
    ESP_LOGI(TAG, "ST7789 ready");
}

void display_start(void)
{
    gpio_config_t out = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << PIN_BL) | (1ULL << PIN_SD_CS) | (1ULL << 4) | (1ULL << 16) | (1ULL << 17),
    };
    ESP_ERROR_CHECK(gpio_config(&out));
    gpio_set_level(PIN_BL, 0);
    gpio_set_level(PIN_SD_CS, 1);
    gpio_set_level(4, 1);
    gpio_set_level(16, 1);
    gpio_set_level(17, 1);

    spi_bus_config_t bus = {
        .sclk_io_num = PIN_SCLK,
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * 40 * (int)sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_DC,
        .cs_gpio_num = PIN_CS,
        .pclk_hz = 40 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_HOST, &io_config, &io));

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_config, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    const lvgl_port_cfg_t lvgl_cfg = {
        .task_priority = 4,
        .task_stack = 16384,
        .task_affinity = -1,
        .task_max_sleep_ms = 500,
        .task_stack_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT,
        .timer_period_ms = 5,
    };
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = LCD_W * 20,
        .double_buffer = true,
        .hres = LCD_W,
        .vres = LCD_H,
        .rotation =
            {
                .swap_xy = true,
                .mirror_x = true,
                .mirror_y = false,
            },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags =
            {
                .buff_dma = 1,
                .swap_bytes = 0,
            },
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    ESP_ERROR_CHECK(s_disp != NULL ? ESP_OK : ESP_FAIL);

    if (!lvgl_port_lock(1000)) {
        ESP_LOGE(TAG, "LVGL lock failed");
        return;
    }
    lv_async_call(ui_on_lvgl, NULL);
    lvgl_port_unlock();
}
