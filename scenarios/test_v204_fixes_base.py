#!/usr/bin/env python3
"""v2.04 against the recall: one bedside communicator session on a defective battery must not reset
the device into Safety Mode. On the base firmware it does, so that case fails; on v2.04 the battery
test has disabled wandless telemetry first, and it passes.

One test, run on both levels. A new device goes to the clinic while its battery is healthy (on v2.04
the wand upgrades it); then the cell gets the high-impedance defect and the communicator calls in.
  - base (fails): the radio's milliamps sag the rail below the supervisor's 2.325 V, the chip resets,
    the firmware retries the session, and the third reset within 48 hours latches Safety Mode
    (recall 95969, December 2024).
  - v2.04: the daily battery test (run now with `battest now`) finds the cell and disables wandless
    telemetry; the communicator's wake-up goes unanswered, and for the 48-hour window plus one retry
    there is no reset.

The device's own nightly wakeups are off (`radio wakeup 0`), so this checks the communicator path
only. With them on, v2.04's shipped bug resets the device anyway (test_v205_fixes_v204.py).

    emerson run scenarios/test_v204_fixes_base.py -q -s

Needs emerson-server up on firmware/firmware.bin (`emerson load`), and a session of yours for
`emerson run` to start from; each run starts and stops its own.
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
SCALE = 3600   # implant seconds per guest second
WATCH_S = 52.0  # guest seconds = implant hours after the connect: the 48-hour window plus one 4-hour retry


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


def read_for(tty, out, battery, seconds, stop=None, wall_s=300):
    """Read `tty` into `out` for `seconds` of guest time, or until `stop` appears."""
    start = guest_s(battery)
    deadline = time.monotonic() + wall_s
    while guest_s(battery) - start < seconds and not (stop and stop in out):
        assert time.monotonic() < deadline, "the guest clock stalled"
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


@pytest.mark.parametrize("level", ["base", "v2.04"])
def test_one_communicator_session_on_a_defective_battery(machine, level):
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

        # The clinic visit, while the battery is healthy: on v2.04 the wand installs the update.
        link = pp.WandLink(machine)
        try:
            report = pp.interrogate(pp.Session(link, timeout_s=30), upgrade=None if level == "base" else level)
        finally:
            link.close()
        print(f"\n[{level}] firmware:", pp.FW_LEVEL_NAMES[report["fw"]["level"]])

        # The battery goes bad. v2.04's daily battery test finds it; the base firmware has no such test.
        print(battery.invoke_action("set_defect", "1").split(";")[1].strip())  # R_int ~14 -> ~263 ohm
        if level == "v2.04":
            machine.send_to_broker("tty0", b"battest now\r")
            assert wait_for(tty1, b"ALARM: WANDLESS TELEMETRY DISABLED", out, 60), "the battery test did not disable telemetry"
            print(next(x for x in out.decode(errors="replace").splitlines() if x.startswith("ALARM: WANDLESS")))

        # The bedside communicator calls in.
        mark = len(out)
        communicator = machine.get_device("/patient/communicator")
        communicator.invoke_action("start_session", "")
        read_for(tty1, out, battery, WATCH_S, stop=b"ALARM: SAFETY MODE")
        after = bytes(out[mark:])
        resets = int(re.search(r"(\d+) resets", supervisor.invoke_action("get_status", "")).group(1))
        latched = b"ALARM: SAFETY MODE" in after
        print(f"supervisor resets {resets}, Safety Mode {'LATCHED' if latched else 'no'}")

        # The requirement: one radio session on a bad battery must not reset the device into Safety Mode.
        assert resets == 0 and not latched, \
            f"Safety Mode latched after one communicator session and {resets} supervisor resets"
        if level == "v2.04":
            # Telemetry is disabled, so the communicator never got a link.
            print("communicator:", "; ".join(communicator.invoke_action("get_status", "").split("; ")[:2]))
            assert b"RADIO link up" not in after, "the communicator reached a link"
    finally:
        tty1.close()


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, *sys.argv[1:]]))
