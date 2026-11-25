#!/usr/bin/env python3
"""Decode the mini sensor's frame lines. No BLE dependency, so the GUI and the
test tool share it and it can be exercised against captured text offline.

Format is specified in firmware/src/app/frame.h.
"""
import struct

ACCEL_FS_G = 2.0
GYRO_FS_DPS = 250.0
RECORD_DIGITS = 40

# Header generations this parser understands. Exported so the console can
# print it at startup: a board sending a count that is not in here is the
# failure that looks exactly like a board that is not transmitting.
ACCEPTED_FIELDS = (7, 8, 11)


def decode_frame(line: str) -> dict:
    """Parse one 'MS1 ...' line into something printable. See frame.h."""
    parts = line.split(" ")
    n = len(parts)
    # Three header generations are accepted so one console can talk to any
    # board on the bench:
    #
    #    7  original
    #    8  + tapped
    #   11  + uptime, settle, gmax   (the diagnostic header)
    #
    # The count is checked against that exact set rather than a minimum. A
    # frame with some other count is a frame this parser does not understand,
    # and guessing at it is how a silently mis-decoded capture gets into a log.
    #
    # Fields a board cannot report come back None, never 0. None means "not
    # known"; 0 is a real measurement -- no sensor latched a tap, or the board
    # was already still on the first look -- and conflating the two would make
    # an old frame indistinguishable from a good one.
    if parts[0] != "MS1" or n not in ACCEPTED_FIELDS:
        raise ValueError(f"not a frame line ({n} fields): {line[:40]!r}")

    seq = int(parts[1], 16)
    was_still = parts[2] == "S"
    present = int(parts[3], 16)
    skew_us = int(parts[4], 16)
    waits = int(parts[5], 16)
    tapped = int(parts[6], 16) if n >= 8 else None
    uptime_ms = int(parts[7], 16) if n >= 11 else None
    settle_polls = int(parts[8], 16) if n >= 11 else None
    gyro_max_lsb = int(parts[9], 16) if n >= 11 else None
    body = parts[-1]

    indices = [i for i in range(16) if present >> i & 1]
    if len(body) != len(indices) * RECORD_DIGITS:
        raise ValueError(
            f"payload is {len(body)} digits, bitmap says {len(indices)} sensors"
        )

    sensors = {}
    for n, idx in enumerate(indices):
        raw = bytes.fromhex(body[n * RECORD_DIGITS:(n + 1) * RECORD_DIGITS])
        ax, ay, az, gx, gy, gz, temp = struct.unpack_from("<7h", raw, 0)
        freq_fine = struct.unpack_from("<b", raw, 14)[0]
        status = raw[15]
        timestamp = struct.unpack_from("<I", raw, 16)[0]
        sensors[idx] = {
            "accel_g": [v * ACCEL_FS_G / 32768.0 for v in (ax, ay, az)],
            "gyro_dps": [v * GYRO_FS_DPS / 32768.0 for v in (gx, gy, gz)],
            "temp_c": temp / 256.0 + 25.0,
            "freq_fine": freq_fine,
            "status": status,
            "timestamp_us": timestamp * 25,
            # Raw counts, straight off the chip, for both. raw_accel is what
            # the key is derived from -- nothing converts key material -- and
            # raw_gyro is here for symmetry, because a converted-only field is
            # a field an analysis has to un-convert.
            "raw_accel": [ax, ay, az],
            "raw_gyro": [gx, gy, gz],
            "raw_temp": temp,
        }

    return {
        "seq": seq,
        "was_still": was_still,
        "present": present,
        "tapped": tapped,
        "skew_us": skew_us,
        "waits": waits,
        # Milliseconds since the board booted. A fixed-pose run that degrades
        # over its length is the open question and this is the only time axis
        # in the frame; the host clock cannot substitute, because it does not
        # know when the capture happened, only when the notification arrived.
        "uptime_ms": uptime_ms,
        # How long the stillness gate held the capture back. 0 means already
        # still; TIMEOUT is 300, which with was_still False is a capture the
        # board itself does not vouch for.
        "settle_polls": settle_polls,
        "settle_ms": None if settle_polls is None else settle_polls * 10,
        # Worst |gyro| on the final poll, so "still" has a number attached.
        "gyro_max_lsb": gyro_max_lsb,
        "gyro_max_dps": (None if gyro_max_lsb is None
                         else gyro_max_lsb * GYRO_FS_DPS / 32768.0),
        "sensors": sensors,
    }


def report(frame: dict) -> None:
    s = frame["sensors"]
    print(f"\ncapture #{frame['seq']}   "
          f"{len(s)}/16 sensors   "
          f"skew {frame['skew_us']} us   "
          f"{'settled' if frame['was_still'] else 'MOVING - do not trust'}"
          f"   waits {frame['waits']}")
    if frame["uptime_ms"] is not None:
        print(f"uptime {frame['uptime_ms'] / 1000.0:.2f} s   "
              f"settle {frame['settle_ms']} ms   "
              f"gyro max {frame['gyro_max_dps']:.2f} dps   "
              f"tapped 0x{frame['tapped']:04X}")

    temps = [v["temp_c"] for v in s.values()]
    print(f"board temperature {sum(temps) / len(temps):.2f} C "
          f"(spread {max(temps) - min(temps):.2f} C)\n")

    print(f"{'n':>2}  {'ax':>8} {'ay':>8} {'az':>8}   "
          f"{'|a|':>6}  {'gx':>7} {'gy':>7} {'gz':>7}  {'temp':>6}")
    for idx in sorted(s):
        v = s[idx]
        ax, ay, az = v["accel_g"]
        mag = (ax * ax + ay * ay + az * az) ** 0.5
        gx, gy, gz = v["gyro_dps"]
        st = v["status"]
        flags = ("XLDA" if st & 1 else "----") + ("/GDA" if st & 2 else "/---")
        print(f"{idx:>2}  {ax:8.4f} {ay:8.4f} {az:8.4f}   {mag:6.3f}  "
              f"{gx:7.2f} {gy:7.2f} {gz:7.2f}  {v['temp_c']:6.2f}  "
              f"0x{st:02X} {flags}")

    mags = [sum(c * c for c in v["accel_g"]) ** 0.5 for v in s.values()]
    print(f"\n|a| should sit near 1.000 g for every sensor: "
          f"min {min(mags):.3f}  max {max(mags):.3f}")
