# doorking-esp32

[![build](https://github.com/grantmd/doorking-esp32/actions/workflows/build.yml/badge.svg)](https://github.com/grantmd/doorking-esp32/actions/workflows/build.yml)

Open-source ESP-IDF firmware that bridges a **DoorKing 4602-010** slide-gate
controller to **Apple HomeKit** via an existing **Homebridge** instance. The
ESP32 mimics a wired button press to open and close the gate, reads the
4602-010's built-in "fully open" dry relay for state, and exposes everything
over WiFi as a bearer-authenticated HTTP API that Homebridge maps to a native
`GarageDoorOpener` accessory.

The firmware is **multi-target**: the same code runs on several ESP-IDF
development boards so you can pick whichever chip is in stock, in your budget,
or closest to the router. Board-specific pin assignments and Kconfig defaults
live in `main/board.h` and `sdkconfig.defaults.<target>`; every other module
(`gate_sm`, `config`, `wifi`, `reset_button`, the provisioning HTTP server) is
target-oblivious.

The firmware **never touches any UL 325 safety terminal** on the 4602-010 —
reversing edges, photo beams, entrapment alarm, inherent reverse sensor. It
only drives the same dry-contact command inputs the wired DKS keypad already
uses, and it reads the board's configurable status relay as a dry contact.

## Supported targets

| Target | Board | Chip | Flash | Notes |
|---|---|---|---|---|
| `esp32c3` | [Seeed Studio XIAO ESP32-C3](https://www.seeedstudio.com/Seeed-XIAO-ESP32C3-p-5431.html) | ESP32-C3 (RISC-V, 1 core) | 4 MB | Smallest and cheapest; native USB Serial/JTAG |
| `esp32` | [SparkFun Thing Plus ESP32 WROOM (USB-C)](https://www.sparkfun.com/products/20168) | ESP32-D0WDQ6 (Xtensa, 2 cores) | 16 MB | Feather-compatible footprint, Qwiic, LiPo charger |
| `esp32c5` | [SparkFun Thing Plus ESP32-C5](https://www.sparkfun.com/sparkfun-thing-plus-esp32-c5.html) | ESP32-C5 (RISC-V, 1 core) | 8 MB + 8 MB PSRAM | Dual-band WiFi 6 (2.4 + 5 GHz), Qwiic, newest chip |

All three boards share the same reference I/O topology: two **SparkFun Qwiic
Single Relay** modules (addressed at `0x18` and `0x19`) daisy-chained off the
board's I²C bus via Qwiic cables, plus one GPIO wired to the 4602-010 status
relay. No protoboard, no soldering, no hand-routed jumper wires inside the
gate-operator enclosure — just Qwiic cables and screw terminals.

## Status

Every push to `main` runs a matrix CI build for all three targets, so any
regression that breaks one target lands visibly before it can be merged.
Real-hardware verification has only happened on two of the three boards
so far — the third is waiting on shipping.

| Module | State |
|---|---|
| ESP-IDF scaffold, multi-target via `sdkconfig.defaults.<target>` | done |
| Per-target board pin map + I²C pins (`main/board.h`) — all three targets | done |
| Gate state machine (pure C, 19 host tests) | done |
| NVS-backed config (WiFi creds, bearer token, gate timings) | done |
| WiFi STA mode with AP-mode provisioning fallback | done |
| AP-mode provisioning web form + bearer token mint + reboot | done |
| Factory reset via BOOT-button hold (pure-C sm + 7 host tests) | done |
| HTTP API: `GET /health`, `GET /logs`, `GET /status`, `POST /open`, `POST /close` | done |
| HTTP API: `GET /i2c/scan`, `POST /i2c/relay-address` for bus diagnostics / one-shot device config | done |
| 16 KB RAM log ring buffer via `esp_log_set_vprintf` hook + `GET /logs` | done |
| mDNS `<hostname>.local` (default `doorking.local`) | done |
| I²C master bus init + boot-time device scan | done |
| Qwiic Single Relay driver, wired into `/open` + `/close` | done |
| WS2812 RGB status LED (WiFi state + OTA progress) | done |
| OTA updates — push (`POST /update`) + pull (GitHub Releases auto-check) | done |
| Rollback protection (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` + 120 s WiFi watchdog) | done |
| Dashboard at `GET /` with version, update, check + reboot controls | done |
| `POST /reboot` remote restart endpoint | done |
| `POST /update/check` on-demand GitHub release check | done |
| `esp32c3` build-verified and hardware-verified (XIAO) | done |
| `esp32` build-verified and hardware-verified (Thing Plus WROOM, both relays) | done |
| `esp32c5` build-verified, partially hardware-verified (WiFi + provisioning OK; Qwiic I²C needs LP\_I²C driver — deferred) | in progress |
| Size-optimised `-Os` build + `TWO_OTA_LARGE` partition (1700 KB slots) | done |
| `scripts/idf.sh` wrapper for sandbox-friendly invocations | done |
| GitHub Actions matrix CI (3 targets) + tag-triggered releases with OTA assets | done |
| 4602-010 status GPIO (pin 15–16 "fully open" sense) wiring | pending |
| Homebridge plugin config | pending |

Current binary sizes (ESP-IDF v5.5.4, `-Os`, `TWO_OTA_LARGE` 1700 KB slots):

| Target | Binary | App partition free |
|---|---:|---:|
| `esp32c3` | ~1026 KB | ~41 % |
| `esp32` | ~1062 KB | ~39 % |
| `esp32c5` | ~1100 KB | ~35 % |

Host unit tests (26 scenarios across `gate_sm` and `reset_btn_sm`) run in
a fraction of a second with no ESP-IDF toolchain required:

```
./test/run_tests.sh
```

## Hardware

| Qty | Part | Notes |
|---|---|---|
| 1 | Any supported dev board | See "Supported targets" above |
| 2 | [SparkFun Qwiic Single Relay (COM-15093)](https://www.sparkfun.com/products/15093) | Both ship at `0x18`. Reassign one to `0x19` at runtime via `POST /i2c/relay-address` (see "Reassigning relay addresses" below) — the board has no physical jumper for this, only a solder pad. Omron G6K-2F-Y relay, 5.5 A @ 240 VAC — overkill for 24 V DC dry-contact switching, which is what we want |
| 1–2 | [Qwiic cable](https://www.sparkfun.com/products/14426) | One daisy-chains the two relays; if your dev board has a Qwiic jack, a second runs from the board to the first relay. For the XIAO ESP32-C3 (no Qwiic jack), a [Qwiic-to-pigtail adapter](https://www.sparkfun.com/products/14425) bridges to the D4/D5 I²C pins |
| 1 | 5 V USB power brick | Powers the dev board from the 115 VAC convenience outlet inside the DKS operator enclosure |
| 1 | USB-C data cable | Flashing, console, and power |
| — | 18–22 AWG hookup wire | From each relay's screw terminals to the DKS 4602-010 terminal strip |

### Wiring to the DKS 4602-010 main terminal

Confirmed against the 9150 Installation & Owner's Manual (document
`9150-065-M-5-18`), Section 5.1 Main Terminal Description. The relay wiring
is **target-independent** — it goes to the same DKS pins regardless of which
dev board you picked.

| From | DKS pins | DKS function |
|---|---|---|
| Relay `0x18` COM + NO | **4** ↔ **1** (Low Voltage Common) | Full Open — momentary short to common |
| Relay `0x19` COM + NO | **9** ↔ **18** (Low Voltage Common) | 3-Button Close — momentary short to common |
| Dev-board GPIO input | **15** ↔ **18** | 4602-010 configurable dry relay, set to "fully open" via SW1 switches 4=OFF 5=OFF |

**DIP switch setup on the 4602-010**: set SW1 switch 4 = OFF and switch 5 =
OFF so the onboard dry relay energises when the gate is fully open. Verify
the relay shorting bar is on the **NO** pins. The firmware reads: contact
closed → GPIO pulled to common → status = `OPEN`.

**Safety rules** (non-negotiable):
- Do not touch any UL 325 terminals (pins 1–10 on the UL 325 strip, photo
  beams, reversing edges, entrapment alarm).
- Do not disable or bypass the inherent reverse sensor, clutch, or
  entrapment alarm.
- Mount the dev board and relays inside the operator enclosure or in an
  adjacent weatherproof enclosure.

### Reassigning relay addresses

Both Qwiic Single Relays leave the factory at I²C address `0x18`, and the
firmware expects one at `0x18` (OPEN) and one at `0x19` (CLOSE). Reassign
one of them at runtime, with only that relay plugged into the Qwiic bus:

```
# confirm the bus state
curl -s -H "Authorization: Bearer <token>" http://doorking.local/i2c/scan

# move the plugged-in relay to 0x19
curl -s -X POST -H "Authorization: Bearer <token>" \
  "http://doorking.local/i2c/relay-address?from=0x18&to=0x19"
```

The endpoint writes SparkFun's `SINGLE_CHANGE_ADDRESS` command (register
`0x03`, payload = new address), which the relay persists to on-board
EEPROM. Then plug in the second relay — it comes up at the default `0x18`
and the two coexist. The relay addresses the firmware drives are
NVS-backed (`relay_open_addr`, `relay_close_addr`) so you can flip the
convention without re-flashing.

## Software prerequisites (macOS)

The firmware requires **ESP-IDF v5.5** or newer — v5.5 is the first release
with stable ESP32-C5 support. One-time setup, cloning into a versioned
directory so future IDF upgrades can live side-by-side:

```
brew install cmake ninja dfu-util python3
mkdir -p ~/esp && cd ~/esp
git clone -b release/v5.5 --recursive https://github.com/espressif/esp-idf.git esp-idf-v5.5
cd ~/esp/esp-idf-v5.5 && ./install.sh esp32,esp32c3,esp32c5
```

Passing the comma-separated target list to `install.sh` installs the
toolchains for all three supported targets in one pass. If you only want
one, pass just that name.

The `scripts/idf.sh` wrapper (see next section) sources
`~/esp/esp-idf-v5.5/export.sh` by default, so no manual `export.sh`
sourcing is required for normal use. If you've installed ESP-IDF to a
different path, override via the `IDF_EXPORT` env var:

```
IDF_EXPORT=~/esp/my-custom-idf/export.sh ./scripts/idf.sh build
```

## Build and flash

The repo ships a thin wrapper at `scripts/idf.sh` that sources ESP-IDF's
`export.sh` internally and forwards all arguments to `idf.py`. Using the
wrapper means builds work without pre-activating the toolchain in the
current shell, and it gives automated agents / sandboxes a concrete
namespaced command prefix (`./scripts/idf.sh *`) to whitelist instead of
the leading `.` POSIX-source operator. From the repo root:

```
./scripts/idf.sh set-target <target>   # esp32c3, esp32, or esp32c5 — once per clone per target
./scripts/idf.sh build
./scripts/idf.sh -p <serial port> flash monitor
```

When switching between targets on the same clone, run
`./scripts/idf.sh fullclean` first so CMake regenerates `sdkconfig` from
scratch against the new target's `sdkconfig.defaults.<target>`.

If you'd rather activate the toolchain yourself and call `idf.py`
directly, the canonical pattern still works:

```
. ~/esp/esp-idf-v5.5/export.sh
idf.py set-target <target>
idf.py build
idf.py -p <serial port> flash monitor
```

Optional `~/.zshrc` alias for that flow:

```
alias get_idf='. ~/esp/esp-idf-v5.5/export.sh'
```

Serial port patterns vary by board:

| Board | Port pattern | Why |
|---|---|---|
| Seeed XIAO ESP32-C3 | `/dev/cu.usbmodem*` | Native USB Serial/JTAG |
| SparkFun Thing Plus ESP32 WROOM | `/dev/cu.usbserial-*` | CH340C USB-UART bridge |
| SparkFun Thing Plus ESP32-C5 | `/dev/cu.usbmodem*` | Native USB Serial/JTAG |

If the glob doesn't match, plug the board in and run `ls /dev/cu.*` to find
the right path. Exit the serial monitor with **Ctrl+]**.

## First-boot provisioning

The firmware has two WiFi modes, chosen at boot from what's in NVS:

- **No WiFi credentials** — comes up in AP mode broadcasting an open
  `doorking-setup` SSID and serves a one-page provisioning form.
- **WiFi credentials set** — comes up in STA mode, auto-reconnects on
  disconnect.

To provision (or re-provision) a device:

1. Flash the firmware.
2. Join the open `doorking-setup` WiFi network from a phone or laptop.
3. Open `http://192.168.4.1/` in a browser. You should see a "DoorKing setup"
   form.
4. Enter your real home WiFi SSID and password. Submit.
5. The success page shows a 64-character hex bearer token. **Copy it
   immediately.** This token is the only way Homebridge will be able to
   authenticate to the gate bridge. Paste it somewhere safe (password
   manager).
6. The device reboots after 3 seconds into STA mode. Check the serial
   monitor for the assigned IP address and a backup copy of the bearer
   token:

   ```
   I (xxx) wifi:   sta got ip 192.168.1.42
   I (xxx) config: auth_token=<64 hex chars>
   ```

Even if the browser success page fails to render (iOS captive-portal view
is fragile in this specific scenario), the token is always logged to serial
during provisioning and on every subsequent boot as a backup delivery path.

## Factory reset — press and hold BOOT

Every supported board has a BOOT button wired to a strapping pin. Press and
hold for 5 seconds **while the firmware is running** to clear `wifi_ssid`,
`wifi_psk`, and `auth_token` from NVS and reboot into AP provisioning mode.
Gate timings and hostname are preserved. Serial logs look like:

```
W (xxx) reset_btn: button pressed — keep holding for 5 seconds to erase wifi creds
W (xxx) reset_btn: hold threshold crossed — clearing wifi credentials and rebooting
W (xxx) config:    wifi credentials and auth token cleared
```

> The BOOT button doubles as the chip's bootloader strapping pin on every
> supported target. Holding BOOT *during* power-on drops the chip into the
> ROM download mode instead of running our firmware, so the reset path is
> intentionally runtime-only: press and hold while the device is already
> up and logging. This is documented in `main/reset_button.c`.

If all else fails, erase flash over USB and re-flash:

```
./scripts/idf.sh -p <serial port> erase-flash
./scripts/idf.sh -p <serial port> flash monitor
```

## OTA updates

The firmware supports two update paths. Both use ESP-IDF's dual OTA
partitions — the new image is written to the inactive slot, then the
bootloader swaps on reboot with automatic rollback if the new firmware
fails to connect to WiFi within 120 seconds.

### Pull (automatic)

The device checks the GitHub Releases API every 6 hours (configurable via
NVS `ota_intv`). When a new release is found, the version is cached and
shown on the dashboard at `http://doorking.local/`. Click **Update
firmware** on the dashboard to download and install, or set `ota_install`
to `1` in NVS for fully hands-off updates.

### Push (manual)

Upload a firmware binary directly from the LAN:

```
curl -X POST http://doorking.local/update \
  -H "Authorization: Bearer <token>" \
  -H "Content-Type: application/octet-stream" \
  --data-binary @build/doorking.bin
```

The device validates the image (including chip-ID check — you can't flash
an `esp32c3` binary onto an `esp32` board) and reboots on success.

### Dashboard

Visit `http://doorking.local/` for a lightweight status page showing the
current firmware version, target chip, and available update. The page
also has **Check for updates** and **Reboot** buttons (both require the
bearer token).

### Rollback

If a freshly-OTA'd firmware fails to get an IP address within 120 seconds,
the device reboots and the bootloader automatically rolls back to the
previous working firmware. USB reflash is always available as a last
resort.

## 3D-printable baseplate

The `enclosure/` directory contains OpenSCAD models for mounting baseplates
that hold a dev board and two Qwiic Single Relay modules. Two variants:

- **`baseplate-thingplus.scad`** — for the SparkFun Thing Plus (WROOM or
  C5). The board mounts on two standoffs at the USB end and two solid
  support posts at the radio end, with USB-C facing outward. The two
  relays sit side by side below on four standoffs each.
- **`baseplate-xiao.scad`** — for the Seeed XIAO ESP32-C3. The XIAO has
  no mounting holes, so it sits in a friction-fit clip holder with a
  USB-C cutout.

Both plates include mounting tabs with screw holes for attaching to the
gate operator enclosure wall, and engraved "ESP32"/"XIAO", "OPEN", and
"CLOSE" labels.

Board dimensions were extracted from the SparkFun Eagle/KiCad design files
and Seeed documentation. Shared parameters (standoff height, screw hole
diameter, board clearances) live in `baseplate-common.scad` — adjust there
if your printer tolerances differ.

Print flat side down, no supports, 0.2mm layer height. Use M3 or 4-40
self-tapping screws for the standoffs.

## Repo layout

```
.
├── CMakeLists.txt               # top-level ESP-IDF project file
├── sdkconfig.defaults           # common defaults (all targets): partition layout,
│                                #   FreeRTOS tick, httpd buffers, -Os optimisation
├── sdkconfig.defaults.esp32c3   # C3-specific: USB-JTAG console, 4 MB flash
├── sdkconfig.defaults.esp32     # WROOM-specific: 16 MB flash, UART0 console
├── sdkconfig.defaults.esp32c5   # C5-specific: 8 MB flash, USB-JTAG console, 802.15.4 off
├── main/                        # firmware component
│   ├── CMakeLists.txt
│   ├── idf_component.yml        # IDF Component Registry deps (mdns)
│   ├── main.c                   # app_main — boots modules in order
│   ├── board.h                  # per-target pin map + I²C pins + board name
│   ├── gate_sm.{c,h}            # pure-C gate state machine (no ESP-IDF deps)
│   ├── config.{c,h}             # NVS-backed persistent settings
│   ├── wifi.{c,h}               # STA + AP-provisioning + provisioning HTTP server
│   ├── http_api.{c,h}           # REST API + dashboard (/ /health /logs /status /open /close
│   │                            #   /update /update/check /update/pull /reboot
│   │                            #   /i2c/scan /i2c/relay-address)
│   ├── ota.{c,h}                # OTA: push upload, GitHub pull check, rollback confirmation
│   ├── log_buffer.{c,h}         # 16 KB RAM ring buffer, tees ESP_LOGx to buffer + UART
│   ├── i2c_bus.{c,h}            # I²C master init + boot-time scan + address-change helper
│   ├── relay_i2c.{c,h}          # Qwiic Single Relay driver (pulse_open / pulse_close)
│   ├── status_led.{c,h}         # WS2812 RGB status LED (WiFi state, OTA progress)
│   ├── reset_btn_sm.{c,h}       # pure-C debounce + hold-threshold state machine
│   └── reset_button.{c,h}       # FreeRTOS task: poll BOOT pin, clear wifi on hold
├── enclosure/                   # 3D-printable mounting hardware (OpenSCAD)
│   ├── baseplate-common.scad    # shared dimensions, standoff/holder modules
│   ├── baseplate-thingplus.scad # baseplate for Thing Plus WROOM or C5 + 2 relays
│   └── baseplate-xiao.scad     # baseplate for XIAO ESP32-C3 + 2 relays
├── scripts/
│   └── idf.sh                   # thin wrapper: sources export.sh, forwards to idf.py
├── test/
│   ├── run_tests.sh             # host-side test runner, uses system cc
│   ├── test_gate_sm.c           # 19 gate_sm scenarios
│   └── test_reset_btn_sm.c      # 7 debounce / hold-threshold scenarios
└── .github/
    └── workflows/
        └── build.yml            # host tests + 3-target matrix build + release on tag
```

## Adding a new target

If you want to port this firmware to another ESP-IDF-supported board, the
friction is intentionally low:

1. Create `sdkconfig.defaults.<target>` with any target-specific Kconfig
   options (flash size, console routing, USB mode).
2. Add a `#if CONFIG_IDF_TARGET_<TARGET>` block in `main/board.h` that
   defines at minimum `BOARD_NAME` and `RESET_BUTTON_GPIO`.
3. Add the target to the CI matrix in `.github/workflows/build.yml`.

No other files need to change — the `gate_sm`, `config`, `wifi`, and
`reset_button` modules are all target-independent.

## References

- **DoorKing 9150 Installation & Owner's Manual** (covers circuit board
  4602-010 Rev AA+):
  https://www.doorking.com/wp-content/uploads/2018/05/9150-065-M-5-18.pdf
  The Quick Guide pages 1–2 and Section 5.1 are the terminal-level source
  of truth for this project.
- **Seeed Studio XIAO ESP32-C3 wiki**:
  https://wiki.seeedstudio.com/XIAO_ESP32C3_Getting_Started/
- **SparkFun Thing Plus ESP32 WROOM (USB-C)**:
  https://www.sparkfun.com/products/20168
- **SparkFun Thing Plus ESP32-C5**:
  https://www.sparkfun.com/sparkfun-thing-plus-esp32-c5.html
- **SparkFun Qwiic Single Relay Hookup Guide**:
  https://learn.sparkfun.com/tutorials/qwiic-single-relay-hookup-guide/all
- **ESP-IDF Programming Guide** (pin `release/v5.5` or newer):
  https://docs.espressif.com/projects/esp-idf/en/release-v5.5/
- **ESP-IDF HTTP Server API**:
  https://docs.espressif.com/projects/esp-idf/en/release-v5.5/esp32/api-reference/protocols/esp_http_server.html
