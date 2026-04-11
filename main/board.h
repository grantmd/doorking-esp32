// Per-target board pin map and board identity.
//
// Each supported dev board gets a #if CONFIG_IDF_TARGET_* block with the
// minimal set of #defines the rest of the firmware consumes. New targets
// are added by:
//
//   1. Creating sdkconfig.defaults.<target> with target-specific options
//      (flash size, console routing).
//   2. Adding a #if block here with at minimum BOARD_NAME and
//      RESET_BUTTON_GPIO.
//   3. Adding the target to the CI matrix in
//      .github/workflows/build.yml.
//
// What belongs here: anything board-specific and known at compile time.
// Today that is the human-readable board name and the BOOT-button GPIO.
// When the Qwiic / I2C relay layer lands this file will also gain
// I2C_MASTER_SDA_GPIO and I2C_MASTER_SCL_GPIO; when the gate-status
// sensor input is wired up it will gain STATUS_INPUT_GPIO.
//
// What does NOT belong here: anything the user configures at runtime via
// NVS (WiFi creds, auth token, gate timings) and anything target-
// independent (partition layout, FreeRTOS tick rate, HTTP buffer sizes).
//
// GPIO values are plain integers (not gpio_num_t) so this header has no
// ESP-IDF dependency and could, in principle, be consumed by host-side
// tests. Call sites pass them directly to the ESP-IDF GPIO API, which
// accepts implicit integer-to-enum conversion for pin arguments.

#pragma once

#include "sdkconfig.h"

#if CONFIG_IDF_TARGET_ESP32C3

    #define BOARD_NAME          "Seeed Studio XIAO ESP32-C3"

    // The BOOT button on the XIAO. GPIO9 is a strapping pin — holding
    // it LOW during power-on enters the ROM download mode instead of
    // running our firmware. See reset_button.c for why this only
    // supports "press and hold while the device is already running"
    // resets and not "hold at power-on" resets.
    #define RESET_BUTTON_GPIO   9

#elif CONFIG_IDF_TARGET_ESP32

    #define BOARD_NAME          "SparkFun Thing Plus ESP32 WROOM (USB-C)"

    // The BOOT button on the Thing Plus, wired to GPIO0. Like GPIO9 on
    // the C3, GPIO0 is a strapping pin — holding it LOW during power-on
    // drops the chip into the ROM download mode instead of running our
    // firmware. The reset-button hold logic therefore only runs after
    // the firmware is already up.
    //
    // GOTCHA specific to this board: the Thing Plus wires GPIO0 so that
    // pressing the BOOT button also momentarily disables the Qwiic
    // connector's power rail. That means our 5-second factory-reset
    // hold will drop Qwiic power for the duration of the hold, which in
    // turn briefly un-powers any Qwiic I2C devices we're driving (like
    // the Qwiic Relays). This is acceptable because the user is
    // deliberately resetting the device and we reboot immediately
    // after; relay state is re-established from scratch on the next
    // boot. Do not use GPIO0 as a runtime-polled input for anything
    // other than the reset button.
    #define RESET_BUTTON_GPIO   0

#else

    #error "Unsupported IDF target — add a block in main/board.h"

#endif
