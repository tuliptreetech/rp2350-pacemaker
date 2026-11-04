#!/usr/bin/env python3
"""v2.05 against v2.04's bug: after the battery test disables wandless telemetry, the device's own
nightly wakeups must not reset it into Safety Mode. On v2.04 they do, so that case fails; on v2.05 the
radio is unpowered, and it passes.

One test, run on both levels. A new device is upgraded through the wand while its battery is healthy;
then the cell gets the high-impedance defect, the daily battery test (run now with `battest now`)
disables wandless telemetry, and the nightly check-ins go back on (`radio wakeup 86400`: once a day,
24 guest seconds at 3600x).
  - v2.04 (fails): the next nightly wakeup still loads the cell, the chip resets, the retries reset it
    again, and Safety Mode latches (BSC letter, Sept 2025: wakeups "not disabled as intended").
  - v2.05: the test opened the radio's load switch; for 50 implant hours (two nightly check-ins and
    the whole 48-hour window) there is no wakeup and no reset.

    emerson run scenarios/test_v205_fixes_v204.py -q -s

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
WATCH_S = 50.0  # guest seconds = implant hours after the check-ins go back on


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


def read_for(tty, out, battery, seconds, stop=None):
    """Read `tty` into `out` for `seconds` of guest time, or until `stop` appears."""
    start = guest_s(battery)
    while guest_s(battery) - start < seconds and not (stop and stop in out):
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


@pytest.mark.parametrize("level", ["v2.04", "v2.05"])
def test_nightly_wakeups_after_the_telemetry_shutoff(machine, level):
    tty1 = machine.subscribe_broker("tty1")  # telemetry: the marker channel and the alarms
    try:
        out = bytearray()
        machine.go()
        assert wait_for(tty1, b"[telemetry]", out, 30), "the firmware did not boot"

        battery = machine.get_device("/board/battery")
        supervisor = machine.get_device("/board/supervisor")
        machine.send_to_broker("tty0", f"timescale {SCALE}\r".encode())
        machine.send_to_broker("tty0", b"radio wakeup 0\r")  # no check-in before the battery test has run
        battery.invoke_action("set_aging_factor", str(SCALE))
        battery.invoke_action("set_depletion", "86.5")  # ~2 years from replacement
        read_for(tty1, out, battery, 5.0)  # the time scale reaches the EEPROM before anything resets

        # The clinic upgrades the healthy device through the wand.
        link = pp.WandLink(machine)
        try:
            report = pp.interrogate(pp.Session(link, timeout_s=30), upgrade=level)
        finally:
            link.close()
        print(f"\n[{level}] upgraded:", pp.FW_LEVEL_NAMES[report["fw"]["level"]])

        # The battery goes bad, and the daily battery test disables wandless telemetry.
        print(battery.invoke_action("set_defect", "1").split(";")[1].strip())  # R_int ~14 -> ~263 ohm
        machine.send_to_broker("tty0", b"battest now\r")
        assert wait_for(tty1, b"ALARM: WANDLESS TELEMETRY DISABLED", out, 60), "the battery test did not disable telemetry"
        print(next(x for x in out.decode(errors="replace").splitlines() if x.startswith("ALARM: WANDLESS")))

        # The nightly check-ins go back on, as on any real device.
        mark = len(out)
        machine.send_to_broker("tty0", b"radio wakeup 86400\r")
        read_for(tty1, out, battery, WATCH_S, stop=b"ALARM: SAFETY MODE")
        after = bytes(out[mark:])
        wakeups = after.count(b"RADIO wakeup (automatic")
        resets = int(re.search(r"(\d+) resets", supervisor.invoke_action("get_status", "")).group(1))
        latched = b"ALARM: SAFETY MODE" in after
        print(f"nightly wakeups {wakeups}, supervisor resets {resets}, Safety Mode {'LATCHED' if latched else 'no'}")

        # Telemetry is disabled, so nothing may wake the radio. On v2.04 the nightly wakeup still loads the cell.
        assert wakeups == 0 and resets == 0 and not latched, \
            f"Safety Mode latched after {wakeups} nightly wakeups and {resets} supervisor resets"
        if level == "v2.05":
            # The fix: the radio is unpowered, so there is nothing left to wake.
            switch = machine.get_device("/board/radio_vsup").invoke_action("get_status", "")
            print("radio load switch:", switch.split(";")[0])
            assert b"radio unpowered, no wakeups of any kind" in out
            assert "ON low" in switch
    finally:
        tty1.close()


if __name__ == "__main__":
    sys.exit(pytest.main([__file__, *sys.argv[1:]]))
