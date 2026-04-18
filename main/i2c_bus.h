// I²C master bus initialization and diagnostic scan.
//
// Brings up the I²C master on the board-specific SDA/SCL pins (from
// board.h), probes every 7-bit address, and logs which devices respond.
// Returns a bus handle that callers (e.g. the future relay_i2c driver)
// can use to add device handles on the same bus.
//
// The bus runs at 100 kHz (I²C standard mode) with internal pull-ups
// enabled. External 4.7 kΩ pull-ups on the Qwiic connector are
// preferred and typically already present on SparkFun / Seeed boards.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the I²C master bus and run a one-time address scan at
// boot. Logs every responding address at INFO level, with labels for
// known devices (MAX17048 fuel gauge, Qwiic Relay). Returns the bus
// handle for other modules to register devices on. Must be called
// exactly once.
i2c_master_bus_handle_t i2c_bus_init_and_scan(void);

// One entry in an I²C scan result.
typedef struct {
    uint8_t     addr;
    const char *label;  // "" if unknown; otherwise a short device name
} i2c_bus_scan_entry_t;

// Probe every 7-bit address on `bus` and fill `entries` with each ACK.
// Writes at most `max_entries` entries. Returns the total number of
// devices found (may exceed max_entries; entries beyond max_entries are
// discarded but still counted).
size_t i2c_bus_scan(i2c_master_bus_handle_t bus,
                    i2c_bus_scan_entry_t *entries,
                    size_t max_entries);

// Return a short human-readable label for known I²C devices, or ""
// if `addr` isn't one we recognize.
const char *i2c_bus_known_label(uint8_t addr);

// Send the SparkFun Qwiic Single Relay address-change command to the
// relay currently at `from_addr`, instructing it to respond at
// `to_addr` going forward. Writes the 2-byte sequence {0x03, to_addr},
// which the relay persists to its onboard EEPROM. Caller should verify
// afterwards by probing `to_addr`. Returns ESP_OK on transmit success.
esp_err_t i2c_bus_relay_change_address(i2c_master_bus_handle_t bus,
                                       uint8_t from_addr,
                                       uint8_t to_addr);

#ifdef __cplusplus
}
#endif
