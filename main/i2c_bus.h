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

#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the I²C master bus and run a one-time address scan at
// boot. Logs every responding address at INFO level, with labels for
// known devices (MAX17048 fuel gauge, Qwiic Relay). Returns the bus
// handle for other modules to register devices on. Must be called
// exactly once.
i2c_master_bus_handle_t i2c_bus_init_and_scan(void);

#ifdef __cplusplus
}
#endif
