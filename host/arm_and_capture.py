#!/usr/bin/env python3
"""Take one capture from the mini sensor over BLE, decode and print it.

Bench verification tool for the temperature-test firmware (PING answers
MS1-PONG TT1), not the GUI. It proves the whole path in one run: pair, connect,
CAP, frame arrives, decode sixteen sensors. No tap: the board captures as soon
as its gyroscope says it is still.

    pip install bleak
    python3 host/arm_and_capture.py
    python3 host/arm_and_capture.py --temp      # the sixteen die temperatures
    python3 host/arm_and_capture.py --list      # what is advertising, unfiltered

The board advertises as "pic32...". First run pairs and bonds; later runs
reconnect without asking.
"""
import asyncio
import os
import sys

# This file's own directory, explicitly. Ordinary CPython adds it for us, but
# the Windows bundle runs an embeddable interpreter with a ._pth file, which
# replaces the standard path calculation -- so a sibling import fails there
# and only there. frame_codec is a sibling, and this script is the bundle's
# Bluetooth diagnostic, which would have been a poor thing to have break on
# the one machine it was shipped to diagnose.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from bleak import BleakClient, BleakScanner        # noqa: E402

from frame_codec import decode_frame, report       # noqa: E402

TX_CHAR = "49535343-1e4d-4bd9-ba61-23c647249616"   # board -> host, notify
RX_CHAR = "49535343-8841-43f4-a8d4-ecbe34729bb3"   # host -> board, write
# A board flashed from this project advertises as "ms1-xxxx", its own four hex
# digits, and now sets the same string as its GAP device name. The other two
# are the stock Microchip example's: it advertises "pic32cx-bz" but reports
# "Microchip" over GAP, and BlueZ caches the latter after pairing, so a filter
# on "pic32" alone stopped matching a board already paired with.
# Only names that belong to this project.
#
# "pic32" and "microchip" used to be in here, so that a board still carrying the
# stock example could be found. They are also the names every other Microchip
# part in the room advertises -- a dev kit, a colleague's board, anything with
# the unmodified example on it -- so the scan list filled up with devices that
# were not ours and a tester had no way to tell which was which. Avoiding that
# collision is the whole reason the firmware name carries a per-board UID, and
# it is undone if the filter matches a generic vendor string.
#
# A board that has never had this firmware is not lost: it advertises as
# pic32cx-bz at the example's placeholder address, and acquire() takes an
# explicit address, so BOARD_ADDR below reaches it without a name match.
#
# "ms1-" stays for boards flashed before 2026-10-06, which are still ours.
TARGET_NAMES = ("hes_", "ms1-")



def print_temps(line: str) -> None:
    parts = line.split()
    present = int(parts[1], 16)
    temps = []
    for i, h in enumerate(parts[2:18]):
        if not (present >> i) & 1:
            print(f"  sensor {i:2d}      absent")
            continue
        v = int(h, 16)
        v = v - 0x10000 if v & 0x8000 else v
        temps.append(v / 256.0 + 25.0)
        print(f"  sensor {i:2d}  {temps[-1]:7.2f} C")
    if temps:
        print(f"  mean {sum(temps) / len(temps):.2f} C  "
              f"spread {max(temps) - min(temps):.2f} C  ({len(temps)} sensors)")


async def main() -> int:
    if "--forget" in sys.argv:
        import subprocess
        # Addresses given on the command line, plus the firmware's fallback.
        # One hard-coded address was enough when every board shared it; a
        # board with its own address has to be named.
        addrs = [a for a in sys.argv[1:] if a.count(":") == 5]
        for addr in addrs + ["A6:A5:A4:A3:A2:A1"]:
            subprocess.run(["bluetoothctl", "remove", addr],
                           capture_output=True)
        cache = os.path.expanduser("~/.wbz451_ble_addr")
        if os.path.exists(cache):
            os.remove(cache)
        print("cleared the old bond and address cache")

    if "--list" in sys.argv:
        # Deliberately unfiltered. The question this answers is "is the board
        # advertising at all", and a name filter that is wrong would answer
        # "no" for entirely the wrong reason.
        print("scanning 10 s -- everything in range, unfiltered:\n")
        try:
            found = await BleakScanner.discover(timeout=10.0, return_adv=True)
            rows = list(found.values())
        except TypeError:                     # older bleak: no return_adv
            rows = [(d, None) for d in await BleakScanner.discover(timeout=10.0)]

        def rssi_of(pair):
            d, adv = pair
            return getattr(adv, "rssi", None) or getattr(d, "rssi", None) or -999

        for d, adv in sorted(rows, key=rssi_of, reverse=True):
            name = d.name or getattr(adv, "local_name", None) or "(no name)"
            mine = (d.address.upper() == "A6:A5:A4:A3:A2:A1"
                    or any(n in name.lower() for n in TARGET_NAMES))
            print(f"  {d.address}  {str(rssi_of((d, adv))):>5} dBm  "
                  f"{name:<26}{'  <-- the mini sensor' if mine else ''}")
        if not rows:
            print("  nothing at all. That is the adapter, not the board: "
                  "check  rfkill list  and  bluetoothctl show")
        else:
            print(f"\n  {len(rows)} device(s). If the mini sensor is not "
                  f"marked above, it is not advertising -- and no amount of "
                  f"retrying on this side will find it.")
        return 0

    print(f"scanning for {' or '.join(TARGET_NAMES)} ...")
    device = None
    for d in await BleakScanner.discover(timeout=10.0):
        if d.name and any(n in d.name.lower() for n in TARGET_NAMES):
            device = d
            break
    if device is None:
        print("board not found. It stops advertising while connected to "
              "anything -- press RESET and try again.")
        return 1

    print(f"found {device.name} at {device.address}")
    want_temp = "--temp" in sys.argv
    pending = []
    done = asyncio.Event()

    def on_notify(_handle, data: bytearray) -> None:
        pending.append(bytes(data))
        joined = b"".join(pending)
        while b"\n" in joined:
            line, joined = joined.split(b"\n", 1)
            text = line.decode("ascii", "replace").strip()
            if not text:
                continue
            if text.startswith("MS1 "):
                try:
                    report(decode_frame(text))
                except ValueError as exc:
                    print(f"bad frame: {exc}")
                done.set()
            elif text.startswith("MS1-TEMP "):
                print_temps(text)
                done.set()
            else:
                print(f"board: {text}")
                if text.startswith("MS1-PONG") and text != "MS1-PONG TT1":
                    print("this board runs the tap image, not the temperature-"
                          "test image; CAP and TEMP will answer MS1-ERR")
        pending.clear()
        if joined:
            pending.append(joined)

    async with BleakClient(device) as client:
        # Pair from here rather than from bluetoothctl. The notification
        # descriptor requires an encrypted link (PERMISSION_WRITE_ENC), so
        # without a bond start_notify is refused outright -- on BlueZ that
        # surfaces as the thoroughly misleading "Write Not Permitted".
        try:
            await client.pair()
            print("paired")
        except NotImplementedError:
            pass                       # macOS/Windows pair on demand
        except Exception as exc:
            print(f"pair: {exc} (continuing -- may already be bonded)")

        await client.start_notify(TX_CHAR, on_notify)
        await client.write_gatt_char(RX_CHAR, b"PING\n", response=False)
        cmd = b"TEMP\n" if want_temp else b"CAP\n"
        print(f"connected. sending {cmd.decode().strip()} ...")
        await client.write_gatt_char(RX_CHAR, cmd, response=False)
        try:
            await asyncio.wait_for(done.wait(), timeout=15.0)
        except asyncio.TimeoutError:
            print("no reply in 15 s. A CAP should be answered by MS1-CAP and "
                  "a frame within about 4 s; if nothing came back, the write "
                  "never arrived.")
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
