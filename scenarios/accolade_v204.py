#!/usr/bin/env python3
"""The v2.04 mitigation and its two shipped bugs (BSC ACCOLADE physician letter, Sept 2025: the "Model 3869
v2.04 software update", Appendix A "Description of Software Enhancement"), as a timeline.

The recall's defect (a high-impedance Li/CFx cell, stage 1) walks a device from ~3.6 years of longevity left
toward replacement. Four sub-scenarios, each a fresh device and a fresh session, run on their own with `--only`:

  a  BASE firmware: remote-monitoring wakeups reset the defective cell, three resets in 48 h latch Safety Mode.
     The reference date (the stage-4 result, now along a long trajectory instead of a single point).
  b  v2.04, upgraded at the start: the daily battery test raises Code-1003 (red alert) at least three months
     before (a)'s latch, then disables wandless telemetry; the NFC wand still works. Shipped bug 1: the
     automatic low-power wakeups are not disabled, so as the impedance keeps climbing they still reset the
     device, and it still latches Safety Mode -- later than (a), or this model says honestly that it does not.
  c  the FIXED firmware: same trajectory, Code-1003, telemetry off, the radio unpowered: no wakeups, no resets,
     no Safety Mode through ~1 year left.
  d  shipped bug 2 (HYPOTHESIS: the letter gives no mechanism): a HEALTHY cell, v2.04, a magnet over the device
     across the daily test -> wandless telemetry disabled and an explant indicator dated 2000-01-01. With
     `magnet off`: no false positive. On the fixed firmware: none, and an upgrade to it re-enables the radio.

    python3 accolade_v204.py                 # all four (about 25 minutes of wall time)
    python3 accolade_v204.py --only a        # one, or a list: --only b,c
    python3 accolade_v204.py --only d

The cell has to age over implant MONTHS, so the script fast-forwards: it raises BOTH the firmware's implant
clock (`timescale <n>` on the console) and the cell's aging factor (`/board/battery set_aging_factor <n>`) to
the same `--ff-scale` (they must always match, or dates and wear disagree), then returns both to `--scale`
(default 3600: 48 implant hours are 48 guest seconds) for the 48-hour windows where wakeups and resets happen.
The radio's charge is not multiplied by the aging factor (implant_cell `aging_exempt`: its activity is
scheduled on the implant clock, so it costs what it costs however the clock is scaled).

It uses the emerson library only (and the clinic programmer, programmer/pacemaker_programmer.py, to upgrade
and read the device). By default it starts its own sessions and stops them at the end. Needs
emerson-server up on firmware/firmware.bin (`emerson load`); run it there with ../emerson-py.sh.
"""

import argparse
import collections
import re
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "programmer"))
sys.path.insert(0, str(HERE))
import pacemaker_programmer as pp  # noqa: E402
import accolade_safety_mode as base  # noqa: E402

PROJECT = base.PROJECT
YEAR_S = 365.25 * 86400
CAPACITY_C = 1.6 * 3600
DEFAULT_LOAD_A = 12.3e-6   # what "years of longevity left" means in this model (implant_cell, stage 1)
DAY_S = 86400


def depletion_pct_for_years(years):
    return 100.0 * (1.0 - years * YEAR_S * DEFAULT_LOAD_A / CAPACITY_C)


def years_for_depletion_pct(pct):
    return (1.0 - pct / 100.0) * CAPACITY_C / DEFAULT_LOAD_A / YEAR_S


class Run(base.Run):
    """base.Run plus what this scenario watches for on telemetry, each event stamped with the implant day
    (when the device printed one) and the cell's longevity left."""

    def __init__(self, machine, scale):
        super().__init__(machine, scale)
        self.tests = []               # (day, quiescent V, projected y, burst V or None, expected V, margin, verdict)
        self.timeline = []            # (label, implant day or None, cell years left, wall s)
        self.seen = set()
        self.refused = 0
        self.last_day = None
        self.eri = False
        self.latched_day = None
        self.magnet_lines = 0
        self.tty0_log = []            # every console line, so a typed command's reply can be waited for
        self.recent = collections.deque(maxlen=40)   # the last telemetry lines, shown if something unexpected happens

    def years_left(self):
        st = self.battery.invoke_action("get_status", "")
        return years_for_depletion_pct(float(re.search(r"([0-9.]+)% discharged", st).group(1)))

    def mark(self, key, text, day=None, once=True):
        if once and key in self.seen:
            return
        self.seen.add(key)
        y = self.years_left()
        self.timeline.append((text, day, y, time.monotonic() - self.wall0))
        self.note(f"{text}  [implant day {day if day is not None else '?'}, cell {y:.2f} y left]")

    trace = False   # --trace: every console and telemetry line that is not a beat marker
    trace_file = None

    def command(self, text, expect, tries=5):
        """Type a console command and wait for its reply (a line matching `expect`). The firmware's console
        has a 16-byte receive FIFO and the daily test holds the main loop for ~40 ms, so a command typed
        into a test can lose characters: retype it until the device has answered as it should."""
        for attempt in range(tries):
            seen = len(self.tty0_log)
            self.type(text)
            end = time.monotonic() + 2.0
            while time.monotonic() < end:
                self.pump(0.15)
                if any(re.search(expect, x) for x in self.tty0_log[seen:]):
                    return True
            self.note(f"console command `{text}` was not answered as expected (attempt {attempt + 1}); retyping")
        raise RuntimeError(f"the device never answered `{text}` with /{expect}/")

    def line(self, port, text):
        if not text.startswith(("VP ", "VS ", "AS ", "AP ", "AR ", "VR ", "[status]")):
            self.recent.append(f"{port}: {text[:160]}")
        if port == "tty0":
            self.tty0_log.append(text)
        if self.trace_file and not text.startswith(("VP ", "VS ", "AS ", "AP ", "AR ", "VR ", "[status]")):
            self.trace_file.write(f"[{time.monotonic() - self.wall0:7.2f}] {port}: {text[:220]}\n")
            self.trace_file.flush()
        if self.trace and not text.startswith(("VP ", "VS ", "AS ", "AP ", "AR ", "VR ", "BATTEST", "[status]")):
            print(f"      {port}: {text[:200]}", flush=True)
        if port == "tty0" and "reset cause:" in text:
            day = re.search(r"implant day (\d+)", text)
            if day:
                self.last_day = int(day.group(1))
        super().line(port, text)
        if port != "tty1":
            return
        m = re.match(r"BATTEST #(\d+) day (\d+): quiescent ([0-9.]+) V -> projected ([0-9.]+) y; idle ([0-9.]+) V, "
                     r"burst (\(no sample\)|[0-9.]+) V vs expected ([0-9.]+) V, margin (\d+) mV: ([A-Z ]+)", text)
        if m:
            self.last_day = int(m.group(2))
            burst = None if m.group(6).startswith("(") else float(m.group(6))
            self.tests.append((int(m.group(2)), float(m.group(3)), float(m.group(4)), burst, float(m.group(7)),
                               int(m.group(8)), m.group(9).strip()))
            return
        if text.startswith("ALARM: Code-1003"):
            self.mark("code1003", "Code-1003 (voltage too low for projected remaining longevity), red alert",
                      self.last_day)
        elif text.startswith("ALARM: WANDLESS TELEMETRY DISABLED"):
            self.mark("tele_off", "wandless telemetry DISABLED by the battery test", self.last_day)
        elif text.startswith("ALARM: EXPLANT INDICATOR"):
            self.mark("explant", "explant indicator raised (battery not at ERI; no date written)", self.last_day)
        elif text.startswith("RADIO link refused: wandless telemetry is disabled"):
            self.refused += 1
        elif text.startswith("ALARM: ERI"):
            self.eri = True
            if "tele_off" not in self.seen and "eri_dump" not in self.seen:
                self.seen.add("eri_dump")
                print("  UNEXPECTED ERI before the telemetry shut-off; recent device output:\n    " + "\n    ".join(self.recent))
            self.mark("eri", "ALARM: ERI (elective replacement indicator)", self.last_day)
        elif text.startswith("ALARM: SAFETY MODE -- latched at implant s"):
            s = int(re.search(r"implant s (\d+)", text).group(1))
            self.latched_day = s // DAY_S
            self.mark("latched", "SAFETY MODE latched", self.latched_day)
        elif text.startswith("FIRMWARE level"):
            self.note(text)
        elif text.startswith("MAGNET on"):
            self.magnet_lines += 1


# --- scale control ----------------------------------------------------------------------------------

def go_scale(run, scale):
    """The implant clock and the cell's aging factor move together, always."""
    run.command(f"timescale {scale}", rf"time scale {scale} implant")
    run.battery.invoke_action("set_aging_factor", str(scale))
    run.scale = scale


def fast_forward(run, args, to_years, stop=None):
    """Raise both clocks to --ff-scale until the cell has `to_years` of longevity left (or `stop()`), then back."""
    go_scale(run, args.ff_scale)
    deadline = time.monotonic() + args.wall_limit
    try:
        while time.monotonic() < deadline:
            run.pump(0.4)
            if run.years_left() <= to_years or (stop and stop()):
                break
    finally:
        go_scale(run, args.scale)


def probe(run, args, until=None, max_guest_s=14.0):
    """Hourly remote-monitoring wakeups at the scenario scale until one has run its course (the link or session
    closed, the device reset, or `until()`), then back to the daily schedule. Returns the resets it saw."""
    go_scale(run, args.scale)
    before, wakes0 = run.resets, run.wakeups
    closed0 = len([e for e in run.events if "closed" in e[2]]) + run.refused
    run.command("radio wakeup 3600", r"every 3600 implant s")
    start = run.guest_s()
    try:
        while run.guest_s() - start < max_guest_s:
            run.pump(0.4)
            now_closed = len([e for e in run.events if "closed" in e[2]]) + run.refused
            if run.resets > before or run.latched or run.eri or (until and until()):
                break
            if run.wakeups > wakes0 and now_closed > closed0:
                break
    finally:
        run.command("radio wakeup 86400", r"every 86400 implant s")
    return run.resets - before


def latch_run(run, args, label_hint="", max_guest_s=120.0):
    """After the first reset: hourly wakeups (the radio retries four hours after a failed one) until the
    third reset within 48 implant hours latches Safety Mode, or the cell reaches ERI."""
    run.command("radio wakeup 3600", r"every 3600 implant s")
    start = run.guest_s()
    while run.guest_s() - start < max_guest_s and not run.latched and not run.eri:
        run.pump(0.5)
    run.command("radio wakeup 86400", r"every 86400 implant s|automatic wakeups off")


# --- the device and the patient --------------------------------------------------------------------------

def new_device(run, args, l0, defect=True, upgrade=None, scale=None):
    """A fresh device with `l0` years of longevity left, a defective cell (or not), a pacemaker-dependent patient."""
    m = run.m
    m.go()
    run.pump(2.5)  # let the firmware boot
    go_scale(run, scale or args.scale)
    run.battery.invoke_action("set_depletion", f"{depletion_pct_for_years(l0):.4f}")
    run.battery.invoke_action("set_defect", "1" if defect else "0")
    run.battery.invoke_action("reset_min", "")
    run.heart.invoke_action("set_av_block", "third")        # complete block, no escape: a reset is a pacing gap
    run.heart.invoke_action("set_escape_rate", "0")
    run.heart.invoke_action("reset_gap_stats", "")
    run.pump(1.0)
    print(f"  cell: {run.battery.invoke_action('get_status', '')[:150]}")
    if upgrade:
        r = read(run, "wand", upgrade=upgrade)
        print(f"  upgraded through the wand: firmware level now {pp.FW_LEVELS[r['fw']['level']]} "
              f"(a telemetry request; the real upgrade is a download)")
    run.last_day = 0


def read(run, link, **kw):
    """The clinic's programmer, through the wand or the communicator."""
    machine = run.m
    kind = pp.CommunicatorLink if link == "communicator" else pp.WandLink
    ln = kind(machine)
    try:
        return pp.interrogate(pp.Session(ln, timeout_s=kw.pop("timeout", 30)), **kw)
    finally:
        ln.close()


def read_retry(run, link, tries=4, **kw):
    for attempt in range(tries):
        run.pump(2.0)
        try:
            return read(run, link, **kw)
        except (RuntimeError, TimeoutError) as err:
            print(f"  {link} attempt {attempt + 1} failed ({str(err)[:70]})")
    return None


def show_report(r, heading):
    print(f"\n{heading}\n")
    text = pp.summary_text(r)
    print(text.split("\nEpisodes")[0])


def gaps(run):
    v = base.heart_view(run)
    return f"longest ventricular gap {v['longest']} ms, {v['over2']} over 2 s, {v['over3']} over 3 s"


def check_radio(run):
    """The two telemetry paths on a device that has disabled wandless telemetry."""
    try:
        read(run, "communicator", timeout=10)
        return False, "the communicator session COMPLETED"
    except (RuntimeError, TimeoutError) as err:
        why = str(err).split(";")[0][:80]
    wand = read_retry(run, "wand", tries=2)
    return wand is not None, f"communicator refused ({why}); wand {'reads it' if wand else 'FAILED'}"


def table(run):
    rows = run.tests
    if not rows:
        return
    print("\n  daily battery tests (as the device printed them):")
    print("    day  quiescent  projected   burst V   expected   margin  verdict")
    step = max(1, len(rows) // 16)
    shown = None
    for i, (day, vq, proj, burst, exp, margin, verdict) in enumerate(rows):
        # a sample of the daily rows, and every row where the verdict changes
        if i % step and i != len(rows) - 1 and verdict == shown:
            continue
        shown = verdict
        b = f"{burst:.3f}" if burst else "  ---"
        print(f"    {day:4d}   {vq:.3f} V   {proj:5.2f} y    {b}    {exp:.3f}    {margin:4d} mV  {verdict}")


def print_timeline(run, title):
    print(f"\n{title} -- timeline")
    for text, day, y, wall in run.timeline:
        print(f"  implant day {day if day is not None else '?':>5} | cell {y:5.2f} y left | {text}")


# --- (a) ---------------------------------------------------------------------------------------------------

def scenario_a(machine, args):
    run = Run(machine, args.scale)
    out = {"ok": False}
    try:
        print(f"\n=== (a) BASE firmware: cell from {args.l0} y toward replacement, no battery test ===")
        new_device(run, args, args.l0)
        run.command("radio wakeup 86400", r"every 86400 implant s")
        level = args.l0 - 0.3
        first_reset_y = None
        while level >= 0.7 and not run.latched:
            fast_forward(run, args, level)
            resets = probe(run, args)
            run.note(f"probe at {run.years_left():.2f} y left: {resets} reset(s)")
            if resets:
                first_reset_y = run.years_left()
                run.mark("reset1", "first uncommanded reset (remote-monitoring wakeup)", run.last_day)
                break
            level -= 0.1
        if run.resets and not run.latched:
            latch_run(run, args)
        if not run.latched:
            print("FAIL: no latch")
            print_timeline(run, "(a)")
            return out
        run.pump(2.0)
        print_timeline(run, "(a) base")
        r = read_retry(run, "wand")
        sf = r["safety"]
        out.update(ok=True, latched_day=run.latched_day, latched_s=sf["latched_s"], first_reset_y=first_reset_y,
                   latched_years=run.years_left(), resets=run.resets)
        print(f"\n(a) reference: first reset at {first_reset_y:.2f} y left, Safety Mode latched on implant day "
              f"{run.latched_day} with {out['latched_years']:.2f} y left ({run.resets} resets); patient: {gaps(run)}")
        show_report(r, "(a) interrogation through the wand")
        return out
    finally:
        run.close()


# --- (b) ---------------------------------------------------------------------------------------------------

def scenario_b(machine, args, ref=None):
    run = Run(machine, args.scale)
    out = {"ok": False}
    try:
        print(f"\n=== (b) v2.04: upgraded at {args.l0} y; battery test, Code-1003, telemetry off, bug 1 ===")
        new_device(run, args, args.l0, upgrade="v2.04")
        run.command("radio wakeup 86400", r"every 86400 implant s")
        # Stage 1 and 2: the daily test, until it has disabled wandless telemetry.
        fast_forward(run, args, 0.5, stop=lambda: "tele_off" in run.seen)
        if "tele_off" not in run.seen:
            print("FAIL: the test never disabled wandless telemetry")
            return out
        y_dis = run.years_left()
        print(f"\n  telemetry disabled with {y_dis:.2f} y left. The two telemetry paths now:")
        ok, detail = check_radio(run)
        print(f"  [{'PASS' if ok else 'FAIL'}] {detail}")
        out["radio_check"] = ok
        run.pump(1.0)
        # Bug 1: the automatic wakeups go on. Probe each step down toward replacement.
        levels = [x for x in (2.8, 2.7, 2.6, 2.5, 2.4, 2.3, 2.2, 2.0, 1.8, 1.6, 1.4, 1.2, 1.0, 0.9, 0.8, 0.7)
                  if x < y_dis - 0.05]
        probes = []
        for lv in levels:
            fast_forward(run, args, lv, stop=lambda: run.eri)
            if run.eri:
                break
            resets = probe(run, args)
            probes.append((lv, resets, run.refused))
            run.note(f"bug-1 probe at {run.years_left():.2f} y left: {resets} reset(s); {run.refused} wakeups hung up on "
                     "(link refused) so far")
            if resets:
                run.mark("reset1", "first uncommanded reset (an automatic wakeup, telemetry disabled)", run.last_day)
                break
        if run.resets and not run.latched and not run.eri:
            latch_run(run, args)
        run.pump(2.0)
        print_timeline(run, "(b) v2.04")
        table(run)
        r = read_retry(run, "wand")
        for text, day, y, _ in run.timeline:
            if text.startswith("Code-1003"):
                out.update(code1003_day=day, code1003_years=y)
            elif text.startswith("wandless telemetry DISABLED"):
                out.update(tele_off_day=day, tele_off_years=y)
        out.update(ok=True, code1003="code1003" in run.seen, tele_off=True, latched=run.latched,
                   latched_day=run.latched_day, eri=run.eri, resets=run.resets, refused=run.refused,
                   latched_years=run.years_left() if run.latched else None)
        if run.latched:
            print(f"\n(b) bug 1 reproduced: the wakeups the telemetry shut-off left running reset the device after all "
                  f"({run.resets} resets, {run.refused} earlier wakeups hung up on); Safety Mode on implant day "
                  f"{run.latched_day} with {run.years_left():.2f} y left")
        elif run.eri:
            print(f"\n(b) bug 1 did NOT latch Safety Mode before ERI in this model: {run.resets} resets, {run.refused} "
                  "wakeups were refused cleanly; the cell reached ERI (wandless telemetry is switched off there anyway)")
        else:
            print(f"\n(b) no latch within the probes; {run.resets} resets")
        print(f"    patient: {gaps(run)}")
        if r:
            show_report(r, "(b) interrogation through the wand")
        return out
    finally:
        run.close()


# --- (c) ---------------------------------------------------------------------------------------------------

def scenario_c(machine, args):
    run = Run(machine, args.scale)
    out = {"ok": False}
    try:
        print(f"\n=== (c) FIXED firmware: upgraded at {args.l0} y; Code-1003, telemetry off, radio unpowered ===")
        new_device(run, args, args.l0, upgrade="fixed")
        run.command("radio wakeup 86400", r"every 86400 implant s")
        fast_forward(run, args, 0.5, stop=lambda: "tele_off" in run.seen)
        if "tele_off" not in run.seen:
            print("FAIL: the test never disabled wandless telemetry")
            return out
        y_dis = run.years_left()
        print(f"\n  telemetry disabled with {y_dis:.2f} y left. The radio and the two telemetry paths now:")
        sw = run.m.get_device("/board/radio_vsup").invoke_action("get_status", "")
        print("  load switch:", sw[:90])
        ok, detail = check_radio(run)
        print(f"  [{'PASS' if ok else 'FAIL'}] {detail}")
        out["radio_check"] = ok
        wakes0 = run.wakeups
        for lv in [x for x in (2.4, 1.8, 1.4, 1.0) if x < y_dis]:
            fast_forward(run, args, lv, stop=lambda: run.eri)
            resets = probe(run, args, max_guest_s=6.0)
            run.note(f"probe at {run.years_left():.2f} y left: {resets} reset(s), {run.wakeups - wakes0} wakeups")
        draws = run.battery.invoke_action("get_rail", "")
        print("  rail draws:", re.search(r"draws?:? ?(.*?);", draws).group(1) if re.search(r"draws?:? ?(.*?);", draws) else draws[:100])
        run.pump(2.0)
        print_timeline(run, "(c) fixed")
        table(run)
        r = read_retry(run, "wand")
        ok = (not run.latched and run.resets == 0 and run.wakeups == wakes0 and "code1003" in run.seen
              and run.years_left() <= 1.05)
        out.update(ok=ok, resets=run.resets, wakeups=run.wakeups - wakes0, latched=run.latched,
                   years=run.years_left())
        print(f"\n(c) fixed: {run.resets} resets, {run.wakeups - wakes0} wakeups after the shut-off, latched={run.latched}, "
              f"{run.years_left():.2f} y left; patient: {gaps(run)}  ->  {'PASS' if ok else 'FAIL'}")
        if r:
            show_report(r, "(c) interrogation through the wand")
        return out
    finally:
        run.close()


# --- (d) ---------------------------------------------------------------------------------------------------

def wait_for_test(run, n, wall=90.0):
    """Pump until the device's n-th BATTEST line of this run has been printed."""
    end = time.monotonic() + wall
    while len(run.tests) < n and time.monotonic() < end:
        run.pump(0.4)
    return len(run.tests) >= n


def magnet_case(machine, args, name, upgrade, magnet_response, expect_fp):
    run = Run(machine, args.scale)
    res = {"name": name}
    try:
        print(f"\n--- (d) {name} ---")
        new_device(run, args, args.d_years, defect=False, upgrade=upgrade, scale=args.d_scale)
        # No automatic remote-monitoring wakeups: a session would hold the radio when the test falls due.
        run.command("radio wakeup 0", r"automatic wakeups off")
        if not magnet_response:
            run.command("magnet off", r"magnet=off")
        # The wand that upgraded the device carried a magnet of its own, and a magnet that never leaves the Hall
        # switch is read as the wand: give the switch time to see it go before the clinic magnet goes on.
        run.pump(2.0)
        magnet = run.m.get_device("/patient/magnet")
        run.magnet_lines = 0
        magnet.invoke_action("apply_magnet", "")
        run.note("magnet applied over the device across the daily test")
        run.pump(1.0)
        print(f"  Hall switch: {'MAGNET on (the response is in force)' if run.magnet_lines else 'no MAGNET line (response off)'}")
        got = wait_for_test(run, 1, wall=45.0 if upgrade != "fixed" else 22.0)
        run.pump(1.5)
        fp = "tele_off" in run.seen
        during = len(run.tests)
        magnet.invoke_action("remove_magnet", "")
        run.note(f"magnet removed ({during} daily test(s) ran while it was over the device)")
        if upgrade == "fixed":
            # No test under a magnet: the test is due, deferred; it runs once the magnet is gone.
            deferred = not got
            got = wait_for_test(run, 1, wall=45.0)
            run.pump(1.0)
            res["deferred"] = deferred
        if magnet_response:
            run.pump(1.0)
        ok, detail = check_radio(run) if fp else (None, "")
        r = read_retry(run, "wand")
        fw = r["fw"]
        res.update(fp=fp, tests=len(run.tests), tests_during=during, explant=fw["explant"], explant_date=pp.explant_date(fw) if fw["explant"] else None,
                   tele_off=fw["tele_off"], verdict=run.tests[-1][6] if run.tests else None,
                   telemetry_ok=None if ok is None else ok)
        good = (fp == expect_fp) and (not expect_fp or (fw["explant"] and pp.explant_date(fw) == "2000-01-01"))
        res["ok"] = good
        print(f"  false positive: {fp} (expected {expect_fp}); tests during the magnet {during}, in all {len(run.tests)}; "
              f"last verdict: {res['verdict']}; "
              f"explant indicator {'reached ' + str(res['explant_date']) if fw['explant'] else 'not set'}; "
              f"wandless telemetry {'DISABLED' if fw['tele_off'] else 'enabled'}"
              + (f"; {detail}" if detail else "") + f"  ->  {'PASS' if good else 'FAIL'}")
        res["report"] = r
        res["run"] = run
        return res
    except BaseException:
        run.close()
        raise


def scenario_d(conn, args):
    out = {"ok": True, "cases": []}
    print(f"\n=== (d) shipped bug 2 (HYPOTHESIS): a magnet across the daily test, healthy cell at {args.d_years} y, "
          f"x{args.d_scale} clock ===")
    print("  Mechanism (not in the letter, modelled): with the magnet response in force the test's RF burst is held off\n"
          "  like a link next to a wand; the sample the evaluation reads stays at its cleared 0 mV and v2.04 does not\n"
          "  check that one was taken. Nothing is wrong with the cell.")
    cases = [("v2.04, magnet response ON, magnet over the device", "v2.04", True, True),
             ("v2.04, `magnet off`, magnet over the device", "v2.04", False, False),
             ("FIXED, magnet response ON, magnet over the device", "fixed", True, False)]
    first = None
    for name, upgrade, resp, expect in cases:
        token = conn.run_project(PROJECT)
        try:
            with conn.attach(token) as machine:
                res = magnet_case(machine, args, name, upgrade, resp, expect)
                out["cases"].append(res)
                out["ok"] = out["ok"] and res.get("ok", False)
                if first is None and res.get("fp"):
                    first = res
                    # the same device, upgraded to the fixed level: the false positive is cleared
                    run = res["run"]
                    print("\n  the false-positive device, upgraded to the FIXED level (a telemetry request over the wand):")
                    r = read(run, "wand", upgrade="fixed")
                    run.pump(3.0)
                    fw = r["fw"]
                    try:
                        read(run, "communicator", timeout=25)
                        comm = "the communicator session COMPLETES again"
                        comm_ok = True
                    except (RuntimeError, TimeoutError) as err:
                        comm, comm_ok = f"communicator still refused ({str(err)[:60]})", False
                    good = not fw["tele_off"] and not fw["explant"] and fw["cleared"] and comm_ok
                    print(f"  level {pp.FW_LEVELS[fw['level']]}; wandless telemetry {'DISABLED' if fw['tele_off'] else 'enabled'}; explant "
                          f"indicator {'set' if fw['explant'] else 'cleared'}; {comm}  ->  {'PASS' if good else 'FAIL'}")
                    out["ok"] = out["ok"] and good
                    out["cleared"] = good
                    show_report(res["report"], "(d) the false-positive device, as the programmer showed it before the upgrade")
        finally:
            for res in out["cases"]:
                if res.get("run"):
                    res["run"].close()
                    res["run"] = None
            try:
                conn.stop_project(token)
            except RuntimeError:
                pass
    return out


# --- main ---------------------------------------------------------------------------------------------------

def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--host", default="http://localhost:10314")
    p.add_argument("--only", default="a,b,c,d", help="sub-scenarios to run, e.g. a,b (default all)")
    p.add_argument("--scale", type=int, default=3600, help="implant s per guest s for the 48-hour windows (default %(default)s)")
    p.add_argument("--ff-scale", type=int, default=800000,
                   help="implant s per guest s while the cell ages over months (default %(default)s; a day is 108 ms)")
    p.add_argument("--l0", type=float, default=3.6, help="years of longevity left at the start of a-c (default %(default)s)")
    p.add_argument("--d-years", type=float, default=3.0, help="longevity left of the healthy cell in d (default %(default)s)")
    p.add_argument("--d-scale", type=int, default=14400, help="implant clock for d (a day is 6 guest s; default %(default)s)")
    p.add_argument("--wall-limit", type=float, default=900.0, help="seconds of wall time per fast-forward (default %(default)s)")
    p.add_argument("--trace", action="store_true", help="print every console and telemetry line that is not a beat marker")
    p.add_argument("--trace-file", default=None, help="write the same lines to this file, quietly")
    p.add_argument("--ref-latch-day", type=int, default=None,
                   help="when (a) is not run: the base firmware's latch day to compare (b)'s Code-1003 with")
    args = p.parse_args(argv)
    todo = [x.strip() for x in args.only.split(",") if x.strip()]
    Run.trace = args.trace
    Run.trace_file = open(args.trace_file, "w") if args.trace_file else None

    from emerson import EmulatorController
    t0 = time.monotonic()
    results = {}
    walls = {}
    with EmulatorController(args.host).connect() as conn:
        for key in todo:
            t1 = time.monotonic()
            if key == "d":
                results[key] = scenario_d(conn, args)
            else:
                token = conn.run_project(PROJECT)
                try:
                    with conn.attach(token) as machine:
                        fn = {"a": scenario_a, "b": scenario_b, "c": scenario_c}[key]
                        results[key] = fn(machine, args)
                finally:
                    try:
                        conn.stop_project(token)
                    except RuntimeError:
                        pass
            walls[key] = time.monotonic() - t1
            print(f"\n(sub-scenario {key}: wall {walls[key]:.0f} s)")

    print("\n" + "=" * 100 + "\nSUMMARY")
    ok = True
    a, b, c, d = (results.get(k) for k in "abcd")
    if a:
        print(f"(a) base:  Safety Mode latched on implant day {a.get('latched_day')} with {a.get('latched_years', float('nan')):.2f} y left "
              f"(first reset at {a.get('first_reset_y') or float('nan'):.2f} y)")
        ok = ok and a["ok"]
    if b:
        print(f"(b) v2.04: Code-1003 raised: {b.get('code1003')}; telemetry disabled; "
              + (f"bug 1: Safety Mode latched on day {b['latched_day']} with {b['latched_years']:.2f} y left" if b.get("latched")
                 else "bug 1: no latch before ERI" if b.get("eri") else "bug 1: no latch in the probes"))
        ok = ok and b["ok"] and b.get("code1003") and b.get("radio_check")
    if c:
        print(f"(c) fixed: {c.get('resets')} resets, {c.get('wakeups')} wakeups, latched={c.get('latched')}  ->  {'PASS' if c['ok'] else 'FAIL'}")
        ok = ok and c["ok"]
    if d:
        print("(d) magnet false positive: " + "; ".join(f"{x['name'].split(',')[0]}/{x['name'].split(',')[1].strip()}: fp={x.get('fp')} "
                                                       f"{'PASS' if x.get('ok') else 'FAIL'}" for x in d["cases"])
              + f"; upgrade clears it: {d.get('cleared')}")
        ok = ok and d["ok"]
    ref_day = a["latched_day"] if a and a.get("ok") else args.ref_latch_day
    ref_years = a["latched_years"] if a and a.get("ok") else None
    if b and b.get("code1003_day") is not None and ref_day is not None:
        lead = ref_day - b["code1003_day"]
        verdict = lead >= 90
        print(f"Code-1003 (b, implant day {b['code1003_day']}, {b['code1003_years']:.2f} y left) came {lead} implant days "
              f"({lead / 30.44:.1f} months) before the base firmware's Safety Mode latch (a, day {ref_day}"
              + (f", {ref_years:.2f} y left" if ref_years else "") + f"): the letter's >= 3 months  ->  {'PASS' if verdict else 'FAIL'}")
        if b.get("tele_off_day") is not None:
            print(f"telemetry was disabled on day {b['tele_off_day']} ({b['tele_off_years']:.2f} y left): {ref_day - b['tele_off_day']} implant days "
                  f"before that latch, {b['tele_off_day'] - b['code1003_day']} days after the Code-1003")
        ok = ok and verdict
    print(f"\ntotal wall time {time.monotonic() - t0:.0f} s ({', '.join(f'{k} {v:.0f} s' for k, v in walls.items())})")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
