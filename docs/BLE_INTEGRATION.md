# Grafting the sensor onto the BLE example

The radio side is already solved. `WBZ451-Secure-BLE-Sensor/` pairs, bonds,
encrypts with AES-CCM and reconnects across power cycles, all from two edited
files on top of Microchip's `peripheral_trp_uart`. Step 4 adds the sensor to
that, rather than adding BLE to the bring-up project.

## 1. Base project

```bash
git clone --depth 1 --filter=blob:none --sparse \
  https://github.com/Microchip-MPLAB-Harmony/wireless_apps_pic32cxbz2_wbz45.git
cd wireless_apps_pic32cxbz2_wbz45
git sparse-checkout set apps/ble/building_blocks/peripheral/profiles_services/peripheral_trp_uart
```

Two files carry the security changes. **Do not copy them wholesale.**
`WBZ451-Secure-BLE-Sensor/` targets an older Harmony release with a different
layout, and one of the two will not compile here:

| file | this release | the other repo |
|---|---|---|
| `app_ble_handler.c` | `src/app_ble/` | same — **copying works** |
| `ble_trs.c` | `src/config/.../service_ble/ble_trs/` | `service_ble/`, and it includes `api/gatt.h` and `byte_stream.h`, neither of which exists here |

So: copy `app_ble_handler.c`, and for `ble_trs.c` transcribe only the change
into this release's own file, leaving its includes alone. Both files keep
`WBZ_SECURITY_ENABLED 1`.

The change is five permission flags, and **the README in that repo gets them
wrong**. From `ble/lib/api/gatt.h`:

```c
#define PERMISSION_WRITE        0x10   /* access: attribute CAN BE written */
#define PERMISSION_WRITE_AUTHEN 0x20   /* ...and requires authentication   */
#define PERMISSION_WRITE_ENC    0x80   /* ...and requires encryption       */
```

`PERMISSION_WRITE` is the access bit; `PERMISSION_WRITE_ENC` is an additional
constraint. They are independent bits, so replacing one with the other leaves
no access bit set and the attribute becomes read-only. **Both are required:**

```c
#define TRS_PERM_WRITE  (PERMISSION_WRITE | PERMISSION_WRITE_ENC)
#define TRS_PERM_CCCD   (PERMISSION_READ | PERMISSION_WRITE | PERMISSION_WRITE_ENC)
```

Not `AUTHEN`: Just Works pairing yields unauthenticated keys, so an `AUTHEN`
bit denies access permanently.

Get this wrong and the symptom is specific and misleading: pairing, bonding and
service discovery all succeed, `bluetoothctl` reports `ServicesResolved: yes`
and looks perfectly healthy — because those need only *reads*. The failure
appears solely when a client enables notifications, as
`ATT 0x03 Write Not Permitted` on the CCCD write. Verified on hardware
2026-10-01: with both bits set, the link pairs, encrypts and notifies.

Copying `ble_trs.c` wholesale fails with `fatal error: api/gatt.h: No such file
or directory` followed by a cascade of `unknown type name 'uint8_t'`. Copying it
to the wrong directory is worse: it builds clean and silently gives you an
unsecured link, because nothing compiles the file you edited.

## 2. MCC configuration to reproduce

The bring-up project's SPI and pin setup has to exist in this project too.
Nothing here is optional and two of the four fail as a completely silent bus:

| Where | Setting |
|---|---|
| System → Device Configuration | `SCOM1_HSEN = DIRECT` — **not** PPS |
| SERCOM1 → SPI Master → Data In Pinout | `PAD2` (DIPO) |
| SERCOM1 | SPI master, 2 MHz, mode 3 |
| Pin Table | SERCOM1 PAD0/PAD2/PAD3 on RA7 / RA9 / RA8 |
| Pin Table | the sixteen chip selects plus LED, as GPIO outputs, initial high |

Pin assignments are in `PINMAP.md`. The chip selects must initialise **high**:
a chip select that boots low holds one sensor permanently selected and the bus
never works.

## 3. Files to add

Add `firmware/src/sensor/`, `firmware/src/app/` to the project. Do not add
`firmware/src/bringup/` — that was steps 1–3 and `capture.c` has a blocking
loop that would starve the BLE stack.

```
src/sensor/board_pins.h      pins, as MCC names them
src/sensor/lsm6dsox.{h,c}    register access, coherent capture, tap, stillness
src/app/frame.{h,c}          one frame -> one text line
src/app/sensor_app.{h,c}     the state machine, as a pure step function
src/app/sensor_task.{h,c}    the FreeRTOS task that ticks it every 10 ms
src/app/ble_link.{h,c}       the only file that includes Harmony BLE headers
```

## 4. Four call sites

Verified against this release's headers, not guessed.

In `app_ble/app_trsps_handler.c`, `APP_TrspsEvtHandler`:

```c
case BLE_TRSPS_EVT_TX_STATUS:
    ble_link_tx_ready(p_event->eventField.onTxStatus.connHandle,
                      p_event->eventField.onTxStatus.status
                          == BLE_TRSPS_STATUS_TX_OPENED);
    break;

case BLE_TRSPS_EVT_RECEIVE_DATA:
    ble_link_rx(p_event->eventField.onReceiveData.connHandle);
    break;
```

Replace the stock `BLE_TRSPS_EVT_RECEIVE_DATA` body entirely — it mallocs the
data and writes it to `SERCOM0_USART`, which this board does not have.

**There is no `BLE_TRSPS_EVT_CONNECTED`.** The profile's events are
`CTRL_STATUS`, `TX_STATUS`, `CBFC_ENABLED`, `CBFC_CREDIT`, `RECEIVE_DATA`,
`VENDOR_CMD`. `TX_STATUS` with `BLE_TRSPS_STATUS_TX_OPENED` is the right
connect signal: it fires when the central enables the TX characteristic's CCCD,
which is the moment notifications become possible. A GAP connection alone is
not enough — nobody has subscribed yet, and a send would be dropped.

In `app_ble/app_ble_handler.c`, `APP_BleGapEvtHandler`, add to the existing
`BLE_GAP_EVT_DISCONNECTED` case:

```c
ble_link_down();
```

In `app.c`, `APP_STATE_INIT`, after `APP_BleStackInit()`:

```c
sensor_task_start();
```

That is all app.c needs. **Do not** try to call the sensor from `APP_Tasks()`:
it blocks on `OSAL_QUEUE_Receive(..., OSAL_WAIT_FOREVER)` and only wakes when
the BLE stack posts a message, so anything added there would run at the radio's
convenience or not at all. `sensor_task.c` creates a separate FreeRTOS task at
priority 1 — below the stack's — ticking every 10 ms.

Optionally, wherever the MTU exchange completes:

```c
ble_link_mtu_set(negotiatedAttMtu);
```

Skipping it is safe; the link just sends 20-byte chunks, so a frame takes about
thirty notifications instead of four.

## 5. Wire protocol

Host to board, ASCII lines: `ARM`, `DISARM`, `PING`, `STATUS`.

Board to host, ASCII lines:

```
MS1-ARMED        armed, waiting for a tap
MS1-TAP          tap detected, settling
MS1-IDLE         disarmed; a capture just finished, or DISARM was sent
MS1-BUSY         ARM arrived mid-capture
MS1-PONG         reply to PING
MS1-STAT <s> <n> state digit, capture count
MS1 <seq> <flag> <present> <skew> <records...>
```

The frame line is specified in `firmware/src/app/frame.h`. One capture per
`ARM`: the board disarms itself after sending, so a frame can only ever exist
because the host asked for one.

## 6. What stays on the host

Everything with a secret in it. The board sends raw sensor counts and nothing
else — no normalisation, no pair selection, no key, no encryption beyond the
BLE link layer. Losing the board loses no key material, because there is none
on it. Temperature is sent per element, all sixteen, and the GUI averages for
display; the per-element spread is the part Chapter 5 cares about.

## 7. Known issue carried over

Stock WBZ451 boards all advertise the placeholder address
`A6:A5:A4:A3:A2:A1`. One board at a time, or set a unique address per board
before running two.
