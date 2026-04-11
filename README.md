# doorking-esp32

An ESP32-C3 bridge for a **DoorKing 4602-010** slide-gate controller. The ESP32
mimics a wired button press to open and close the gate, reads the
controller's built-in "fully open" dry relay for state, and exposes everything
over WiFi as a bearer-authenticated HTTP API. The API is designed to plug into
an existing Homebridge instance via a generic HTTP garage-door plugin so the
gate appears as a native HomeKit `GarageDoorOpener` accessory.

The ESP32 never touches any UL 325 safety terminal (reversing edges, photo
beams, entrapment alarm, inherent reverse sensor). It only drives the same
dry-contact command inputs the wired DKS keypad already uses.

Full design: [`plans/smooth-fluttering-bird.md`](../../.claude/plans/smooth-fluttering-bird.md)
in the user's Claude config — copy if you want it committed in-repo.

## Status

| Module | State | File(s) |
|---|---|---|
| ESP-IDF scaffold, builds cleanly for esp32c3 | done | `CMakeLists.txt`, `sdkconfig.defaults`, `main/CMakeLists.txt` |
| Gate state machine (pure C, host-testable) | done | `main/gate_sm.{c,h}`, `test/test_gate_sm.c` |
| NVS-backed config (WiFi, token, timings) | done | `main/config.{c,h}` |
| WiFi STA + AP-mode provisioning fallback | done | `main/wifi.{c,h}` |
| HTTP API (`/status`, `/open`, `/close`, bearer auth) | pending | — |
| Embedded web UI | pending | — |
| GPIO wiring (relay module, status input) | pending | — |
| mDNS `doorking.local` | pending | — |
| OTA updates | pending | — |
| Homebridge plugin config | pending | — |

Host unit tests (19 scenarios across the gate state machine) run in a fraction
of a second with no ESP-IDF toolchain required:

```
./test/run_tests.sh
```

## Hardware

| Qty | Part | Notes |
|---|---|---|
| 1 | Seeed Studio XIAO ESP32-C3 | 4 MB flash, native USB-C, runs off 5 V |
| 1 | 2-channel 3.3 V relay module (HW-383 style, SRD-03VDC-SL-C) | Pre-built board with screw terminals, optocoupler-isolated inputs |
| 1 | 5 V USB wall brick | Powers the XIAO from the 115 VAC convenience outlet inside the DKS operator enclosure |
| 1 | USB-C data cable | Flashing, console, and power |
| — | Male-to-female jumper wires | XIAO ↔ relay module during bench work |
| — | 18–22 AWG hookup wire | Relay module ↔ DKS 4602-010 terminal strip during field install |

Purchase links and alternatives: see the conversation history that produced
this repo, or search Amazon for *"2 channel 3.3V relay module optocoupler"*.

### Planned wiring to the DKS 4602-010 main terminal

Confirmed against the 9150 Installation & Owner's Manual (document
`9150-065-M-5-18`), Section 5.1 Main Terminal Description:

| ESP32 GPIO | Direction | Relay channel | DKS pin | DKS function |
|---|---|---|---|---|
| `GPIO_OPEN` (TBD) | OUT | relay #1 contacts | **4** ↔ **1** | Full Open — momentary short to common |
| `GPIO_CLOSE` (TBD) | OUT | relay #2 contacts | **9** ↔ **18** | 3-Button Close — momentary short to common |
| `GPIO_STATUS` (TBD) | IN, internal pull-up | direct (dry contact, no SSR) | **15** ↔ **18** | Configurable dry relay, set to "fully open" via SW1 switches 4=OFF 5=OFF |

Exact GPIO assignments land when the hardware layer is implemented.

## Software prerequisites (macOS)

The firmware targets ESP-IDF v5.3+. One-time setup:

```
brew install cmake ninja dfu-util python3
mkdir -p ~/esp && cd ~/esp
git clone -b release/v5.3 --recursive https://github.com/espressif/esp-idf.git
cd ~/esp/esp-idf && ./install.sh esp32c3
```

Activate the toolchain in any shell that needs it:

```
. ~/esp/esp-idf/export.sh
```

Optional `~/.zshrc` alias:

```
alias get_idf='. ~/esp/esp-idf/export.sh'
```

## Build and flash

From the repo root with the toolchain activated:

```
idf.py set-target esp32c3   # only needed once per clone
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor
```

`/dev/cu.usbmodem*` expands to the XIAO's native USB Serial/JTAG device. If
the glob doesn't match, plug the board in and run `ls /dev/cu.*` to find the
right path.

Exit the serial monitor with **Ctrl+]**.

## First-boot provisioning

The device has two modes, chosen at boot from what's in NVS:

- **No WiFi credentials** — comes up in AP mode broadcasting an open
  `doorking-setup` SSID and serves a one-page provisioning form.
- **WiFi credentials set** — comes up in STA mode, auto-reconnects on
  disconnect.

To provision (or re-provision) a device:

1. Flash the firmware (see above).
2. On your phone, join the open `doorking-setup` WiFi network.
3. Open `http://192.168.4.1/` in a browser. You should see a small blue
   "DoorKing setup" form.
4. Enter your real home WiFi SSID and password. Tap **Save & reboot**.
5. The success page shows a 64-character bearer token. **Copy it
   immediately.** This token is the only way Homebridge will be able to
   authenticate to the gate bridge, and it is never displayed again without
   re-provisioning. Paste it somewhere safe (password manager).
6. The device reboots after 2 seconds into STA mode and joins your home
   network. Check the serial monitor for the assigned IP address:

   ```
   I (xxx) wifi: sta got ip 192.168.1.42
   ```

To re-provision later (e.g. new WiFi password, lost token, moved house),
easiest path today is to erase flash and re-flash:

```
idf.py -p /dev/cu.usbmodem* erase-flash
idf.py -p /dev/cu.usbmodem* flash monitor
```

A proper "factory reset" button handler is planned but not yet wired up.

## Repo layout

```
.
├── CMakeLists.txt          # top-level ESP-IDF project file
├── sdkconfig.defaults      # target esp32c3, 4 MB flash, USB-JTAG console, two-OTA
├── main/                   # firmware component
│   ├── CMakeLists.txt
│   ├── main.c              # app_main — boots modules in order
│   ├── gate_sm.{c,h}       # pure-C gate state machine (no ESP-IDF deps)
│   ├── config.{c,h}        # NVS-backed persistent settings
│   └── wifi.{c,h}          # STA + AP-provisioning mode + provisioning HTTP server
└── test/
    ├── run_tests.sh        # host-side test runner, uses system cc
    └── test_gate_sm.c      # 19 gate_sm scenarios
```

## References

- **DoorKing 9150 Installation & Owner's Manual**
  (covers circuit board 4602-010 Rev AA+):
  https://www.doorking.com/wp-content/uploads/2018/05/9150-065-M-5-18.pdf
  The Quick Guide pages 1–2 and Section 5.1 are the terminal-level source of
  truth for this project.
- **Seeed Studio XIAO ESP32-C3 wiki**:
  https://wiki.seeedstudio.com/XIAO_ESP32C3_Getting_Started/
- **ESP-IDF Programming Guide — macOS/Linux setup**:
  https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/get-started/linux-macos-setup.html
- **ESP-IDF HTTP Server API**:
  https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-reference/protocols/esp_http_server.html
