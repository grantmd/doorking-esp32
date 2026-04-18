#include "i2c_bus.h"

#include <stdio.h>

#include "board.h"
#include "esp_log.h"

static const char *TAG = "i2c_bus";

const char *i2c_bus_known_label(uint8_t addr)
{
    switch (addr) {
    case 0x18: return "SparkFun Qwiic Relay (default)";
    case 0x19: return "SparkFun Qwiic Relay (alt addr)";
    case 0x36: return "MAX17048 battery fuel gauge";
    default:   return "";
    }
}

// Internal: format a scan label for ESP_LOG (prefixed with space+paren).
static const char *log_suffix(uint8_t addr)
{
    static char buf[48];
    const char *lbl = i2c_bus_known_label(addr);
    if (lbl[0] == '\0') {
        return "";
    }
    snprintf(buf, sizeof(buf), " (%s)", lbl);
    return buf;
}

size_t i2c_bus_scan(i2c_master_bus_handle_t bus,
                    i2c_bus_scan_entry_t *entries,
                    size_t max_entries)
{
    size_t found = 0;
    for (uint8_t addr = 0x01; addr < 0x78; addr++) {
        esp_err_t err = i2c_master_probe(bus, addr, 50 /* ms */);
        if (err != ESP_OK) {
            continue;
        }
        if (entries && found < max_entries) {
            entries[found].addr  = addr;
            entries[found].label = i2c_bus_known_label(addr);
        }
        found++;
    }
    return found;
}

i2c_master_bus_handle_t i2c_bus_init_and_scan(void)
{
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port                = I2C_NUM_0,
        .sda_io_num              = I2C_MASTER_SDA_GPIO,
        .scl_io_num              = I2C_MASTER_SCL_GPIO,
        .clk_source              = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt       = 7,
        .flags.enable_internal_pullup = true,
    };

    i2c_master_bus_handle_t bus = NULL;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    ESP_LOGI(TAG, "i2c master on SDA=%d SCL=%d, scanning...",
             I2C_MASTER_SDA_GPIO, I2C_MASTER_SCL_GPIO);

    int found = 0;
    for (uint8_t addr = 0x01; addr < 0x78; addr++) {
        esp_err_t err = i2c_master_probe(bus, addr, 50 /* ms */);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "  0x%02x ACK%s", addr, log_suffix(addr));
            found++;
        }
    }

    if (found == 0) {
        ESP_LOGW(TAG, "  no devices found on i2c bus");
    } else {
        ESP_LOGI(TAG, "  %d device(s) found", found);
    }

    return bus;
}

esp_err_t i2c_bus_relay_change_address(i2c_master_bus_handle_t bus,
                                       uint8_t from_addr,
                                       uint8_t to_addr)
{
    // Attach a temporary device handle at the relay's current address.
    // SparkFun's firmware accepts the default 100 kHz standard-mode
    // clock that matches i2c_bus_init_and_scan.
    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = from_addr,
        .scl_speed_hz    = 100 * 1000,
    };

    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add_device(0x%02x) failed: %s", from_addr, esp_err_to_name(err));
        return err;
    }

    // SINGLE_CHANGE_ADDRESS command register is 0x03. Payload is the
    // new 7-bit address. The relay writes this to its onboard EEPROM
    // and begins responding at the new address immediately.
    const uint8_t cmd[2] = { 0x03, to_addr };
    err = i2c_master_transmit(dev, cmd, sizeof(cmd), 100 /* ms */);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "transmit change-address failed: %s", esp_err_to_name(err));
    }

    esp_err_t rm = i2c_master_bus_rm_device(dev);
    if (rm != ESP_OK) {
        ESP_LOGW(TAG, "rm_device failed: %s", esp_err_to_name(rm));
    }

    return err;
}
