#!/usr/bin/env python3
"""The ACCOLADE recall in one short test: one radio session on a defective battery must not latch
Safety Mode. On the base firmware it does, so this test fails.

A new session of the project, the implant clock slowed to 3600x (48 implant hours in 48 guest
seconds), the cell ~2 years from replacement with the high-impedance defect, then one bedside
communicator wake-up. The radio's milliamps sag the rail below the supervisor's 2.325 V, the chip
resets, the firmware retries the session, and the third reset within 48 hours latches Safety Mode.
The test watches the 48-hour window plus one retry interval and fails on the Safety Mode alarm.

    emerson run scenarios/test_safety_mode_quick.py -q -s

Needs emerson-server up on firmware/firmware.bin (`emerson load`), and a session of yours for
`emerson run` to start from; the test starts and stops its own.
"""

import os
import re
import sys
import time

import pytest

emerson = pytest.importorskip("emerson")

HOST = os.environ.get("EMERSON_HOST", "http://localhost:10314")
PROJECT = os.environ.get("EMERSON_PROJECT", "rp2350")
SCALE = 3600  # implant seconds per guest second
WATCH_S = 52  # guest seconds: the 48-hour window plus one 4-hour retry


def guest_s(battery):
    return float(re.search(r"at guest ([0-9.]+) s", battery.invoke_action("get_rail", "")).group(1))


def wait_for(tty, text, out, wall_s):
    """Read `tty` into `out` until it holds `text`; False after `wall_s` seconds."""
    deadline = time.monotonic() + wall_s
    while text not in out:
        if time.monotonic() > deadline:
            return False
        out += tty.read(timeout=0.2) or b""
    return True


@pytest.fixture
def machine():
    with emerson.EmulatorController(HOST).connect() as conn:
        token = conn.run_project(PROJECT)
        try:
            with conn.attach(token) as m:
                yield m
        finally:
            try:
                conn.stop_project(token)
            except RuntimeError:
                pass


def test_one_radio_session_on_a_defective_battery_does_not_latch_safety_mode(machine):
    tty1 = machine.subscribe_broker("tty1")  # telemetry: the marker channel and the alarms
    try:
        out = bytearray()
        machine.go()
        assert wait_for(tty1, b"[telemetry]", out, 30), "the firmware did not boot"

        battery = machine.get_device("/board/battery")
        machine.send_to_broker("tty0", f"timescale {SCALE}\r".encode())
        battery.invoke_action("set_aging_factor", str(SCALE))  # the cell ages at the clock's rate
        battery.invoke_action("set_depletion", "86.5")         # ~2 years from replacement
        # The time scale reaches the EEPROM in the background; a reset before it lands boots at 1,000,000x.
        start = guest_s(battery)
        while guest_s(battery) - start < 5.0:
            time.sleep(0.2)

        print("\n" + battery.invoke_action("set_defect", "1").split(";")[1].strip())  # R_int ~14 -> ~263 ohm
        machine.get_device("/patient/communicator").invoke_action("start_session", "")

        start = guest_s(battery)
        deadline = time.monotonic() + 180
        while b"ALARM: SAFETY MODE" not in out and guest_s(battery) - start < WATCH_S:
            assert time.monotonic() < deadline, "the guest clock stalled"
            out += tty1.read(timeout=0.2) or b""
        resets = int(re.search(r"(\d+) resets", machine.get_device("/board/supervisor").invoke_action("get_status", "")).group(1))
        print(f"supervisor resets: {resets}")
        if b"ALARM: SAFETY MODE" in out:
            alarm = next(x for x in out.decode(errors="replace").splitlines() if x.startswith("ALARM: SAFETY MODE"))
            pytest.fail(f"one radio session on a defective battery latched Safety Mode after {resets} supervisor resets: {alarm.split(' Permanent')[0]}",
                        pytrace=False)
    finally:
        tty1.close()


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, *sys.argv[1:]]))
