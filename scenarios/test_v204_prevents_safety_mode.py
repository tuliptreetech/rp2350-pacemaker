#!/usr/bin/env python3
"""v2.04 against the bug in test_safety_mode_quick.py: the same defective battery and the same
communicator session, but the device was upgraded first, so it does not reset or latch Safety Mode.

The quick test's device latches Safety Mode on one communicator wake-up. Here the clinic upgrades a
new device to v2.04 through the wand before the battery goes bad. Its daily battery test (run now with
`battest now`) finds the high-impedance cell and disables wandless telemetry; the communicator's
wake-up then goes unanswered, and nothing resets.

The device's own nightly wakeups are off (`radio wakeup 0`), so this checks the communicator path
only. With them on, v2.04's shipped bug resets the device anyway (test_v205_fixes_v204.py).

    emerson run scenarios/test_v204_prevents_safety_mode.py -q -s

Needs emerson-server up on firmware/firmware.bin (`emerson load`), and a session of yours for
`emerson run` to start from; the test starts and stops its own.
"""

import os
import re
import sys
import time
from pathlib import Path

import pytest

emerson = pytest.importorskip("emerson")
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "programmer"))
import pacemaker_programmer as pp  # noqa: E402  (the clinic programmer: the upgrade is a telemetry request)

HOST = os.environ.get("EMERSON_HOST", "http://localhost:10314")
PROJECT = os.environ.get("EMERSON_PROJECT", "rp2350")
SCALE = 3600  # implant seconds per guest second
WATCH_S = 20.0  # guest seconds after the connect: 20 implant hours, five of the radio's 4-hour retries


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


def read_for(tty, out, battery, seconds):
    """Read `tty` into `out` for `seconds` of guest time."""
    start = guest_s(battery)
    while guest_s(battery) - start < seconds:
        out += tty.read(timeout=0.2) or b""


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


def test_v204_disables_telemetry_and_the_communicator_session_resets_nothing(machine):
    tty1 = machine.subscribe_broker("tty1")  # telemetry: the marker channel and the alarms
    try:
        out = bytearray()
        machine.go()
        assert wait_for(tty1, b"[telemetry]", out, 30), "the firmware did not boot"

        battery = machine.get_device("/board/battery")
        supervisor = machine.get_device("/board/supervisor")
        machine.send_to_broker("tty0", f"timescale {SCALE}\r".encode())
        machine.send_to_broker("tty0", b"radio wakeup 0\r")  # the communicator path only (see above)
        battery.invoke_action("set_aging_factor", str(SCALE))
        battery.invoke_action("set_depletion", "86.5")  # ~2 years from replacement
        read_for(tty1, out, battery, 5.0)  # the time scale reaches the EEPROM before anything resets

        # The clinic upgrades the healthy device through the wand.
        link = pp.WandLink(machine)
        try:
            report = pp.interrogate(pp.Session(link, timeout_s=30), upgrade="v2.04")
        finally:
            link.close()
        assert report["fw"]["level"] == pp.FW_V204
        print("\nupgraded:", pp.FW_LEVEL_NAMES[report["fw"]["level"]])

        # The battery goes bad, and the daily battery test finds it.
        print(battery.invoke_action("set_defect", "1").split(";")[1].strip())  # R_int ~14 -> ~263 ohm
        machine.send_to_broker("tty0", b"battest now\r")
        assert wait_for(tty1, b"ALARM: WANDLESS TELEMETRY DISABLED", out, 60), "the battery test did not disable telemetry"
        text = out.decode(errors="replace")
        print(next(x for x in text.splitlines() if x.startswith("BATTEST")).split(" (runs")[0])
        print(next(x for x in text.splitlines() if x.startswith("ALARM: WANDLESS")))

        # The same communicator session that latched Safety Mode on the base firmware.
        resets_before = int(re.search(r"(\d+) resets", supervisor.invoke_action("get_status", "")).group(1))
        communicator = machine.get_device("/patient/communicator")
        communicator.invoke_action("start_session", "")
        read_for(tty1, out, battery, WATCH_S)
        print("communicator:", "; ".join(communicator.invoke_action("get_status", "").split("; ")[:2]))

        resets = int(re.search(r"(\d+) resets", supervisor.invoke_action("get_status", "")).group(1))
        assert resets == resets_before == 0, f"the supervisor counts {resets} resets"
        assert b"ALARM: SAFETY MODE" not in out, "Safety Mode latched"
        assert b"RADIO link up" not in out, "the communicator reached a link"
        print(f"after {WATCH_S:g} implant hours: {resets} resets, no Safety Mode")
    finally:
        tty1.close()


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, *sys.argv[1:]]))
