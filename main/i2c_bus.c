#include "i2c_bus.h"

#include "board.h"
#include "esp_log.h"

static const char *TAG = "i2c_bus";

// Label known I²C addresses so the boot scan is immediately useful
// for debugging "is my device connected?" without a datasheet.
static const char *known_device(uint8_t addr)
{
    switch (addr) {
    case 0x18: return " (SparkFun Qwiic Relay — default)";
    case 0x19: return " (SparkFun Qwiic Relay — alt addr)";
    case 0x36: return " (MAX17048 battery fuel gauge)";
    default:   return "";
    }
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
            ESP_LOGI(TAG, "  0x%02x ACK%s", addr, known_device(addr));
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
