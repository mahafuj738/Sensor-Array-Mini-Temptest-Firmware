# Mini sensor firmware — what changed, and what each change does

Register names and bit fields below are checked against `DS_lsm6dsox.pdf`
(LSM6DSOX datasheet, DS12814 Rev 4), not quoted from memory. Line references
are to that document's extracted text where a claim depends on it.

---

## 1. Scope

Two flashes have happened. They are easy to confuse because both changed
behaviour the operator notices.

| flash | commits | what the operator saw |
|---|---|---|
| **previous** | `f296002`, `f824be7`, `3530854` | the board got its own name (`ms1-xxxx`) and could be reconnected without a power cycle |
| **this one** | `1411626`, `61d0b22` | the tap is easier and the gap between the two taps is longer |

**This flash changed the tap registers and nothing else.** No change to how
the sixteen sensors are configured, read, or converted. That matters when
asking whether the firmware caused a change in key quality: the measurement
path is byte-for-byte identical to the previous flash.

---

## 2. This flash

### 2.1 Tap threshold: 500 mg → 312 mg

```c
lsm_write_reg(cs, LSM_TAP_CFG1,   0x05u);   /* was 0x08 — X threshold */
lsm_write_reg(cs, LSM_TAP_CFG2,   0x85u);   /* was 0x88 — Y threshold + INTERRUPTS_ENABLE */
lsm_write_reg(cs, LSM_TAP_THS_6D, 0x05u);   /* was 0x08 — Z threshold */
```

Each threshold is 5 bits in units of **full-scale ÷ 32**. At ±2 g that is
62.5 mg per count, so `8` = 500 mg and `5` = **312 mg**. An axis must exceed
its threshold for a knock to be considered a tap at all.

`TAP_CFG2` bit 7 is `INTERRUPTS_ENABLE`, which is why that register reads
`0x85` rather than `0x05`.

**Effect:** a lighter knock registers. **Risk accepted:** a spurious tap is
cheap here, because the console arms the board only when a capture has been
asked for — a false trigger costs one re-taken pose and can touch nothing in
the vault.

### 2.2 Double-tap window: 538 ms → 615 ms

```c
lsm_write_reg(cs, LSM_INT_DUR2, 0x8Fu);     /* was 0x7F */
```

`INT_DUR2` packs three fields, and **none of them is in units of 1/ODR** —
this was got wrong once, so the units are spelled out:

| field | bits | per count | value | at 416 Hz |
|---|---|---|---|---|
| `SHOCK` | 1:0 | 8/ODR | 3 | 58 ms — longest an axis may stay over threshold and still count as a tap rather than a shove |
| `QUIET` | 3:2 | 4/ODR | 3 | 29 ms — dead time after a tap, so the board ringing from the first knock is not read as the second |
| `DUR` | 7:4 | **32/ODR** | 8 (was 7) | **615 ms** (was 538 ms) — longest gap allowed *between* the two taps |

Datasheet: `DUR` default `0000` is 16/ODR_XL, and any other value is
32/ODR_XL per count (line 4504); `QUIET` 4/ODR_XL (4509); `SHOCK` 8/ODR_XL
(4512).

The window therefore moves in **77 ms steps**, and 615 ms is the nearest step
to the 600 ms wanted. `QUIET` and `SHOCK` stay at maximum because both only
make detection more forgiving.

**Effect:** the two taps may be further apart. **Given up:** telling two taps
meant as separate events from one double tap — which costs nothing, because a
tap can only ever cause a capture and only when one was requested.

**Latency note:** a wider window adds none. Detection fires the instant the
second tap arrives; the window only governs how long a lone first tap waits
for a partner.

### 2.3 Host-side, same commit: `raw_gyro` and `raw_temp`

`mini_sensor/host/frame_codec.py` now exposes `raw_gyro` and `raw_temp`
alongside `raw_accel`. The gyro had been converted-only, and a
converted-only field is one an analysis has to un-convert.

**Nothing on the wire changed** — the firmware already sent raw int16 for
all three. This is a decoder addition, so it cannot affect the board.

---

## 3. Previous flash, for completeness

### 3.1 Advertise again after a disconnect

```c
void ble_link_down(void) { ... (void)BLE_GAP_SetAdvEnable(0x01, 0x00); }
```

A connectable advert stops the moment it wins a connection, and nothing
restarted it: the stock example calls `BLE_GAP_SetAdvEnable` exactly once, in
`APP_STATE_INIT`, and its `BLE_GAP_EVT_DISCONNECTED` handler did not. **After
one disconnect the board was invisible until reset.** Re-enabled in
`ble_link_down()`, which the GAP handler already calls.

### 3.2 One image, a different identity per board

`ble_identity_configure()` in `app/ble_link.c`, called from
`APP_BleConfigBasic` in place of the example's hard-coded address block:

1. ask the stack what address it already has (`BLE_GAP_GetDeviceAddr`)
2. keep it if it is neither all-zero nor all-ones
3. otherwise fall back to the example's `A6:A5:A4:A3:A2:A1`
4. advertise as `ms1-<last two bytes>`, and set the same string as the GAP
   device name

Confirmed on hardware: the WBZ451 **does** carry a factory address, and
Microchip's example was overwriting it. Both boards flashed so far came up
with their own, sharing Microchip's `9C:95:6E` prefix:

| address | advertised name |
|---|---|
| `9C:95:6E:4D:43:BE` | `ms1-43be` |
| `9C:95:6E:4D:3C:65` | `ms1-3c65` |

**One image serves any number of boards** — no per-board build, no stored
address.

Two details folded in:

- advert flags went `0x05` → **`0x06`** (general discoverable rather than
  limited, which some scanners treat as short-lived)
- the GAP device name is set from the same string, which is what stops a
  bonded host caching `Microchip` for every board

---

## 4. Complete register state after this flash

Everything the firmware writes to each LSM6DSOX. There is nothing else.

### Once, at startup — `lsm_configure_all()`, all sixteen together

| register | value | meaning |
|---|---|---|
| `INT1_CTRL` | `0x01` | `INT1_DRDY_XL` — accelerometer data-ready on INT1 |
| `COUNTER_BDR1` | `0x80` | `DRDY_PULSED` — data-ready is a pulse, not a latched level |
| `CTRL10_C` | `0x20` | `TIMESTAMP_EN` — the per-chip timestamp counter runs |

### Per capture — `lsm_capture()`, all sixteen together

| register | value | meaning |
|---|---|---|
| `CTRL1_XL`, `CTRL2_G` | `0x00, 0x00` | stop both, then |
| `CTRL1_XL` | `0x10` | ODR **12.5 Hz**, FS **±2 g**, `LPF2_XL_EN` = 0 |
| `CTRL2_G` | `0x10` | gyro ODR 12.5 Hz, FS 250 dps |
| `CTRL1_XL`, `CTRL2_G` | `0x00, 0x00` | freeze after readout |

Two rounds are taken and the first is discarded as unsettled.

### When armed for a tap — `lsm_tap_configure()`, one sensor only

| register | value | meaning |
|---|---|---|
| `CTRL1_XL` | `0x60` | ODR **416 Hz**, FS ±2 g |
| `TAP_CFG0` | `0x0E` | `TAP_X_EN` \| `TAP_Y_EN` \| `TAP_Z_EN`; `SLOPE_FDS` = 0, `LIR` = 0 |
| `TAP_CFG1` | `0x05` | X threshold 5 → 312 mg |
| `TAP_CFG2` | `0x85` | `INTERRUPTS_ENABLE` + Y threshold 5 |
| `TAP_THS_6D` | `0x05` | Z threshold 5 |
| `INT_DUR2` | `0x8F` | DUR 8 (615 ms), QUIET 3, SHOCK 3 |
| `WAKE_UP_THS` | `0x80` | `SINGLE_DOUBLE_TAP` = 1 — double taps only |

### At the moment of the tap — `lsm_gyro_enable()`, one sensor only

| register | value | meaning |
|---|---|---|
| `CTRL2_G` | `0x60` | gyro ODR 416 Hz, 250 dps, for the stillness gate |

The gyro is enabled **at the tap, not while armed**: running it continuously
made that sensor 2 K warmer, with an offset that grew with how long the
operator waited before tapping. A drifting offset is far worse than a fixed
one, because the same board then gives different keys depending on timing.

---

## 5. What did *not* change, and is worth knowing

- **No calibration is applied, and none can be switched off.** `X/Y/Z_OFS_USR`
  stay zero, `CTRL7_G.USR_OFF_ON_OUT` stays clear so they could not reach the
  output anyway, `CTRL8_XL` is untouched so there is no high-pass or slope
  filter in the data path, and `CTRL1_XL` bit 1 leaves `LPF2` disabled. What
  remains is the chip's factory trim, which is the output path itself.
- **The data path is raw counts end to end.** The firmware writes the output
  registers verbatim — no scaling, averaging or filtering — and the console
  builds its `(16, 3)` array from `raw_accel`. MAD normalisation is
  scale-invariant, so no conversion is needed or performed. `accel_g`,
  `gyro_dps` and `temp_c` exist for display and the stillness gate only.
- **`BDU` (`CTRL3_C` bit 6) is still 0.** The chip may therefore replace an
  output register between the host's high-byte and low-byte reads. Setting it
  would be `CTRL3_C = 0x44` — **`0x44`, not `0x40`**, because `IF_INC` is bit 2,
  defaults to 1 (line 3123), and is what makes block reads auto-increment;
  clearing it would break every multi-byte read.

---

## 6. Operational consequences of any flash

- **Bonds are erased.** A full-chip program wipes PDS, so every paired machine
  must pair again. On Linux: `bluetoothctl remove <address>`, then pair with an
  agent registered (`agent on`, `default-agent`).
- **The address does not change.** It comes from the silicon, not the image.
- **The board stops advertising while connected**, so only one machine may hold
  it at a time. It holds **8 bonds** (`BLE_DM_MAX_PAIRED_DEVICE_NUM`,
  `ble_dm.h:98`), so several machines can stay paired.
- **The build tree keeps its own copies** of `firmware/src/**`. Editing only
  the tracked tree builds the stale file — see
  `mini_sensor/firmware/ble_patches/` and check with `md5sum` before building.

---

## 7. Open, as of 2026-10-05

- **Sensor 0 on `ms1-43be` reads exactly +1.000 g on Z in ~15 % of captures.**
  Subtracting 16384 counts recovers the cohort value to within normal noise
  (residual sd 148 counts against the healthy 146). Measured, cause unknown.
  Not a torn read — a splice gives random values, not a precise 2¹⁴ fifteen
  times. Not a stuck bit — it *adds* rather than ORs. It does not measurably
  affect the key: across 99 sessions, wrong-bit counts show no correlation
  with it in either the live or the enrolment path.
- **`ms1-3c65` shows none of it** — 0 of 22 frames with a >100 mg within-frame
  outlier, against 56 of 140 on `ms1-43be`. Chance is excluded, but board,
  firmware and host all differ between those datasets, so which difference
  matters is not established.
- **`BDU` not set.** Worth folding into a future flash as data hygiene, not as
  a fix for the above.
