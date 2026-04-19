#include "relay_i2c.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "relay_i2c";

// SparkFun Qwiic Single Relay command bytes. The relay treats a
// single-byte write as a command, no register-offset prefix.
#define RELAY_CMD_OFF 0x00
#define RELAY_CMD_ON  0x01

// Per-transmit timeout. The I²C write is trivially short (1 byte +
// addr + ACK). A generous 100 ms covers even a mid-EEPROM-write
// relay without making a real failure feel sluggish.
#define RELAY_XFER_TIMEOUT_MS 100

static i2c_master_dev_handle_t s_open_dev  = NULL;
static i2c_master_dev_handle_t s_close_dev = NULL;
static uint32_t                s_pulse_ms  = 500;

esp_err_t relay_i2c_init(const relay_i2c_config_t *cfg)
{
    if (!cfg || !cfg->bus) {
        return ESP_ERR_INVALID_ARG;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .scl_speed_hz    = 100 * 1000,
    };

    dev_cfg.device_address = cfg->open_addr;
    esp_err_t err = i2c_master_bus_add_device(cfg->bus, &dev_cfg, &s_open_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add_device open (0x%02x) failed: %s",
                 cfg->open_addr, esp_err_to_name(err));
        return err;
    }

    dev_cfg.device_address = cfg->close_addr;
    err = i2c_master_bus_add_device(cfg->bus, &dev_cfg, &s_close_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add_device close (0x%02x) failed: %s",
                 cfg->close_addr, esp_err_to_name(err));
        // Leave s_open_dev attached — the driver can still pulse OPEN
        // even if CLOSE isn't on the bus.
        return err;
    }

    s_pulse_ms = cfg->pulse_ms;
    ESP_LOGI(TAG, "ready: open=0x%02x close=0x%02x pulse=%ums",
             cfg->open_addr, cfg->close_addr, (unsigned)s_pulse_ms);
    return ESP_OK;
}

static esp_err_t pulse(i2c_master_dev_handle_t dev, const char *label)
{
    if (!dev) {
        ESP_LOGE(TAG, "%s: relay not attached", label);
        return ESP_ERR_INVALID_STATE;
    }

    const uint8_t on  = RELAY_CMD_ON;
    const uint8_t off = RELAY_CMD_OFF;

    esp_err_t err = i2c_master_transmit(dev, &on, 1, RELAY_XFER_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: ON transmit failed: %s", label, esp_err_to_name(err));
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(s_pulse_ms));

    err = i2c_master_transmit(dev, &off, 1, RELAY_XFER_TIMEOUT_MS);
    if (err != ESP_OK) {
        // Relay may be stuck ON; log loudly. A reboot clears it.
        ESP_LOGE(TAG, "%s: OFF transmit failed: %s — relay may be stuck ON",
                 label, esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "%s: pulsed %ums", label, (unsigned)s_pulse_ms);
    return ESP_OK;
}

esp_err_t relay_i2c_pulse_open(void)
{
    return pulse(s_open_dev, "open");
}

esp_err_t relay_i2c_pulse_close(void)
{
    return pulse(s_close_dev, "close");
}
