// Qwiic Single Relay driver — pulses the OPEN / CLOSE relays that
// drive the DoorKing 4602-010's dry-contact inputs.
//
// The DKS controller treats its OPEN and CLOSE inputs as momentary
// pushbutton equivalents: any transient contact closure of a few
// hundred milliseconds is interpreted as a button press. We emulate
// that by turning a Qwiic Single Relay ON (command byte 0x01), holding
// for `pulse_ms`, and turning it OFF (command byte 0x00). A separate
// physical relay is wired to each DKS input so OPEN and CLOSE can
// pulse independently and in either order.
//
// Both relays share the one I²C bus that i2c_bus.c brought up. They
// are distinguished by I²C address, which is configurable via NVS
// (relay_open_addr, relay_close_addr) and reassignable at runtime via
// the /i2c/relay-address HTTP endpoint without re-flashing.

#pragma once

#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    i2c_master_bus_handle_t bus;          // shared bus from i2c_bus_init_and_scan()
    uint8_t                 open_addr;    // e.g. 0x18
    uint8_t                 close_addr;   // e.g. 0x19
    uint32_t                pulse_ms;     // hold duration between ON and OFF
} relay_i2c_config_t;

// Attach device handles for the OPEN and CLOSE relays on `cfg->bus`
// and stash the pulse duration. Must be called exactly once, after
// i2c_bus_init_and_scan. Returns ESP_OK if both handles attach; logs
// and returns the first error otherwise. A missing relay at boot is
// not fatal — the pulse functions will surface transmit failures when
// called.
esp_err_t relay_i2c_init(const relay_i2c_config_t *cfg);

// Pulse the OPEN relay: write 0x01 (ON) → delay pulse_ms → write 0x00
// (OFF). Blocks the caller for ~pulse_ms. Returns ESP_OK on success,
// or a transmit error (typically ESP_ERR_TIMEOUT if the relay isn't
// on the bus at its configured address). On a failed ON transmit no
// OFF is sent; on a failed OFF transmit the relay may be left ON —
// the Qwiic Single Relay has no watchdog of its own, so a persistent
// I²C fault after the ON could wedge the DKS input closed until we
// reboot. In practice, transmits succeed atomically or fail fast, so
// this is a theoretical concern only.
esp_err_t relay_i2c_pulse_open(void);

// Pulse the CLOSE relay. See relay_i2c_pulse_open for semantics.
esp_err_t relay_i2c_pulse_close(void);

#ifdef __cplusplus
}
#endif
