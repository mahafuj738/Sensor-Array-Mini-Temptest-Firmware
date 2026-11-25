---
title: "Mini Sensor Board — Firmware Guide"
subtitle: "WBZ451PE-I with sixteen LSM6DSOX accelerometers"
date: "2026-10-06"
geometry: margin=2.4cm
fontsize: 11pt
colorlinks: true
---

# 1. What the board is

| | |
|---|---|
| MCU / radio | Microchip **WBZ451PE-I** module (PIC32CX1012BZ25048, Cortex-M4F + BLE 5.2) |
| Sensors | **sixteen** ST **LSM6DSOX** 6-axis IMUs on one SPI bus |
| Battery | Li-ion 3.7 V 150 mAh, charged by an MCP73831 over USB-C |
| Supply | AP2112K-3.3 LDO — the module runs at a fixed 3.3 V |
| Interaction | one **double tap** on the board; no usable button on rev 1.0 |
| Link | BLE only. There is no USB data path |

The board has **no serial port and no working button**. `User_BTN` is shorted
to ground on rev 1.0 — all four switch legs read 0 V and beep continuity to
ground — so every interaction is either a tap or a BLE command.

**Why sixteen sensors.** The key comes from the *differences between* the
sixteen parts. Each LSM6DSOX leaves the factory with its own residual offset
(±20 mg, datasheet `LA_TyOff`) and gain (±1 %, `G_So%`) after trimming. Those
residuals are stable per board and unpredictable across boards, which is what
makes the board identifiable. A perfectly calibrated array would have no key.

---

# 2. Source layout

```
mini_sensor/firmware/src/
    sensor/
        board_pins.h      pin map: SPI, 16 chip selects, LED
        lsm6dsox.h/.c     the driver. SPI, registers, capture, tap
    app/
        sensor_app.h/.c   the state machine. commands, tap, framing
        sensor_task.h/.c  the FreeRTOS task that steps it
        frame.h/.c         the wire format
        ble_link.h/.c      the only file that includes Harmony BLE headers
    bringup/
        bringup.c          step 1-2 hardware proving (not in the BLE build)
        capture.c          step 3 standalone capture (not in the BLE build)
```

Two things to know before editing:

1. **The MPLAB project compiles a *copy*.** `configurations.xml` references
   `../src/app/...` relative to the `.X` directory, which resolves **inside
   `ble_upstream/`**. Editing only the tracked tree builds the stale file.
   Copy into `ble_upstream/.../firmware/src/` and into
   `firmware/ble_patches/`, then confirm with `md5sum`.
2. **Adding a new source file needs the IDE.** Registering it by editing
   `configurations.xml` by hand does nothing, because the build runs from a
   generated `Makefile-default.mk` that is only rebuilt when the IDE notices
   the project changed. Put new code in an existing file, or add the file
   through MPLAB.

---

# 3. Boot sequence

```
main()
  SYS_Initialize()
  APP_Tasks()                     FreeRTOS task, priority 1
    APP_STATE_INIT
      APP_BleStackInit()
        APP_BleConfigAdvance()    SMP, DM, GAP service options
        APP_BleConfigBasic()      TX power, advertising params,
                                  then ble_identity_configure()
      BLE_GAP_SetAdvEnable(1, 0)  start advertising
      sensor_task_start()         our task, priority 1, 512-word stack
    APP_STATE_SERVICE_TASKS
      OSAL_QUEUE_Receive(..., OSAL_WAIT_FOREVER)
```

`APP_Tasks()` blocks forever on its queue and wakes only for stack events, so
**nothing of ours can be driven from it**. That is why `sensor_task.c` exists:
a separate FreeRTOS task that calls `sensor_app_step()` every 10 ms.

```c
#define SENSOR_APP_TICK_MS  10u
vTaskDelay(pdMS_TO_TICKS(SENSOR_APP_TICK_MS));
```

`sensor_app_step()` is a pure step function — it never sleeps and never
blocks. All pacing lives in the task. That separation is what lets the same
logic be unit-reasoned without a scheduler.

---

# 4. The BLE side

## 4.1 Identity — one image, every board different

`ble_identity_configure()` in `app/ble_link.c`, called from
`APP_BleConfigBasic` in place of the example's hard-coded address:

1. ask the stack what address it has — `BLE_GAP_GetDeviceAddr()`
2. **keep it** if it is neither all-zero nor all-ones
3. otherwise fall back to `A6:A5:A4:A3:A2:A1`
4. advertise as `ms1-<last two address bytes>` and set the same string as the
   GAP device name

Microchip's `peripheral_trp_uart` example overwrites the factory address with
`A6:A5:A4:A3:A2:A1`, so **every board built from it is the same Bluetooth
device**. That is survivable with one board and not with two: Windows and
BlueZ both key the long-term key by address, so pairing a second board either
overwrites the first board's bond or hands the first board's key to the second.

Confirmed on hardware — the WBZ451 *does* carry a factory address:

| address | advertised name |
|---|---|
| `9C:95:6E:4D:43:BE` | `ms1-43be` |
| `9C:95:6E:4D:3C:65` | `ms1-3c65` |
| `9C:95:6E:4D:24:8A` | `ms1-248a` |

All share Microchip's `9C:95:6E` OUI. **One image serves any number of
boards** — no per-board build, nothing stored.

## 4.2 Advertising

```c
adv[0..2] = 0x02 0x01 0x06     flags: general discoverable, BR/EDR unsupported
adv[3..]  = len 0x09 "ms1-xxxx"  complete local name
```

The same name goes in the scan response, because some hosts cache only that.
Flags are `0x06`, not the example's `0x05` (limited discoverable), which some
scanners treat as short-lived.

**Advertising must be re-enabled after a disconnect.** A connectable advert
stops the moment it wins a connection, and nothing restarts it: the example
calls `BLE_GAP_SetAdvEnable` once, in `APP_STATE_INIT`, and its
`BLE_GAP_EVT_DISCONNECTED` handler does not. Before this was fixed, **the
board was invisible after one disconnect until it was reset.** It is now
re-enabled in `ble_link_down()`, which the GAP handler already calls.

## 4.3 The data channel

Microchip's **Transparent UART service** (TRSPS), a byte pipe:

| | UUID |
|---|---|
| board to host, notify | `49535343-1e4d-4bd9-ba61-23c647249616` |
| host to board, write | `49535343-8841-43f4-a8d4-ecbe34729bb3` |

Three facts about this Harmony release, found by reading its headers rather
than guessing:

- `BLE_TRSPS_GetDataLength` returns **void**, not a status code.
- There is **no** `BLE_TRSPS_EVT_CONNECTED`. The usable "ready to notify"
  signal is `BLE_TRSPS_EVT_TX_STATUS` with status
  `BLE_TRSPS_STATUS_TX_OPENED`, which fires when the central enables the TX
  characteristic's CCCD — exactly when notifications become possible.
- Disconnect must come from the GAP handler; the profile has no such event.

## 4.4 Link encryption, and the bug that cost an hour

The TX characteristic's CCCD requires an **encrypted** link, so the host must
be bonded before notifications can be enabled. In `ble_trs.c`:

```c
#define TRS_PERM_WRITE  (PERMISSION_WRITE | PERMISSION_WRITE_ENC)
#define TRS_PERM_CCCD   (PERMISSION_READ | PERMISSION_WRITE | PERMISSION_WRITE_ENC)
```

**Both bits are required.** They are independent: `PERMISSION_WRITE` (0x10)
grants access, `PERMISSION_WRITE_ENC` (0x80) adds the constraint. Setting only
`WRITE_ENC` leaves no access bit and the attribute becomes read-only.

The symptom is badly misleading. Pairing, bonding and service discovery all
succeed and `bluetoothctl` reports `ServicesResolved: yes`, because those
exercise only reads. The failure appears **only** when a client enables
notifications, as `ATT 0x03 Write Not Permitted` on the CCCD write — which
reads like a wrong handle. A genuine encryption shortfall would give 0x05,
Insufficient Authentication.

Note: the `WBZ451-Secure-BLE-Sensor` reference repo and its README both say to
change `PERMISSION_WRITE` **to** `PERMISSION_WRITE_ENC`, which is wrong.

## 4.5 MTU

```c
#define MTU_DEFAULT   20u      ATT MTU 23 minus 3 bytes of notification header
#define MTU_CEILING  180u
```

`ble_link_mtu_set()` takes the negotiated ATT MTU and stores `MTU - 3`, capped
at 180. A full frame is 675 bytes, so it is always sent in several
notifications.

---

# 5. The sensor side

## 5.1 The bus

One SPI master, **SERCOM1**, mode 0 at 1 MHz, with sixteen GPIO chip selects.

| net | module pin | port |
|---|---|---|
| SPI_SCK | 29 | PA8 |
| SPI_MISO | 30 | PA9 |
| SPI_MOSI | 31 | PA7 |
| IMU_INT | 39 | PB4 |

**Two MCC settings default wrong and fail silently:**

- `SCOM1_HSEN` must be **DIRECT**, not PPS, in DEVCFG1.
- `DIPO` must be **PAD2**; `DOPO` is PAD0.

Get either wrong and the bus clocks happily while MISO reads nothing.

All sixteen `INT1` lines share **one net**, so the line says *some* device is
ready, never which. Capture therefore polls `STATUS_REG` instead and the
interrupt is unused.

`PB0`, `PB8`, `PB9` are deliberately not used as chip selects: two of them
carry SWD_CLK and SWD_IO, and driving those as GPIO would cost the debugger.

## 5.2 Register map — everything the firmware writes

There is nothing else. No calibration, no filtering.

**Once at startup** — `lsm_configure_all()`, all sixteen in one transaction:

| register | value | meaning |
|---|---|---|
| `INT1_CTRL` | `0x01` | `INT1_DRDY_XL` — accel data-ready on INT1 |
| `COUNTER_BDR1` | `0x80` | `DRDY_PULSED` — data-ready is a pulse, not a latched level |
| `CTRL10_C` | `0x20` | `TIMESTAMP_EN` — the per-chip timestamp counter runs |

**Per capture** — `lsm_capture()`, all sixteen together:

| register | value | meaning |
|---|---|---|
| `CTRL1_XL`, `CTRL2_G` | `0x00`, `0x00` | power both down |
| `CTRL1_XL` | `0x10` | ODR **12.5 Hz**, FS **±2 g**, `LPF2_XL_EN` = 0 |
| `CTRL2_G` | `0x10` | gyro ODR 12.5 Hz, FS **250 dps** |

**When armed for a tap** — `lsm_tap_configure()`, **one sensor only**:

| register | value | meaning |
|---|---|---|
| `CTRL1_XL` | `0x60` | ODR **416 Hz**, FS ±2 g |
| `TAP_CFG0` | `0x0E` | `TAP_X_EN` \| `TAP_Y_EN` \| `TAP_Z_EN`; `SLOPE_FDS`=0, `LIR`=0 |
| `TAP_CFG1` | `0x05` | X threshold 5 → **312 mg** |
| `TAP_CFG2` | `0x85` | `INTERRUPTS_ENABLE` + Y threshold 5 |
| `TAP_THS_6D` | `0x05` | Z threshold 5 |
| `INT_DUR2` | `0x8F` | DUR 8, QUIET 3, SHOCK 3 |
| `WAKE_UP_THS` | `0x80` | `SINGLE_DOUBLE_TAP` = 1 — double taps only |

**At the moment of the tap** — `lsm_gyro_enable()`, same one sensor:

| register | value | meaning |
|---|---|---|
| `CTRL2_G` | `0x60` | gyro ODR 416 Hz, for the stillness gate |

### Scale factors

| quantity | full scale | per LSB |
|---|---|---|
| acceleration | ±2 g | 0.061 mg → **16384 LSB/g** |
| angular rate | ±250 dps | 8.75 mdps |
| temperature | — | **256 LSB/°C**, offset +25 °C |

### Tap threshold arithmetic

Each threshold is 5 bits in units of **full scale ÷ 32**. At ±2 g that is
**62.5 mg per count**, so `5` = 312 mg and `8` = 500 mg.

### `INT_DUR2`, decoded

None of the three fields is in units of 1/ODR:

| field | bits | per count | value | at 416 Hz |
|---|---|---|---|---|
| `SHOCK` | 1:0 | 8/ODR | 3 | 58 ms — longest over-threshold excursion still counted as a tap |
| `QUIET` | 3:2 | 4/ODR | 3 | 29 ms — dead time after a tap, so ringing is not read as the second |
| `DUR` | 7:4 | **32/ODR** | 8 | **615 ms** — longest gap *between* the two taps |

The window therefore moves in 77 ms steps. A wider window adds **no latency**:
detection fires the instant the second tap arrives; the window only bounds how
long a lone first tap waits for a partner.

## 5.3 What is deliberately **not** written

- `X_OFS_USR`, `Y_OFS_USR`, `Z_OFS_USR` (0x73–0x75) stay zero, and
  `CTRL7_G.USR_OFF_ON_OUT` stays clear so they could not reach the output
  anyway.
- `CTRL8_XL` untouched — no high-pass, no slope filter in the data path.
- `CTRL1_XL` bit 1 leaves `LPF2` disabled.
- `CTRL3_C.BDU` is **0**. See §9.

What remains is the chip's **factory trim**, which is the output path itself
and has no off switch. It is also what leaves the ±1 % / ±20 mg residual the
PUF depends on.

---

# 6. The capture sequence

`lsm_capture()`, six steps. The order is the whole design.

```
1.  write CTRL1_XL/CTRL2_G = 0x00 to ALL SIXTEEN in one transaction
        every sensor stops at the same instant

2.  write CTRL1_XL/CTRL2_G = 0x10 to ALL SIXTEEN in one transaction
        every sensor starts at the same instant

3.  read and throw away both output banks on all sixteen
        clears any sample that predates the synchronised start, and
        clears the stale data-ready flags that go with it

4.  poll STATUS_REG on each sensor until (XLDA | GDA) is set on ALL sixteen
        5 ms between rounds, 600 ms cap, counting waits
        TWO rounds; the first sample is discarded as unsettled

5.  write CTRL1_XL/CTRL2_G = 0x00 to ALL SIXTEEN
        freeze, so the readout cannot race a new conversion

6.  read each sensor in turn: STATUS_REG first, then accel, gyro,
        temperature, INTERNAL_FREQ_FINE, timestamp
```

Four hard-won details:

**Step 3 must clear both banks.** Data-ready flags clear only when the
corresponding output registers are read. Accel and gyro have separate banks
and separate flags.

**Step 4 must wait for `GDA` as well as `XLDA`.** The gyroscope converts later
than the accelerometer. Waiting on `XLDA` alone meant reading stale gyro
registers, which showed up as sensor 0 reporting −18 dps at rest.

**Step 4 must poll every sensor, not one.** An earlier version polled
`cs_tab[0]`, whose `XLDA` was stale from running at 416 Hz for tap detection.
Fifteen sensors then read exactly 0.0000 while sensor 0 looked fine. The fix
that found it was not a guess — `STATUS_REG` and `waits` were added to the
wire format, and the frame immediately showed `waits 0` and `0x04` on fifteen
sensors.

**Step 6 reads `STATUS_REG` before the data**, so each record carries proof
that its own reading was fresh. `0x07` means accel, gyro and temperature were
all ready.

## 6.1 The stillness gate

A tap shakes the board, and sampling while it rings records an orientation
nobody meant to present. So after a tap:

```c
lsm_gyro_enable(TAP_CS);                    // gyro on, 416 Hz
lsm_still_poll(TAP_CS, &s_still);           // all three axes under 5 deg/s
                                            // for 200 ms, 3 s timeout
```

On timeout the capture still runs and `was_still` records that the frame
cannot be trusted. **The gyro contributes no key bits** — it reads zero at
rest in every orientation — but it is the only thing that can certify
stillness.

**The gyro is enabled at the tap, not while armed.** Running it continuously
made that one sensor 2 K warmer than the other fifteen, which at the
datasheet's 4.5 mg/°C on Z is tens of milli-g of offset — and an offset that
*grew with how long the operator waited before tapping*. A drifting offset is
far worse than a fixed one: the same board then yields different keys
depending on timing.

---

# 7. Tap detection — and why only one sensor

```c
app/sensor_app.c:8   #define TAP_CS  BOARD_CS0_PIN
```

Used in five places, always the same sensor. It has been one sensor since the
first version of the app layer and has never been otherwise.

**Why not all sixteen:**

1. **No coverage benefit.** The board is small and rigid, so a tap moves it as
   a rigid body — all sixteen feel essentially the same shock. A second
   watcher at the far end sees what the first sees.
2. **The interrupt cannot help.** All sixteen `INT1` lines share one net, so
   the line never says *which* sensor fired. There is no information to gain.
3. **Power and heat.** Tap detection needs **416 Hz**; the other fifteen sit
   at 12.5 Hz. Sixteen sensors at 416 Hz on a 150 mAh cell costs current, and
   warms the whole array — and thermal drift is the thing the project exists
   to fight.
4. **Confining it is better than spreading it.** If tap duty does perturb its
   sensor, having that confined to one known index is diagnosable and could be
   compensated. Spread thinly across sixteen it would be neither.
5. **The threshold, not the count, is the lever.** If taps are hard to
   trigger, lower `TAP_CFG1/2` and `TAP_THS_6D`. A second watcher at the same
   threshold just asks the same question twice.

A spurious tap is cheap by design: the console arms the board only when a
capture has been asked for, so a false trigger costs one re-taken pose and can
touch nothing in the vault.

---

# 8. The application layer

## 8.1 State machine

`sensor_app.c`, four states, stepped every 10 ms:

| state | meaning | leaves when |
|---|---|---|
| `SA_IDLE` | nothing pending | `ARM` arrives |
| `SA_ARMED` | tap registers written, polling `TAP_SRC` | a double tap is seen → `MS1-TAP` |
| `SA_SETTLING` | gyro on, waiting for stillness | still, or 3 s timeout → capture, encode |
| `SA_SENDING` | pushing the frame over BLE | sent, or 2 s of refusals, or link lost |

## 8.2 Protocol

Host to board, ASCII lines:

| command | reply |
|---|---|
| `ARM` | `MS1-ARMED`, or `MS1-BUSY` if not idle |
| `DISARM` | `MS1-IDLE` |
| `PING` | `MS1-PONG` |
| `STATUS` | `MS1-STAT <state digit> <capture count>` |
| unknown | `MS1-ERR` |

Board to host, unsolicited: `MS1-TAP` when a tap is detected, then the frame.

**The host arms the board.** A tap produces a frame only when one has been
asked for, so handling the device cannot generate spurious captures.

## 8.3 Wire format

One ASCII line per frame:

```
MS1 <seq:8> <flag:1> <present:4> <skew:8> <waits:4> <records...>\n
```

Each record is 20 bytes, sent as 40 hex characters:

| field | bytes | type |
|---|---|---|
| `ax ay az` | 0–5 | int16 LE, raw LSB |
| `gx gy gz` | 6–11 | int16 LE, raw LSB |
| `temp` | 12–13 | int16 LE, raw LSB |
| `freq_fine` | 14 | int8 |
| `status` | 15 | uint8 — `STATUS_REG` at readout |
| `timestamp` | 16–19 | uint32 LE, 25 µs per count |

`FRAME_LINE_MAX` is 704: 34 header + 16 × 40 + newline = 675.

**No scaling, averaging or filtering happens on the board.** The output
registers go out verbatim. Anything the firmware did to the numbers would be
one more thing to reproduce and verify on the host.

### Header fields

- **`present`** — bitmask of which sensors answered. A frame missing a sensor
  is reported, never padded.
- **`waits`** — how many 5 ms rounds step 4 spent waiting. A health signal.
- **`skew`** — spread of the sixteen timestamps, in µs. **Currently
  meaningless**: each chip's counter free-runs from its own power-up and is
  never reset, so the spread measures boot order, not sample skew.
- **`freq_fine`** — `INTERNAL_FREQ_FINE` (0x63). The internal oscillator is
  not exact; the true rate is `ODR × (1 + 0.0015 × freq_fine)`. It is on the
  wire so the host can convert each chip's timestamp to real time.

## 8.4 The host decoder

`mini_sensor/host/frame_codec.py` exposes both forms per sensor:

```python
"raw_accel": [ax, ay, az],      # raw LSB -- what the key uses
"raw_gyro":  [gx, gy, gz],
"raw_temp":  temp,
"accel_g":   [v * 2.0   / 32768 for v in (ax, ay, az)],
"gyro_dps":  [v * 250.0 / 32768 for v in (gx, gy, gz)],
"temp_c":    temp / 256.0 + 25.0,
```

The console builds its `(16, 3)` array from **`raw_accel`**. The converted
fields are for display and the stillness check only.

**Nothing converts key material.** MAD normalisation is
`(x − µ) / mean|x − µ|`, which is scale-invariant, so 16384 LSB/g feeds a
pipeline built for 265 LSB/g unchanged.

---

# 9. Known faults and open items

**`BDU` is not set.** `CTRL3_C` bit 6 is 0, so the chip may replace an output
register between the host's high-byte and low-byte reads. Setting it would be
`CTRL3_C = 0x44` — **`0x44`, not `0x40`**, because `IF_INC` is bit 2, defaults
to 1, and is what makes block reads auto-increment. Clearing it would break
every multi-byte read.

**Board `ms1-248a` has a defective sensor 0.** It reads exactly **+1.000 g**
on Z — subtracting 16384 counts recovers the cohort value to within normal
noise (residual sd 148 counts against the healthy 146). Measured on **100 %**
of 56 frames, on both firmware versions. Boards `ms1-43be` and `ms1-3c65` show
it on **0 %** of 34 frames. It is hardware, not firmware, and not tap duty —
every board runs the identical image. **Retire that board from measurements.**

**`skew` is meaningless** until the timestamp counters are reset at capture.

**Sensor 0 carries the largest offset** of the sixteen on `ms1-43be`
(+16.3 mg against the array median), but that is inside the datasheet's
±20 mg and sensor 13 sits at +9.9 mg without any tap duty — so it is not
evidence of self-heating.

**`User_BTN` is unusable** on rev 1.0 (shorted to ground). Rev 2 should fix
the footprint, and could add a `+BATT` divider to two spare ADC pins for
battery reporting — there is none today, so battery level cannot be measured.

---

# 10. Build and flash

```
MPLAB X v6.30, MCC Harmony v5.7.0, XC32 v6.00, MPLAB Snap over SWD

Project:  ble_upstream/.../peripheral_trp_uart/firmware/peripheral_trp_uart.X
          Clean and Build  ->  Make and Program Device
```

**After every flash:**

- **Bonds are erased.** A full-chip program wipes PDS, so every paired machine
  must pair again. On Linux: `bluetoothctl remove <address>`, then pair with
  an agent registered (`agent on`, `default-agent`).
- **The address does not change** — it comes from the silicon, not the image.
- **The board stops advertising while connected**, so only one machine may
  hold it at a time. It holds **8 bonds**
  (`BLE_DM_MAX_PAIRED_DEVICE_NUM`), so several machines can stay paired.

---

# 11. Reading list, in order

1. `sensor/board_pins.h` — the pin map, and the two silent MCC gotchas
2. `sensor/lsm6dsox.h` — register names and the sample structure
3. `sensor/lsm6dsox.c`, `lsm_capture()` — the six steps in §6
4. `app/frame.c` — the wire format in §8.3
5. `app/sensor_app.c` — the state machine in §8.1
6. `app/ble_link.c` — the BLE seam and `ble_identity_configure()`
7. `mini_sensor/host/frame_codec.py` — the decoder, to see the format from
   the other side

Register claims in this document are checked against `DS_lsm6dsox.pdf`
(LSM6DSOX datasheet, DS12814 Rev 4), not recalled.
