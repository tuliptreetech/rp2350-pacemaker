#!/usr/bin/env python3
"""Reproduce the ACCOLADE Safety Mode recall end to end (BSC ACCOLADE physician letter, Sept 2025,
Appendix A / Table 3), and print a timeline.

The story: a pacemaker with the high-impedance battery defect, about two years from replacement.
Each remote-monitoring radio wakeup loads the cell with milliamps; the defective cell sags below
the supply supervisor's threshold (2.325 V, TPS3839G25), RUN falls and the device resets. The
wakeup is retried, so the resets come in a row; the third within 48 hours latches Safety Mode:
permanent VVI 72.5 ppm, 5.0 V at 1.0 ms, unipolar, 0.25 mV, magnet response off. A patient with
complete AV block and no escape rhythm is paced through all of it, and misses beats at each reset.

    python3 accolade_safety_mode.py              # the recall (about 2 minutes of wall time)
    python3 accolade_safety_mode.py --healthy    # control: same period, no defect: no resets, no latch
    python3 accolade_safety_mode.py --escape-bpm 35
    python3 accolade_safety_mode.py --no-harm    # stop after the latch checks and the interrogation
    python3 accolade_safety_mode.py --control    # the bipolar control: no defect, no latch, same effort levels

After the latch it runs the harm phase (BSC letter, "Clinical Impact": pacing inhibition due to
myopotential oversensing in unipolar sensing, pauses, syncope): the patient has complete block and no
escape rhythm, so every ventricular beat is a pacemaker beat, and the pectoral arm effort is stepped
through `--efforts`. The table is measured from the heart's own ground truth (longest gap between
ventricular depolarizations, gaps over 2 s and 3 s, pocket stimulations) beside what the device
logged (noise reversion, pause episodes). `--control` repeats the table with the device healthy:
bipolar sensing at its programmed sensitivity, the same patient and the same effort.

It uses the emerson library only. By default it starts its own session of the project and stops it
at the end; `--session TOKEN` runs it in a session you already have (which it resets first, blanking
the EEPROM: a new device). Needs emerson-server up on firmware/firmware.bin (`emerson load`); run it
from the repo root with `emerson run scenarios/accolade_safety_mode.py`, which runs it in a container from
the server's image and sets EMERSON_HOST for `--host`.

Time. The firmware's implant clock runs `--scale` implant seconds per guest second (default 3600,
so 48 implant hours are 48 guest seconds) and the cell's aging factor must be the same number, so
the script sets both: `timescale <n>` on the console and `/board/battery set_aging_factor <n>`.
The checks after the latch read the device back through its wand and its radio exactly as the
clinic's programmer does (programmer/pacemaker_programmer.py).
"""

import argparse
import os
import re
import statistics
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "programmer"))
import pacemaker_programmer as pp  # noqa: E402

PROJECT = pp.DEFAULT_PROJECT
# Arm effort steps for the harm phase; 0.17/0.3/0.5/1.0 are the rows of the stage 5a myopotential table
# (a 100 ms window holds a >0.25 mV sample 39 / 87 / 100 / 100 % of the time).
DEFAULT_EFFORTS = "0,0.17,0.3,0.5,1.0"
# 1.6 Ah cell, ~2 years of default-load service left: 86.5 % discharged (stage 1 notes).
TWO_YEARS_LEFT_PCT = 86.5


class Run:
    """The session, its two serial ports, and a timeline of what happened."""

    def __init__(self, machine, scale):
        self.m = machine
        self.scale = scale
        self.wall0 = time.monotonic()
        self.tty0 = machine.subscribe_broker("tty0")
        self.tty1 = machine.subscribe_broker("tty1")
        self.buf = {"tty0": b"", "tty1": b""}
        self.lines1 = []          # tty1 lines since the last clear_lines()
        self.events = []          # (wall_s, guest_s, text)
        self.resets = 0
        self.wakeups = 0
        self.links = 0
        self.latched = False
        self.alarms = 0           # SAFETY MODE alarm lines on telemetry (one per boot while latched)
        self.battery = machine.get_device("/board/battery")
        self.supervisor = machine.get_device("/board/supervisor")
        self.heart = machine.get_device("/patient/heart")

    def guest_s(self):
        m = re.search(r"at guest ([0-9.]+) s", self.battery.invoke_action("get_rail", ""))
        return float(m.group(1)) if m else float("nan")

    def note(self, text):
        wall = time.monotonic() - self.wall0
        guest = self.guest_s()
        self.events.append((wall, guest, text))
        print(f"[wall {wall:6.1f} s | guest {guest:7.2f} s] {text}", flush=True)

    def type(self, command):
        """A console command, as a clinician types it."""
        self.m.send_to_broker("tty0", command.encode() + b"\r")

    def pump(self, seconds):
        """Read both ports for `seconds` of wall time, noting what matters."""
        end = time.monotonic() + seconds
        while True:
            for name, sub in (("tty0", self.tty0), ("tty1", self.tty1)):
                chunk = sub.read(timeout=0.05)
                if chunk:
                    self.buf[name] += chunk
            for name in ("tty0", "tty1"):
                *done, self.buf[name] = self.buf[name].split(b"\n")
                for raw in done:
                    self.line(name, raw.decode(errors="replace").strip("\r "))
            if time.monotonic() >= end:
                return

    def line(self, port, text):
        if not text:
            return
        if port == "tty1":
            self.lines1.append(text)
        if port == "tty0" and "reset cause:" in text:
            cause = re.search(r"reset cause: ([^;]+);", text)
            what = cause.group(1) if cause else "?"
            if what.startswith("supervisor"):
                self.resets += 1
                rail = self.battery.invoke_action("get_rail", "")
                low = re.search(r"min ([0-9.]+) V", rail)
                self.battery.invoke_action("reset_min", "")
                count = re.search(r"(\d+) resets", self.supervisor.invoke_action("get_status", ""))
                self.note(f"RESET #{self.resets}: supervisor (RUN low), rail minimum {low.group(1) if low else '?'} V"
                          f" (supervisor counts {count.group(1) if count else '?'})")
            else:
                self.note(f"boot: {what}")
        elif port == "tty1":
            if text.startswith("RADIO wakeup (automatic"):
                self.wakeups += 1
                self.note(f"implant-initiated remote-monitoring wakeup #{self.wakeups}")
            elif text.startswith("RADIO link up"):
                self.links += 1
                self.note("radio link up (the communicator answered)")
            elif text.startswith("RADIO session closed") or text.startswith("RADIO link closed"):
                self.note(text.split(" t=")[0])
            elif text.startswith("ALARM: SAFETY MODE"):
                self.alarms += 1
                if self.latched:
                    return
                self.latched = True
                self.note("ALARM on telemetry: " + text.split(": ", 1)[1].split(" Permanent")[0][:150])

    def clear_lines(self):
        self.lines1 = []

    def close(self):
        self.tty0.close()
        self.tty1.close()


def setup(run, args):
    m = run.m
    m.go()
    print(f"setup: implant clock {args.scale} implant s per guest s, cell aging factor {args.scale}")
    run.pump(2.5)  # let the firmware boot
    run.type(f"timescale {args.scale}")
    run.battery.invoke_action("set_aging_factor", str(args.scale))
    run.battery.invoke_action("set_depletion", str(args.depletion))
    if args.healthy:
        run.battery.invoke_action("set_defect", "0")
    else:
        run.battery.invoke_action("set_defect", "1")
    run.battery.invoke_action("reset_min", "")
    h = run.heart
    h.invoke_action("set_av_block", "third")           # complete AV block: nothing conducts
    h.invoke_action("set_escape_rate", str(args.escape_bpm))
    run.pump(1.0)
    print("  cell:", run.battery.invoke_action("get_status", ""))
    print("  heart:", h.invoke_action("get_status", "")[:120], "...")
    print("  patient: complete AV block, escape rhythm",
          "absent" if args.escape_bpm == 0 else f"{args.escape_bpm} bpm", flush=True)


def heart_beats(run):
    m = re.search(r"beats: ([^;]*)", run.heart.invoke_action("get_status", ""))
    return m.group(1) if m else "?"


def wait_for_outcome(run, args):
    start_guest = run.guest_s()
    deadline = time.monotonic() + args.wall_timeout
    while time.monotonic() < deadline:
        run.pump(0.5)
        spent = run.guest_s() - start_guest
        if args.healthy and spent >= args.span_s:
            return True
        if not args.healthy and run.latched:
            return True
        if spent > args.max_guest_s:
            return False
    return False


def verify_latched(run):
    """The state after the latch, measured. Returns a list of (name, ok, detail)."""
    checks = []
    # Windows are guest time, not wall: the emulator's speed depends on the host
    # (about 0.5x wall natively, about 0.23x for the amd64 image on Apple Silicon).
    wait_guest(run, 1.0)
    run.clear_lines()
    wait_guest(run, 4.0)
    vps = [int(x) for x in re.findall(r"^VP t=\S+ interval=(\d+) ms", "\n".join(run.lines1), re.M)]
    med = statistics.median(vps) if vps else 0
    checks.append(("VVI 72.5 ppm (median VP interval 827-828 ms)", 826 <= med <= 829,
                   f"{len(vps)} paces, median {med} ms (LRI 827.6 ms)"))
    pace = run.m.get_device("/board/vpace").invoke_action("get_status", "")
    wid = re.search(r"last width ([0-9.]+) us; output capacitor ([0-9.]+) V", pace)
    checks.append(("5.0 V at 1.0 ms at the switch", bool(wid) and abs(float(wid.group(1)) - 1000) < 5
                   and abs(float(wid.group(2)) - 5.0) < 0.01, wid.group(0) if wid else pace))
    st = run.heart.invoke_action("get_status", "")
    pol = re.search(r"lead polarity: ventricle (\w+)", st)
    pocket0 = int(re.search(r"pocket stimulations (\d+)", st).group(1))
    checks.append(("right ventricular lead unipolar", bool(pol) and pol.group(1) == "unipolar", st[st.find("lead polarity"):][:60]))
    wait_guest(run, 3.0)
    pocket1 = int(re.search(r"pocket stimulations (\d+)", run.heart.invoke_action("get_status", "")).group(1))
    checks.append(("pocket stimulation counter climbing (5 V unipolar)", pocket1 > pocket0 + 1,
                   f"{pocket0} -> {pocket1} in 3 s of guest time"))
    # Magnet: the response is disabled.
    magnet = run.m.get_device("/patient/magnet")
    run.clear_lines()
    magnet.invoke_action("apply_magnet", "")
    wait_guest(run, 2.5)
    magnet.invoke_action("remove_magnet", "")
    seen = [x for x in run.lines1 if "MAGNET" in x]
    vps = [int(x) for x in re.findall(r"^VP t=\S+ interval=(\d+) ms", "\n".join(run.lines1), re.M)]
    checks.append(("magnet does nothing", not seen and all(x > 700 for x in vps),
                   f"{len(seen)} magnet lines, paces at {sorted(set(vps))[:4]} ms"))
    # Console: mode and parameters are refused; reboot does not clear it.
    run.clear_lines()
    alarms = run.alarms
    run.type("mode DDD")
    run.type("amp 1000")
    run.pump(1.5)
    run.type("reboot")
    wait_guest(run, 3.0)
    checks.append(("`reboot` keeps it (the alarm is raised again at the next boot)", run.alarms > alarms,
                   f"{run.alarms - alarms} new SAFETY MODE alarm(s) after the reboot"))
    return checks


def wait_guest(run, seconds, wall_timeout=600.0):
    """Pump the ports until `seconds` of guest time have passed."""
    start = run.guest_s()
    deadline = time.monotonic() + wall_timeout
    while run.guest_s() - start < seconds and time.monotonic() < deadline:
        run.pump(0.5)


GAPS = re.compile(r"ventricular gaps: longest (\d+) ms \(ongoing (\d+) ms\), (\d+) over 2 s, (\d+) over 3 s")  # open gap counted


def heart_view(run):
    st = run.heart.invoke_action("get_status", "")
    g = GAPS.search(st)
    pocket = re.search(r"pocket stimulations (\d+)", st)
    cap = re.search(r"ventricle lead .*?paced (\d+) captured", st)
    return {"longest": int(g.group(1)), "over2": int(g.group(3)), "over3": int(g.group(4)),
            "pocket": int(pocket.group(1)), "captured": int(cap.group(1)),
            "polarity": (re.search(r"lead polarity: ventricle (\w+)", st) or [None, "?"])[1]}


def apply_effort(run, body, effort, seconds, burst):
    """Hold `effort` for `seconds` of guest time: steady, or squeezing `burst` = (on_s, off_s) repeatedly
    (an isometric squeeze, the letter's provocative maneuver, is bursts, not a constant)."""
    if not burst:
        body.invoke_action("set_arm_effort", str(effort))
        wait_guest(run, seconds)
        return
    start = run.guest_s()
    while run.guest_s() - start < seconds:
        body.invoke_action("set_arm_effort", str(effort))
        wait_guest(run, burst[0])
        body.invoke_action("set_arm_effort", "0")
        wait_guest(run, burst[1])


def harm_trial(run, effort, seconds, burst=None):
    """One effort level: ground truth from the heart, what the device logged from telemetry."""
    body = run.m.get_device("/patient/body")
    body.invoke_action("set_arm_effort", "0")
    wait_guest(run, 2.5)   # at rest: the previous level's pause and noise reversion end
    run.heart.invoke_action("reset_gap_stats", "")
    before = heart_view(run)
    run.clear_lines()
    apply_effort(run, body, effort, seconds, burst)
    after = heart_view(run)
    lines = "\n".join(run.lines1)
    pauses = [int(x) for x in re.findall(r"ventricular pause ended after (\d+) ms", lines)]
    return {
        "effort": effort, "burst": burst, "longest": after["longest"], "over2": after["over2"], "over3": after["over3"],
        "pocket": after["pocket"] - before["pocket"], "paced": after["captured"] - before["captured"],
        "vp": len(re.findall(r"^VP t=", lines, re.M)), "vs": len(re.findall(r"^VS t=", lines, re.M)),
        "vr": len(re.findall(r"^VR t=", lines, re.M)),
        "noise_on": len(re.findall(r"^NOISE reversion on", lines, re.M)),
        "dev_pauses": len(re.findall(r"ventricular pause, no VP or VS for over", lines)), "dev_pause_ms": pauses,
        "polarity": after["polarity"],
    }


def pattern_label(r):
    return "steady" if not r["burst"] else f"{r['burst'][0]:g} s on/{r['burst'][1]:g} s off"


def harm_phase(run, efforts, seconds, title, burst):
    """Each effort level steady and (when `burst`) as repeated squeezes."""
    trials = [(0.0, None)] + [(e, None) for e in efforts if e > 0] + ([(e, burst) for e in efforts if e > 0] if burst else [])
    print(f"\n{title}: {seconds:.0f} guest s per trial, patient in complete block with no escape rhythm")
    rows = []
    # The daily remote-monitoring wakeup (a defective cell resets on it) would punch its own gaps in the
    # table; this phase measures sensing, so take the radio's schedule out of it.
    run.type("radio wakeup 0")
    run.pump(1.0)
    try:
        for e, b in trials:
            r = harm_trial(run, e, seconds, b)
            rows.append(r)
            print(f"  effort {e:4.2f} {pattern_label(r):>18}: longest gap {r['longest']} ms, {r['over2']} > 2 s, "
                  f"{r['over3']} > 3 s", flush=True)
    finally:
        run.m.get_device("/patient/body").invoke_action("set_arm_effort", "0")
        run.type("radio wakeup 86400")
        run.pump(0.5)
    print(f"\n{title}  (lead {rows[0]['polarity']}; heart = ground truth, device = its own telemetry)")
    print("  effort pattern            | longest gap | gaps>2 s | gaps>3 s |  VP  VS  VR | noise rev. | dev pauses | pocket")
    for r in rows:
        print(f"   {r['effort']:4.2f} {pattern_label(r):<19}| {r['longest']:7d} ms | {r['over2']:8d} | {r['over3']:8d} | "
              f"{r['vp']:3d} {r['vs']:3d} {r['vr']:3d} | {r['noise_on']:10d} | {r['dev_pauses']:10d} | {r['pocket']:6d}")
    return rows


def report_device_view(run, harm_rows):
    """What the device itself recorded of the harm phase, read through the wand before anything is cleared."""
    r = None
    for attempt in range(3):
        run.pump(2.0)
        try:
            r = interrogate(run, "wand")
            break
        except (RuntimeError, TimeoutError) as err:
            print(f"  wand attempt {attempt + 1} failed ({str(err)[:60]}), retrying")
    if r is None:
        print("could not read the device's own record of the harm phase")
        return
    c = r["counters"]
    eps = [e for e in r["episodes"] if e["kind"] in (pp.EP_NOISE, pp.EP_PAUSE)]
    truth2 = sum(x["over2"] for x in harm_rows)
    print("\nwhat the device recorded (interrogated through the wand, nothing cleared):")
    print(f"  counters: noise reversions {c['noise']}, ventricular pauses {c["pauses"]}, VS {c['vs']}, VP {c['vp']}, VR {c['vr']}")
    for e in eps[-6:]:
        print(f"  #{e['num']:<3} {pp.EPISODE_NAMES[e['kind']]:<22} {pp.fmt_duration(e['dur_ms']):>9}  {pp.episode_detail(e)}"
              + (f"  [EGM {e['strip']['slot']}]" if e["strip"] else ""))
    print(f"  ground truth: {truth2} gaps over 2 s between ventricular depolarizations; the device logged {c['pauses']} pauses.")
    if truth2 > c["pauses"]:
        print("  The device cannot see them: every myopotential it senses is a VS, a ventricular event in its own count, so\n"
              "  its interval since the last event never reaches 2 s. Only the noise-reversion episodes and a VS count far\n"
              "  above the patient's intrinsic rate (none, in a patient with no escape rhythm) betray the interference.")


def interrogate(run, link_kind, clear=False):
    machine = run.m
    link = (pp.CommunicatorLink(machine) if link_kind == "communicator" else pp.WandLink(machine))
    try:
        return pp.interrogate(pp.Session(link, timeout_s=30), clear=clear)
    finally:
        link.close()


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--host", default=os.environ.get("EMERSON_HOST", "http://localhost:10314"),
                   help="franklin server (default: EMERSON_HOST, else %(default)s)")
    p.add_argument("--session", default=None, metavar="TOKEN", help="use this session (it is reset first)")
    p.add_argument("--healthy", action="store_true", help="control: no battery defect; no resets, no latch")
    p.add_argument("--scale", type=int, default=3600, help="implant s per guest s (default %(default)s: 48 h = 48 s)")
    p.add_argument("--depletion", type=float, default=TWO_YEARS_LEFT_PCT, help="%% of the cell used (default %(default)s: ~2 years left)")
    p.add_argument("--escape-bpm", type=int, default=0, help="the patient's escape rhythm, 0 = none (default)")
    p.add_argument("--span-s", type=float, default=60.0, help="--healthy: guest seconds to watch (default %(default)s)")
    p.add_argument("--no-harm", action="store_true", help="skip the harm phase")
    p.add_argument("--control", action="store_true",
                   help="bipolar control: healthy cell, VVI 72 ppm bipolar at its programmed sensitivity, harm table only")
    p.add_argument("--skip-latch", action="store_true",
                   help="development: harm table only, on a device already in Safety Mode (a firmware build that forces it)")
    p.add_argument("--efforts", default=DEFAULT_EFFORTS, help="arm effort levels, 0..1 (default %(default)s)")
    p.add_argument("--trial-s", type=float, default=12.0, help="guest seconds per trial (default %(default)s)")
    p.add_argument("--burst", default="0.3,0.3", metavar="ON,OFF",
                   help="squeeze pattern in guest seconds for the second set of trials; 'none' skips them (default %(default)s)")
    p.add_argument("--max-guest-s", type=float, default=240.0)
    p.add_argument("--wall-timeout", type=float, default=420.0)
    args = p.parse_args(argv)
    if args.control or args.skip_latch:
        args.healthy = True  # no battery defect: the device is not being reset

    from emerson import EmulatorController
    t0 = time.monotonic()
    own = args.session is None
    with EmulatorController(args.host).connect() as conn:
        token = conn.run_project(PROJECT) if own else args.session
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


def scenario(machine, args):
    run = Run(machine, args.scale)
    try:
        setup(run, args)
        efforts = [float(x) for x in args.efforts.split(",")]
        burst = None if args.burst == "none" else tuple(float(x) for x in args.burst.split(","))
        if args.control or args.skip_latch:
            if args.control:
                run.type("mode VVI")
                run.type("rate 72")
                run.pump(1.0)
            rows = harm_phase(run, efforts, args.trial_s,
                              "BIPOLAR CONTROL (device healthy, VVI 72 ppm)" if args.control
                              else "SAFETY MODE (unipolar, AGC 0.25 mV)", burst)
            return 0
        print("\ntimeline" + (" (healthy control)" if args.healthy else ""))
        done = wait_for_outcome(run, args)
        beats = heart_beats(run)
        print(f"\npatient beats so far: {beats}")
        if args.healthy:
            ok = done and run.resets == 0 and not run.latched and run.wakeups >= 1
            print(f"\nHEALTHY CONTROL: {run.wakeups} wakeups, {run.links} radio links, {run.resets} resets, "
                  f"latched={run.latched}  ->  {'PASS' if ok else 'FAIL'}")
            try:
                r = interrogate(run, "communicator")
                print("\ninterrogation over the communicator:\n" + pp.summary_text(r).split("\nEpisodes")[0])
            except (RuntimeError, TimeoutError) as err:
                print(f"communicator interrogation failed: {err}")
                ok = False
            return 0 if ok else 1
        if not (done and run.latched):
            print(f"\nFAIL: no latch (resets seen {run.resets})")
            return 1
        print(f"\nlatched after {run.resets} supervisor resets. Verifying the device's state:")
        checks = verify_latched(run)
        for name, ok, detail in checks:
            print(f"  [{'PASS' if ok else 'FAIL'}] {name}: {detail}")
        harm_rows = None
        if not args.no_harm:
            run.pump(2.0)
            harm_rows = harm_phase(run, efforts, args.trial_s, "SAFETY MODE (unipolar, AGC 0.25 mV)", burst)
            rest = harm_rows[0]
            checks.append(("at rest: no pause, no noise reversion", rest["longest"] < 2000 and not rest["noise_on"],
                           f"longest gap {rest['longest']} ms, {rest['noise_on']} reversions"))
            worst = max(harm_rows, key=lambda r: r["longest"])
            print(f"\nworst trial: effort {worst['effort']:g} {pattern_label(worst)}: a {worst['longest']} ms gap with no "
                  "heartbeat in a patient with no escape rhythm (an 827.6 ms pacing interval; 2 s is a pause)")
            checks.append(("myopotentials lengthen the interval beyond the pacing interval",
                           worst["longest"] > 1200, f"{worst['longest']} ms vs 828 ms"))
        if harm_rows:
            report_device_view(run, harm_rows)
        # The clinic. The radio cannot finish a session on this cell (the defect is still there:
        # the same milliamps reset it again), so the wand reads it.
        try:
            run.pump(3.0)
            interrogate(run, "communicator")
            print("\ncommunicator session completed")
        except (RuntimeError, TimeoutError) as err:
            print(f"\ncommunicator interrogation after the latch: {err}")
            print("  (expected with the defect still present: the session's milliamps reset the device again)")
        r = r2 = None
        for attempt in range(4):  # the failed radio session reset the device; let it boot
            run.pump(4.0)
            try:
                r = interrogate(run, "wand", clear=True)
                r2 = interrogate(run, "wand")
                break
            except (RuntimeError, TimeoutError) as err:
                print(f"  wand attempt {attempt + 1} failed ({str(err)[:60]}), retrying")
        if r2 is None:
            print("FAIL: the wand could not read the device")
            return 1
        print("\ninterrogation through the wand (--clear sent):\n")
        print(pp.summary_text(r2))
        still = r2["safety"] and r2["safety"]["latched"]
        checks.append(("`--clear` does not clear it", bool(still), "latched after clearing diagnostics"))
        sf = r2["safety"]
        window = [x for x in sf["resets"] if sf["latched_s"] - x["implant_s"] <= sf["window_s"]]
        hours = [(x["implant_s"] - sf["resets"][0]["implant_s"]) / 3600 for x in sf["resets"]]
        print("\nimplant-clock timeline of the uncommanded resets (hours since the first): "
              + ", ".join(f"{h:.1f}" for h in hours))
        checks.append(("3 resets within 48 implant hours", len(window) >= 3 and hours[2] < 48.0,
                       f"third reset at +{hours[2]:.1f} h"))
        bad = [n for n, ok, _ in checks if not ok]
        print("\nRESULT:", "PASS: reproduced" if not bad else f"FAIL: {bad}")
        return 0 if not bad else 1
    finally:
        run.close()


if __name__ == "__main__":
    sys.exit(main())
