# sensor-array-mini temperature-test firmware

Firmware for temperature testing the 30 × 30 mm mini sensor board: a Microchip
WBZ451PE-I (PIC32CX1012BZ25048, BLE 5.2) reading sixteen ST LSM6DSOX IMUs on
one SPI bus.

This is a variant of the sensor-array-mini firmware for a board sitting in an
oven, where nobody can tap it. There is no double tap: the host sends `CAP` and
the board captures as soon as its gyroscope says it is still. A `TEMP` command
returns the sixteen die temperatures, so the host can wait for the oven to
settle before taking reads. Everything else (BLE, pairing, the capture sequence,
the frame format) is identical to the tap firmware.

To go back to normal use, flash the sensor-array-mini firmware.

The only files that differ from the tap firmware are
`firmware/src/app/sensor_app.c`, `firmware/src/app/sensor_app.h` and
`host/arm_and_capture.py`.

## Layout

```
firmware/
    mini_sensor_temptest.X/   MPLAB X project. Open this one.
    src/
        app/                  application: state machine, frame encoder, BLE seam
        sensor/               LSM6DSOX driver and board pin map
        app_ble/              Harmony BLE event handlers (patched, see docs)
        app.c, main.c, ...    Harmony application skeleton (patched app.c)
        config/               MCC-generated Harmony config, BLE stack library
        packs/                device family pack and CMSIS headers
        third_party/          FreeRTOS, wolfSSL
host/
    arm_and_capture.py        bench tool: pair, CAP, decode one frame; --temp for temperatures
    frame_codec.py            decoder for the frame line
docs/
    FIRMWARE_GUIDE.md         full description: capture sequence, registers, protocol
    BLE_INTEGRATION.md        how the app was grafted onto the Harmony BLE example
    FIRMWARE_CHANGES.md       change history with the reasons
    PINMAP.md                 pin map traced from the board
    16_Sensor_Board_Stage2.pdf  schematic
mplab_harmony_license.md      licence for the Microchip Harmony code under firmware/src
```

The files we wrote are `firmware/src/app/*` and `firmware/src/sensor/*`.
Microchip's example code was patched in five files: `src/app.c`; under
`src/app_ble/`, `app_ble.c`, `app_ble_handler.c` and `app_trsps_handler.c`;
and `src/config/default/ble/service_ble/ble_trs/ble_trs.c`, which sets the
encryption permission bits. Everything else under `firmware/src` is generated
by MCC or comes from Harmony.

### Paths in the docs

The docs were written in the original working tree, so they use its paths:

| docs say | in this repo |
|---|---|
| `mini_sensor/firmware/src/app`, `.../src/sensor` | `firmware/src/app`, `firmware/src/sensor` |
| `ble_upstream/.../peripheral_trp_uart/firmware/...` | `firmware/...` (project folder renamed `mini_sensor_temptest.X`) |
| `mini_sensor/host/` | `host/` |
| `firmware/ble_patches/` | not needed: the patched files are already in `firmware/src` |

The warning in the docs about "the build tree keeps its own copies" does not
apply here. This repo has only one copy, and it is the one MPLAB builds.

The docs describe the tap firmware. Their sections on the tap, `ARM` and the
state machine do not apply to this image; the protocol below replaces them.

## Build and flash

Toolchain the image was built with: MPLAB X v6.30, MCC Harmony v5.7.0,
XC32 v6.00, MPLAB Snap over SWD.

The project asks for these packs: `PIC32CX-BZ_DFP` 1.4.243 and ARM `CMSIS`
5.8.0.

1. Install MPLAB X and XC32 v6.00.
2. MPLAB X → Open Project → `firmware/mini_sensor_temptest.X`.
3. If MPLAB reports missing packs, install them from its prompt (or Tools →
   Packs).
4. Connect the MPLAB Snap to the SWD pads, then Clean and Build, then Make and
   Program Device.

After every flash the BLE bonds are erased, so each paired machine has to pair
again. On Linux: `bluetoothctl remove <address>`. The board's address and name
(`HES_xxxxxxxx`) come from the silicon, so they do not change.

## Protocol

Host to board, ASCII lines ending in `\n`:

| command | reply |
|---|---|
| `CAP` | `MS1-CAP`, then one `MS1 ...` frame line. `MS1-BUSY` if a capture is in progress. |
| `TEMP` | `MS1-TEMP <present> <t0> ... <t15>`, each a 4-hex-digit int16, degC = v/256 + 25. An absent sensor reads `8000`. `MS1-BUSY` if a capture is in progress. |
| `PING` | `MS1-PONG TT1`. The tap firmware answers plain `MS1-PONG`, so this tells the two apart. |
| `STATUS` | `MS1-STAT <state> <present mask> <capture count>` |
| anything else, including `ARM` | `MS1-ERR` |

`CAP` waits for the gyroscope on sensor 0 to read under 5 deg/s for 100 ms (up
to 3 s), the same check the tap firmware runs after a tap. The frame's flag is
`S` if it passed, `M` if it timed out. The frame format is specified at the top
of `firmware/src/app/frame.h`; the `tapped` field is always `0000`.

`TEMP` runs a full capture internally, because the sensors are powered down
between captures and their temperature registers would otherwise hold the last
capture's values. It does not count towards the capture count.

## Bench check

```
pip install bleak
python3 host/arm_and_capture.py           # pair, CAP, print the frame
python3 host/arm_and_capture.py --temp    # the sixteen die temperatures
python3 host/arm_and_capture.py --list    # everything advertising, unfiltered
```
