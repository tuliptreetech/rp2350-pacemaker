#!/usr/bin/env python3
"""The ACCOLADE Safety Mode recall, short form: one bedside-communicator connect on a defective cell
latches Safety Mode (BSC ACCOLADE physician letter, Sept 2025, Appendix A / Table 3).

The long form (accolade_safety_mode.py) waits for the implant's own daily remote-monitoring wakeup.
Here the communicator wakes it instead (`/patient/communicator start_session`): the radio's milliamps
sag the high-impedance cell below the supply supervisor's 2.325 V, RUN falls and the device resets.
The firmware treats the cut-off session like any failed wakeup and retries it 4 implant hours later,
and each retry resets it again, so the one connect is enough: the third reset within 48 hours latches
Safety Mode. The script then measures the latched state the way the long form does, and reads it back
through the console and the wand.

    python3 accolade_communicator.py             # the connect, the resets, the latch, the checks
    python3 accolade_communicator.py --healthy   # control: no defect; the session completes, no reset

The setup is the shortest that reproduces it:
  - `timescale 3600` on the console, so 48 implant hours are 48 guest seconds (at the default
    1,000,000x they are 0.17 s, too short for three resets, and the retry is never scheduled);
  - the cell aged at the same rate, ~2 years from replacement (the defect adds resistance only
    within about 4 years of it) and the defect on: R_int ~14 -> ~263 ohm;
  - complete AV block and no escape rhythm, so every beat after the latch is paced and its interval
    measures the Safety Mode rate.
Then it waits a few guest seconds before the connect: the firmware writes the time scale to its EEPROM
in the background, behind the whole freshly formatted store, and a reset before that lands boots the
device at the default scale again.

It uses the emerson library only, and the long form's helpers. By default it starts its own session of
the project and stops it at the end; `--session TOKEN` runs it in a session you already have (which it
resets first, blanking the EEPROM: a new device). Run it from the repo root with
`emerson run scenarios/accolade_communicator.py`.
"""

import argparse
import os
import re
import sys
import time

import accolade_safety_mode as recall
from accolade_safety_mode import pp

SAFETY_WINDOW_S = 48 * 3600  # implant seconds (firmware/main.c SAFETY_WINDOW_S)
SAFETY_RESETS = 3


class CommRun(recall.Run):
    """The long form's Run, also keeping the console's lines, the time scale each boot reports, and
    the guest time of each supervisor reset."""

    def __init__(self, machine, scale):
        super().__init__(machine, scale)
        self.lines0 = []
        self.boot_scales = []
        self.reset_guest = []
        self.communicator = machine.get_device("/patient/communicator")

    def line(self, port, text):
        if port == "tty0" and text:
            self.lines0.append(text)
            if "reset cause:" in text:
                m = re.search(r"time scale (\d+)", text)
                self.boot_scales.append(int(m.group(1)) if m else None)
        resets = self.resets
        super().line(port, text)
        if self.resets > resets:
            self.reset_guest.append(self.events[-1][1])

    def console(self, command, wait_s=1.0):
        """Type `command` and return the console's lines that answer it."""
        self.lines0 = []
        self.type(command)
        self.pump(wait_s)
        return self.lines0


def setup(run, args):
    m = run.m
    m.go()
    run.pump(2.5)  # let the firmware boot
    print(f"setup: implant clock {args.scale} implant s per guest s, cell aging factor {args.scale}")
    run.type(f"timescale {args.scale}")
    run.battery.invoke_action("set_aging_factor", str(args.scale))
    run.battery.invoke_action("set_depletion", str(args.depletion))
    run.battery.invoke_action("set_defect", "0" if args.healthy else "1")
    run.heart.invoke_action("set_av_block", "third")
    run.heart.invoke_action("set_escape_rate", "0")
    print("  cell:", run.battery.invoke_action("get_status", ""))
    # The time scale reaches the EEPROM behind the freshly formatted store.
    recall.wait_guest(run, args.settle_s)
    run.battery.invoke_action("reset_min", "")


def connect(run):
    """The communicator's base-initiated wake-up; returns its guest time."""
    run.note("communicator: base-initiated wake-up (start_session)")
    run.communicator.invoke_action("start_session", "")
    return run.guest_s()


def watch(run, args, until):
    """Pump until `until()` or `args.max_guest_s` of guest time (or the wall timeout)."""
    start = run.guest_s()
    deadline = time.monotonic() + args.wall_timeout
    while time.monotonic() < deadline:
        run.pump(0.5)
        if until() or run.guest_s() - start > args.max_guest_s:
            return


def healthy(run, args):
    """The control: the same connect on a healthy cell reaches a link and resets nothing."""
    wakeups = run.wakeups
    t_connect = connect(run)
    watch(run, args, lambda: run.guest_s() - t_connect >= args.span_s)
    run.communicator.invoke_action("end_session", "")
    checks = [
        ("no automatic wakeup before the connect", wakeups == 0, f"{wakeups} wakeups"),
        ("the communicator's session reached a link", run.links >= 1, f"{run.links} radio links"),
        ("no supervisor reset", run.resets == 0, f"{run.resets} resets"),
        ("not latched", not run.latched, f"{run.alarms} SAFETY MODE alarms"),
    ]
    status = " ".join(run.console("status"))
    checks.append(("console: no Safety Mode", "SAFETY MODE" not in status, status[:120]))
    return checks


def defect(run, args):
    wakeups = run.wakeups
    t_connect = connect(run)
    watch(run, args, lambda: run.latched)
    if not run.latched:
        return [("Safety Mode latched", False,
                 f"no latch within {args.max_guest_s:g} guest s: {run.resets} resets, {run.wakeups} wakeups")]
    resets_at_latch = run.resets
    t = run.reset_guest
    span = (t[SAFETY_RESETS - 1] - t[0]) * run.scale if len(t) >= SAFETY_RESETS else None
    checks = [
        ("no automatic wakeup before the connect", wakeups == 0, f"{wakeups} wakeups"),
        ("the connect itself reset the device", bool(t) and t[0] - t_connect < 3.0,
         f"first reset {t[0] - t_connect:.2f} guest s after the wake-up" if t else "no reset"),
        ("latched at the third supervisor reset", resets_at_latch == SAFETY_RESETS,
         f"{resets_at_latch} resets at the latch, {run.wakeups - wakeups} retries between them"),
        ("the three resets fall within 48 implant hours", span is not None and span <= SAFETY_WINDOW_S,
         f"{span / 3600:.1f} implant h from the first to the third" if span is not None else "too few resets"),
        (f"every reboot kept the {run.scale}x time scale", run.boot_scales and set(run.boot_scales) == {run.scale},
         f"boots at {run.boot_scales}"),
    ]
    print(f"\nlatched after {resets_at_latch} supervisor resets. Verifying the device's state:")
    checks += recall.verify_latched(run)
    status = " ".join(run.console("status"))
    checks.append(("console `status`: SAFETY MODE, VVI 72.5 ppm, 5.000 V @ 1000 us, 0.25 mV, magnet off",
                   all(s in status for s in ("SAFETY MODE", "mode=VVI", "rate=72.5 ppm", "pw=1000 us",
                                             "amp=5.000 V", "sens=0.25 mV", "magnet=off")), status[:160]))
    refused = " ".join(run.console("rate 80"))
    checks.append(("console refuses `rate 80`", "not programmable" in refused, refused[:100]))
    try:
        r = recall.interrogate(run, "wand")
        checks.append(("wand interrogation: Safety Mode latched", bool(r["safety"] and r["safety"]["latched"]),
                       pp.summary_text(r).split("\n")[0]))
    except (RuntimeError, TimeoutError) as err:
        checks.append(("wand interrogation: Safety Mode latched", False, str(err)))
    return checks


def scenario(machine, args):
    run = CommRun(machine, args.scale)
    try:
        setup(run, args)
        print("\ntimeline" + (" (healthy control)" if args.healthy else ""))
        checks = healthy(run, args) if args.healthy else defect(run, args)
        print()
        for name, ok, detail in checks:
            print(f"  [{'PASS' if ok else 'FAIL'}] {name}: {detail}")
        ok = all(ok for _, ok, _ in checks)
        what = "healthy control" if args.healthy else "one communicator connect latched Safety Mode"
        print(f"\nRESULT: {'PASS' if ok else 'FAIL'}: {what}")
        return 0 if ok else 1
    finally:
        run.close()


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--host", default=os.environ.get("EMERSON_HOST", "http://localhost:10314"),
                   help="franklin server (default: EMERSON_HOST, else %(default)s)")
    p.add_argument("--session", default=None, metavar="TOKEN", help="use this session (it is reset first)")
    p.add_argument("--healthy", action="store_true", help="control: no battery defect; the session completes")
    p.add_argument("--scale", type=int, default=3600, help="implant s per guest s (default %(default)s: 48 h = 48 s)")
    p.add_argument("--depletion", type=float, default=recall.TWO_YEARS_LEFT_PCT,
                   help="%% of the cell used (default %(default)s: ~2 years left)")
    p.add_argument("--settle-s", type=float, default=5.0,
                   help="guest seconds between the setup and the connect (default %(default)s): the time scale "
                        "reaching the EEPROM")
    p.add_argument("--span-s", type=float, default=10.0, help="--healthy: guest seconds to watch (default %(default)s)")
    p.add_argument("--max-guest-s", type=float, default=60.0,
                   help="guest seconds to wait for the latch (default %(default)s)")
    p.add_argument("--wall-timeout", type=float, default=300.0)
    args = p.parse_args(argv)

    from emerson import EmulatorController
    t0 = time.monotonic()
    own = args.session is None
    with EmulatorController(args.host).connect() as conn:
        token = conn.run_project(recall.PROJECT) if own else args.session
        try:
            with conn.attach(token) as machine:
                if not own:
                    machine.reset()
                code = scenario(machine, args)
        finally:
            if own:
                try:
                    conn.stop_project(token)
                except RuntimeError:
                    pass
    print(f"\nwall time {time.monotonic() - t0:.0f} s")
    return code


if __name__ == "__main__":
    sys.exit(main())
