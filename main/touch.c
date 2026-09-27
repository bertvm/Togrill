#include "touch.h"

#include "display.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

static const char *TAG = "touch";
static i2c_master_dev_handle_t s_dev;

static esp_err_t gt_read(uint16_t reg, uint8_t *data, size_t len)
{
    uint8_t addr[2] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF)};
    return i2c_master_transmit_receive(s_dev, addr, sizeof(addr), data, len, 50);
}

static void gt_ack(void)
{
    uint8_t buf[3] = {0x81, 0x4e, 0x00};
    i2c_master_transmit(s_dev, buf, sizeof(buf), 50);
}

bool touch_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = 33,
        .scl_io_num = 32,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
        ESP_LOGW(TAG, "I2C bus failed");
        return false;
    }

    uint16_t address = 0;
    if (i2c_master_probe(bus, 0x5d, 50) == ESP_OK) {
        address = 0x5d;
    } else if (i2c_master_probe(bus, 0x14, 50) == ESP_OK) {
        address = 0x14;
    } else {
        ESP_LOGW(TAG, "GT911 not found");
        return false;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = 100000,
    };
    if (i2c_master_bus_add_device(bus, &dev_cfg, &s_dev) != ESP_OK) {
        ESP_LOGW(TAG, "GT911 device add failed");
        return false;
    }
    ESP_LOGI(TAG, "GT911 at 0x%02x", address);
    return true;
}

bool touch_read(int *x, int *y)
{
    if (s_dev == NULL) {
        return false;
    }
    uint8_t status = 0;
    if (gt_read(0x814e, &status, 1) != ESP_OK) {
        return false;
    }
    if ((status & 0x80) == 0) {
        return false;
    }
    bool down = (status & 0x0f) > 0 && (status & 0x0f) <= 5;
    uint8_t point[7] = {0};
    if (down && gt_read(0x814f, point, sizeof(point)) == ESP_OK) {
        int raw_x = point[3] | (point[4] << 8);
        int raw_y = point[1] | (point[2] << 8);
        /* Vendor rotation, then the 180 degree correction used by the P2000 port. */
        int mx = LCD_W - 1 - raw_x;
        int my = raw_y;
        mx = LCD_W - 1 - mx;
        my = LCD_H - 1 - my;
        down = mx >= 0 && mx < LCD_W && my >= 0 && my < LCD_H;
        if (down) {
            *x = mx;
            *y = my;
        }
    } else {
        down = false;
    }
    gt_ack();
    return down;
}
