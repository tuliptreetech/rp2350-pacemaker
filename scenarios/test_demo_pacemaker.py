#!/usr/bin/env python3
"""Checks for the "Demo Pacemaker" conditions in talk-track.md: each one is triggered with the same
device actions and console commands the talk uses, and the numbers it says should appear on screen are
measured from the telemetry markers (tty1), the console (tty0), the patient's own ground truth
(`/patient/heart get_status`) and, where the talk says something is stored, an interrogation through
the wand.

Every test starts its own session of the project (a new device and a new patient, power-on defaults)
and stops it at the end, so the session you demo from is never touched, and none is needed. Run from
the repo root, with emerson-server up on firmware/firmware.bin (`emerson load`):

    scenarios/demo_tests.sh -q                          # every condition
    scenarios/demo_tests.sh -q -m "not slow"            # skip rate response and the finale
    scenarios/demo_tests.sh -q -s -k magnet             # one, printing what it measured
    scenarios/demo_tests.sh -q --jobs 4                 # 4 groups at once, a session each

demo_tests.sh copies the scripts into the server container and runs them there with `emerson exec`.
`emerson run scenarios/test_demo_pacemaker.py ...` works as well, but only while a session of yours exists.

`--jobs N` splits the selected tests into N groups and runs each in its own pytest process; it is this
script's option, not pytest-xdist's `-n`.

Windows are guest (heart) time, not wall time: the emulator runs at ~0.23x wall on Apple Silicon.
"""

import os
import re
import statistics
import sys
import time
from pathlib import Path
from typing import NamedTuple, Optional

import pytest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "programmer"))
sys.path.insert(0, str(HERE))
import pacemaker_programmer as pp  # noqa: E402

emerson = pytest.importorskip("emerson")

HOST = os.environ.get("EMERSON_HOST", "http://localhost:10314")
MARKER = re.compile(r"^(AS|AP|AR|VS|VP|VR) t=([0-9.]+)(?: AV=(\d+) ms)?(?: interval=(\d+) ms)?")
HEART_V = re.compile(r"ventricle lead (\w+) \(\d+ ohm\), threshold ([0-9.]+) V @ [0-9.]+ ms, last pulse [^,]+, "
                     r"paced (\d+) captured / (\d+) not")
HEART_BEATS = re.compile(r"beats: (\d+) sinus, (\d+) conducted, (\d+) blocked, (\d+) escape, (\d+) PVC")


class Marker(NamedTuple):
    code: str
    t: float               # firmware seconds
    av: Optional[int]      # ms, on tracked ventricular events
    interval: Optional[int]  # ms since the previous event in the same chamber


class Device:
    """One session: its two serial ports, read into lines, and the parts the conditions act on."""

    def __init__(self, machine):
        self.m = machine
        self.subs = {"tty0": machine.subscribe_broker("tty0"), "tty1": machine.subscribe_broker("tty1")}
        self.buf = {"tty0": b"", "tty1": b""}
        self.lines = {"tty0": [], "tty1": []}
        self.heart = machine.get_device("/patient/heart")
        self.body = machine.get_device("/patient/body")
        self.magnet = machine.get_device("/patient/magnet")
        self.battery = machine.get_device("/board/battery")
        self.vpace = machine.get_device("/board/vpace")

    def close(self):
        for sub in self.subs.values():
            sub.close()

    # --- time and ports ---

    def guest_s(self):
        m = re.search(r"at guest ([0-9.]+) s", self.battery.invoke_action("get_rail", ""))
        return float(m.group(1))

    def pump(self, wall_s):
        end = time.monotonic() + wall_s
        while True:
            for name, sub in self.subs.items():
                chunk = sub.read(timeout=0.05)
                if chunk:
                    self.buf[name] += chunk
                    *done, self.buf[name] = self.buf[name].split(b"\n")
                    self.lines[name] += [x.decode(errors="replace").strip("\r ") for x in done]
            if time.monotonic() >= end:
                return

    def wait_guest(self, seconds, wall_timeout=600.0):
        start = self.guest_s()
        deadline = time.monotonic() + wall_timeout
        while self.guest_s() - start < seconds:
            assert time.monotonic() < deadline, f"emulator stalled: {seconds} guest s not reached"
            self.pump(0.5)

    def mark(self):
        """A position in the telemetry, to read what came after it."""
        return len(self.lines["tty1"])

    def markers(self, since=0):
        out = []
        for line in self.lines["tty1"][since:]:
            m = MARKER.match(line)
            if m:
                out.append(Marker(m.group(1), float(m.group(2)),
                                  int(m.group(3)) if m.group(3) else None, int(m.group(4)) if m.group(4) else None))
        return out

    def window(self, seconds):
        """The markers printed over the next `seconds` of guest time."""
        at = self.mark()
        self.wait_guest(seconds)
        return self.markers(at)

    def wait_for(self, pattern, since, guest_timeout=10.0, port="tty1"):
        """The first line on `port` after `since` that matches, waiting up to `guest_timeout`."""
        start = self.guest_s()
        while True:
            for line in self.lines[port][since:]:
                if re.search(pattern, line):
                    return line
            if self.guest_s() - start > guest_timeout:
                return None
            self.pump(0.3)

    def type(self, command):
        """A console command on tty0, as the clinician types it."""
        self.m.send_to_broker("tty0", command.encode() + b"\r")
        self.pump(0.5)

    def boot(self):
        """Let the firmware boot and the rhythm settle."""
        deadline = time.monotonic() + 180
        while len(self.markers()) < 4:
            assert time.monotonic() < deadline, "no markers on tty1: the firmware did not boot"
            self.pump(0.5)
        self.wait_guest(2.0)

    # --- the patient ---

    def heart_state(self):
        st = self.heart.invoke_action("get_status", "")
        v = HEART_V.search(st)
        b = HEART_BEATS.search(st)
        return {
            "v_lead": v.group(1), "v_threshold": float(v.group(2)),
            "v_captured": int(v.group(3)), "v_not": int(v.group(4)),
            "sinus": int(b.group(1)), "conducted": int(b.group(2)), "escape": int(b.group(4)), "pvc": int(b.group(5)),
            "v_rate": float(re.search(r"ventricular rate ([0-9.]+) bpm", st).group(1)),
            "raw": st,
        }

    def capacitor_v(self):
        return float(re.search(r"output capacitor ([0-9.]+) V", self.vpace.invoke_action("get_status", "")).group(1))

    def interrogate(self, tries=3):
        """A follow-up through the wand, as the clinic programmer does it."""
        err = None
        for _ in range(tries):
            link = pp.WandLink(self.m)
            try:
                return pp.interrogate(pp.Session(link, timeout_s=30))
            except (RuntimeError, TimeoutError) as e:
                err = e
            finally:
                link.close()
            self.pump(2.0)
        raise AssertionError(f"wand interrogation failed: {err}")


def delta(before, after, key):
    return after[key] - before[key]


def intervals(markers, code):
    return [m.interval for m in markers if m.code == code and m.interval]


def avs(markers, code):
    return [m.av for m in markers if m.code == code and m.av]


def count(markers, code):
    return sum(m.code == code for m in markers)


def median(xs):
    return statistics.median(xs) if xs else float("nan")


def show(name, **values):
    print(f"\n[{name}] " + ", ".join(f"{k}={v}" for k, v in values.items()))


# --- sessions -----------------------------------------------------------------------------------------

@pytest.fixture(scope="module")
def conn():
    with emerson.EmulatorController(HOST).connect() as c:
        yield c


@pytest.fixture
def dev(conn):
    """A fresh device and patient: DDD at power-on defaults, sinus 70 bpm, normal conduction."""
    token = conn.run_project(pp.DEFAULT_PROJECT)
    try:
        with conn.attach(token) as machine:
            d = Device(machine)
            try:
                machine.go()
                d.boot()
                yield d
            finally:
                d.close()
    finally:
        try:
            conn.stop_project(token)
        except RuntimeError:
            pass


# --- 1. Normal sinus rhythm -------------------------------------------------------------------------------

def test_1_normal_sinus_rhythm_fully_inhibited(dev):
    ms = dev.window(8.0)
    as_iv, vs_av = intervals(ms, "AS"), avs(ms, "VS")
    show("sinus", AS=count(ms, "AS"), VS=count(ms, "VS"), AP=count(ms, "AP"), VP=count(ms, "VP"),
         cycle_ms=median(as_iv), av_ms=sorted(set(vs_av)))
    assert count(ms, "AP") == 0 and count(ms, "VP") == 0, "DDD must be fully inhibited in sinus rhythm"
    assert count(ms, "AS") >= 7
    assert abs(count(ms, "VS") - count(ms, "AS")) <= 1, "every AS should be followed by a VS"
    assert 850 <= median(as_iv) <= 865, "cycle ~857 ms (70 bpm)"
    assert vs_av and all(155 <= av <= 166 for av in vs_av), "AV interval ~160 ms"


# --- 2. Complete heart block ----------------------------------------------------------------------------

def test_2_complete_heart_block_tracks_and_paces_ventricle(dev):
    dev.heart.invoke_action("set_av_block", "third")
    dev.wait_guest(3.0)
    h0 = dev.heart_state()
    ms = dev.window(8.0)
    h1 = dev.heart_state()
    vp_av = avs(ms, "VP")
    show("third-degree", AS=count(ms, "AS"), VP=count(ms, "VP"), VS=count(ms, "VS"), AP=count(ms, "AP"),
         av_ms=sorted(set(vp_av)), cycle_ms=median(intervals(ms, "VP")),
         captured=delta(h0, h1, "v_captured"), not_captured=delta(h0, h1, "v_not"), heart_rate=h1["v_rate"])
    assert count(ms, "AP") == 0, "the sinus node is still running: the atrium is sensed, not paced"
    assert count(ms, "VS") == 0, "nothing conducts, and every VP comes before the 35 bpm escape"
    assert count(ms, "VP") >= count(ms, "AS") - 1
    assert vp_av and all(188 <= av <= 192 for av in vp_av), "AS then VP at the 190 ms sensed AV delay"
    assert 850 <= median(intervals(ms, "VP")) <= 865, "the ventricular rate follows the sinus node at ~70 bpm"
    assert delta(h0, h1, "v_not") == 0 and delta(h0, h1, "v_captured") >= 7, "every pace captures (evoked QRS)"
    assert 65 <= h1["v_rate"] <= 75


# --- 3. Sinus arrest ------------------------------------------------------------------------------------

def test_3_sinus_arrest_paces_atrium(dev):
    dev.heart.invoke_action("set_av_block", "none")
    dev.heart.invoke_action("set_sinus_rate", "0")
    dev.wait_guest(3.0)
    ms = dev.window(8.0)
    vs_av, ap_iv = avs(ms, "VS"), intervals(ms, "AP")
    show("sinus arrest", AP=count(ms, "AP"), VS=count(ms, "VS"), AS=count(ms, "AS"), VP=count(ms, "VP"),
         av_ms=sorted(set(vs_av)), cycle_ms=median(ap_iv))
    assert count(ms, "AS") == 0 and count(ms, "VP") == 0
    assert count(ms, "AP") >= 6 and abs(count(ms, "VS") - count(ms, "AP")) <= 1, "AP then a conducted VS"
    assert vs_av and all(165 <= av <= 180 for av in vs_av), "AP then VS at ~173 ms"
    assert 940 <= median(ap_iv) <= 965, "cycle ~953 ms (VA restarts at the conducted VS)"

    # Add the block back: AP then VP at exactly 1000 ms.
    dev.heart.invoke_action("set_av_block", "third")
    dev.wait_guest(3.0)
    ms = dev.window(8.0)
    ap_iv, vp_iv = intervals(ms, "AP"), intervals(ms, "VP")
    show("sinus arrest + block", AP=count(ms, "AP"), VP=count(ms, "VP"), VS=count(ms, "VS"),
         av_ms=sorted(set(avs(ms, "VP"))), ap_cycle=sorted(set(ap_iv)), vp_cycle=sorted(set(vp_iv)))
    assert count(ms, "VS") == 0 and count(ms, "AP") >= 6
    assert all(998 <= iv <= 1002 for iv in ap_iv + vp_iv), "AP-VP every 1000 ms (60 ppm)"


# --- 4. Mobitz II ---------------------------------------------------------------------------------------

def test_4_mobitz_ii_paces_only_dropped_beats(dev):
    dev.heart.invoke_action("set_av_block", "second 3")
    dev.wait_guest(3.0)
    ms = dev.window(12.0)
    vseq = [m.code for m in ms if m.code in ("VS", "VP")]
    vp_share = vseq.count("VP") / len(vseq)
    show("Mobitz II", sequence="".join("P" if c == "VP" else "s" for c in vseq), VP_share=round(vp_share, 2),
         vp_av=sorted(set(avs(ms, "VP"))), vs_av=sorted(set(avs(ms, "VS"))))
    assert count(ms, "AP") == 0
    assert 0.25 <= vp_share <= 0.42, "one beat in three is dropped and paced"
    assert all(not (a == b == "VP") for a, b in zip(vseq, vseq[1:])), "never two paced beats in a row"
    assert all(188 <= av <= 192 for av in avs(ms, "VP")), "AS then VP at 190 ms on the dropped beats"
    assert all(155 <= av <= 166 for av in avs(ms, "VS")), "the conducted beats keep their own ~160 ms AV"


# --- 5. PVCs --------------------------------------------------------------------------------------------

def test_5_pvcs_are_sensed_and_counted(dev):
    n = 4
    h0 = dev.heart_state()
    at = dev.mark()
    for _ in range(n):
        # Right after a sinus beat, so the PVC lands mid-cycle (the ventricle is excitable again ~300 ms
        # later, the next P wave is ~700 ms later). At a random moment it can fall in the AV delay after
        # a P wave, and the device rightly takes it for that P wave's conducted beat.
        assert dev.wait_for(r"^VS t=\S+ AV=", dev.mark(), guest_timeout=3.0), "no sinus beat to time the PVC by"
        dev.heart.invoke_action("inject_pvc", "")
        dev.wait_guest(2.0)
    h1 = dev.heart_state()
    ms = dev.markers(at)
    # A PVC as the device sees it: a VS with no atrial event since the previous ventricular one. The
    # window opens on the first injection, so its first marker is that PVC.
    pvcs = [b for a, b in zip([None] + ms, ms)
            if b.code == "VS" and b.av is None and (a is None or a.code in ("VS", "VP", "VR"))]
    r = dev.interrogate()
    show("PVCs", sequence=" ".join(f"{m.code}{'/' + str(m.av) if m.av else ''}" for m in ms),
         injected=n, heart_pvcs=delta(h0, h1, "pvc"), device_vs_without_p=len(pvcs),
         device_pvc_counter=r["counters"]["pvc"])
    assert delta(h0, h1, "pvc") == n, "the heart fired every PVC"
    assert len(pvcs) >= n - 1, "each PVC appears as a VS with no atrial event before it"
    assert r["counters"]["pvc"] >= n - 1, "the PVC counter goes up"


# --- 6. Upper rate behaviour ----------------------------------------------------------------------------

def test_6_fast_atrium_with_block_gives_pacemaker_wenckebach(dev):
    dev.heart.invoke_action("set_av_block", "third")
    dev.heart.invoke_action("set_sinus_rate", "130")
    dev.wait_guest(3.0)
    ms = dev.window(10.0)
    vp_iv, vp_av = intervals(ms, "VP"), avs(ms, "VP")
    a_events = count(ms, "AS") + count(ms, "AR")
    ratio = count(ms, "VP") / a_events
    show("Wenckebach", AS=count(ms, "AS"), AR=count(ms, "AR"), VP=count(ms, "VP"), V_to_A=round(ratio, 2),
         min_vp_interval=min(vp_iv), vp_av=sorted(set(vp_av)))
    assert count(ms, "VS") == 0
    assert min(vp_iv) >= 498, "VP never faster than the 120 ppm upper rate (500 ms)"
    assert count(ms, "AR") >= 3, "P waves fall in PVARP and are marked AR"
    assert any(188 <= av <= 192 for av in vp_av), "the first beat of a cycle at the 190 ms sensed AV delay"
    assert any(215 <= av <= 240 for av in vp_av), "the next one held to the upper rate (~228 ms)"
    assert 0.6 <= ratio <= 0.75, "a 3:2 pattern"

    # 180 bpm: an atrial high-rate episode, with a stored EGM strip.
    at = dev.mark()
    dev.heart.invoke_action("set_sinus_rate", "180")
    line = dev.wait_for(r"^EPISODE: atrial high rate", at, guest_timeout=10.0)
    assert line, "no atrial high-rate episode at 180 bpm"
    dev.wait_guest(5.0)  # the strip holds 4 s after the trigger
    r = dev.interrogate()
    ahr = [e for e in r["episodes"] if e["kind"] == pp.EP_AHR]
    show("AHR 180", line=line, episodes=len(ahr), strip=bool(ahr and ahr[-1]["strip"]), counter=r["counters"]["ahr"])
    assert ahr and ahr[-1]["strip"], "the high-rate episode has a stored EGM strip"


# --- 7. Loss of capture ---------------------------------------------------------------------------------

def test_7_loss_of_capture_and_recovery(dev):
    dev.heart.invoke_action("set_av_block", "third")
    dev.wait_guest(2.0)
    dev.heart.invoke_action("set_capture_threshold", "ventricle 1.5")
    dev.wait_guest(4.0)  # the last captured beat's rate has to leave the heart's average
    h0 = dev.heart_state()
    ms = dev.window(8.0)
    h1 = dev.heart_state()
    show("loss of capture", sequence=" ".join(f"{m.code}@{m.t:.3f}" for m in ms if m.code[0] == "V"),
         threshold_v=h1["v_threshold"], VP=count(ms, "VP"), VS=count(ms, "VS"),
         not_captured=delta(h0, h1, "v_not"), captured=delta(h0, h1, "v_captured"),
         escapes=delta(h0, h1, "escape"), heart_rate=h1["v_rate"])
    assert h1["v_threshold"] > 2.5, "the threshold is above the 2.5 V output"
    assert delta(h0, h1, "v_captured") == 0 and delta(h0, h1, "v_not") >= 6, "spikes with no complex after them"
    assert delta(h0, h1, "escape") >= 3, "slow escape beats"
    # Not checked: the talk's "escape beat ~700 ms later". The 35 bpm escape (1714 ms) is exactly two
    # 857 ms sinus cycles, so each escape lands on a non-capturing spike (in its blanking, or just
    # before it as a VS that inhibits it), never midway. Measured: no VS at all in the window.
    assert 30 <= h1["v_rate"] <= 40, "the heart falls to its 35 bpm escape"

    # Recovery: `amp 3500` on tty0.
    dev.type("amp 3500")
    dev.wait_guest(1.0)
    assert abs(dev.capacitor_v() - 3.5) < 0.01, "the DAC charged the output capacitor to 3.5 V"
    dev.wait_guest(3.0)
    h2 = dev.heart_state()
    dev.wait_guest(6.0)
    h3 = dev.heart_state()
    show("recovery", capacitor_v=dev.capacitor_v(), captured=delta(h2, h3, "v_captured"),
         not_captured=delta(h2, h3, "v_not"), heart_rate=h3["v_rate"])
    assert delta(h2, h3, "v_not") == 0 and delta(h2, h3, "v_captured") >= 5, "capture returns"
    assert 65 <= h3["v_rate"] <= 75


# --- 8. Magnet test -------------------------------------------------------------------------------------

def magnet_vp_intervals(dev, hold_s):
    at = dev.mark()
    dev.magnet.invoke_action("apply_magnet", "")
    on = dev.wait_for(r"^MAGNET on", at, guest_timeout=2.0)
    assert on, "no MAGNET on line"
    start = dev.lines["tty1"].index(on, at)
    dev.wait_guest(hold_s)
    ms = dev.markers(start)
    at = dev.mark()
    dev.magnet.invoke_action("remove_magnet", "")
    assert dev.wait_for(r"^MAGNET off", at, guest_timeout=2.0), "no MAGNET off line"
    return on, ms


def test_8_magnet_threshold_margin_test(dev):
    on, ms = magnet_vp_intervals(dev, 8.0)
    vp_iv = intervals(ms, "VP")[1:]  # the first runs from the last programmed-mode beat
    fast = [iv for iv in vp_iv if 595 <= iv <= 605]
    slow = [iv for iv in vp_iv if 700 <= iv <= 710]
    show("magnet", line=on, vp_intervals=vp_iv)
    assert "DOO" in on, "asynchronous pacing from DDD is DOO"
    assert 2 <= len(fast) <= 3 and vp_iv[:len(fast)] == fast, "the threshold margin test: 3 beats at 100 ppm"
    assert len(slow) >= 3 and len(fast) + len(slow) == len(vp_iv), "then 85 ppm (706 ms)"
    ms = dev.window(4.0)
    assert count(ms, "AS") >= 3 and count(ms, "AP") == 0, "back to DDD, inhibited by sinus rhythm"


def test_8b_magnet_margin_beat_fails_to_capture(dev):
    dev.heart.invoke_action("set_av_block", "third")
    dev.heart.invoke_action("set_capture_threshold", "ventricle 1.2")
    dev.wait_guest(3.0)
    h0 = dev.heart_state()
    dev.wait_guest(4.0)
    h1 = dev.heart_state()
    assert h1["v_threshold"] < 2.5 and delta(h0, h1, "v_not") == 0, "at 0.4 ms every pace captures"
    _, ms = magnet_vp_intervals(dev, 8.0)
    h2 = dev.heart_state()
    show("magnet, threshold 1.2 V", threshold_v=h1["v_threshold"], VP=count(ms, "VP"),
         not_captured=delta(h1, h2, "v_not"), captured=delta(h1, h2, "v_captured"))
    assert delta(h1, h2, "v_not") == 1, "exactly one pace fails: the 80 % pulse width beat"


# --- 9. Lead fracture -----------------------------------------------------------------------------------

def test_9_lead_fracture_alarm_and_recovery(dev):
    # Complete block, so that what the talk shows holds: nothing captures, only escape beats.
    dev.heart.invoke_action("set_av_block", "third")
    dev.wait_guest(3.0)
    at = dev.mark()
    h0 = dev.heart_state()
    dev.heart.invoke_action("set_lead", "ventricle fractured")
    alarm = dev.wait_for(r"^ALARM: LEAD_OFF \(ventricular lead", at, guest_timeout=3.0)
    assert alarm, "no ALARM: LEAD_OFF on tty1"
    status = dev.wait_for(r"^\[status\].*V_LEAD_OFF", at, guest_timeout=6.0)
    dev.wait_guest(6.0)  # several 1714 ms escape cycles
    ms = dev.markers(at)
    h1 = dev.heart_state()
    show("fracture", alarm=alarm, status=bool(status), VS=count(ms, "VS"), VP=count(ms, "VP"),
         captured=delta(h0, h1, "v_captured"), escapes=delta(h0, h1, "escape"))
    assert status, "the status line reports V_LEAD_OFF"
    assert h1["v_lead"].lower() == "fractured"
    assert count(ms, "VS") == 0, "the ventricular channel no longer senses"
    assert delta(h0, h1, "v_captured") <= 1 and delta(h0, h1, "escape") >= 2, "nothing captures, only escape beats"

    at = dev.mark()
    dev.heart.invoke_action("set_lead", "ventricle intact")
    cleared = dev.wait_for(r"^lead-off cleared \(ventricular lead", at, guest_timeout=3.0)
    assert cleared, "the alarm does not clear"
    h2 = dev.heart_state()
    dev.wait_guest(4.0)
    h3 = dev.heart_state()
    r = dev.interrogate()
    loff = [e for e in r["episodes"] if e["kind"] == pp.EP_LEAD_OFF]
    show("repaired", cleared=cleared, captured=delta(h2, h3, "v_captured"),
         episodes=len(loff), strip=bool(loff and loff[-1]["strip"]))
    assert delta(h2, h3, "v_captured") >= 3, "pacing captures again"
    assert loff and loff[-1]["strip"], "the lead-off episode has a stored EGM strip"


# --- Optional: rate response ----------------------------------------------------------------------------

def status_sir(line):
    return int(re.search(r"sir=(\d+) ppm", line).group(1))


@pytest.mark.slow
def test_rate_response_dddr_walk(dev):
    at = dev.mark()
    dev.type("mode DDDR")
    assert dev.wait_for(r"^\[status\] mode=DDDR", at, guest_timeout=6.0), "mode DDDR not in effect"
    dev.body.invoke_action("set_activity", "walk")
    dev.heart.invoke_action("set_competence", "0.3")
    at = dev.mark()
    dev.wait_guest(60.0)  # ~30 s of ramp, then steady
    sirs = [status_sir(x) for x in dev.lines["tty1"][at:] if x.startswith("[status]")]
    ms = dev.markers(dev.mark() - 40)
    a_iv = intervals(ms, "AP") + intervals(ms, "AS")
    show("DDDR walk", sir_ppm=sirs, final_a_cycle_ms=median(a_iv), AP=count(ms, "AP"), AS=count(ms, "AS"))
    assert sirs and sirs[0] < sirs[-1], "the sensor-indicated rate rises"
    assert 78 <= sirs[-1] <= 95, "SIR settles in the 80s"
    assert count(ms, "AP") > count(ms, "AS"), "the incompetent sinus node is overtaken: the atrium is paced"
    assert 60000 / median(a_iv) >= 78, "the paced rate climbs to the 80s"


# --- Finale ---------------------------------------------------------------------------------------------

@pytest.mark.slow
def test_finale_accolade_safety_mode_latches():
    """Steps 1-4 of the finale: defect, resets, the latch, the Safety Mode settings, read back through the wand.
    The myopotential pauses (step 5) are the script's harm phase: `accolade_safety_mode.py` without --no-harm."""
    import accolade_safety_mode
    assert accolade_safety_mode.main(["--no-harm", "--host", HOST]) == 0


@pytest.mark.slow
def test_finale_short_one_communicator_connect_latches():
    """The finale's short form: one communicator connect on the defective cell resets it, the firmware's
    retries reset it twice more, and it latches; the same checks, plus the console and the wand."""
    import accolade_communicator
    assert accolade_communicator.main(["--host", HOST]) == 0


@pytest.mark.slow
def test_finale_short_healthy_connect_completes():
    """The short form's control: the same connect on a healthy cell links, and nothing resets."""
    import accolade_communicator
    assert accolade_communicator.main(["--healthy", "--host", HOST]) == 0


# --- Entry point ----------------------------------------------------------------------------------------

# No cache: `emerson run` mounts the repo, and .pytest_cache would land in it.
PYTEST_ARGS = ["-p", "no:cacheprovider", "-o", "markers=slow: minutes of wall time"]
SLOW_WEIGHT = 4  # a slow test takes about as long as four of the others


def collect(args):
    """The node ids pytest would run with `args` (after -k and -m), and whether each is slow."""
    import contextlib
    import io

    class Collector:
        items = []

        def pytest_collection_finish(self, session):
            self.items = [(item.nodeid, item.get_closest_marker("slow") is not None) for item in session.items]

    c = Collector()
    with contextlib.redirect_stdout(io.StringIO()):
        code = pytest.main([__file__, "--collect-only", "-q", *PYTEST_ARGS, *args], plugins=[c])
    if code not in (0, 5):  # 5: nothing collected
        sys.exit(f"collection failed (pytest exit {code})")
    return c.items


def split(items, jobs):
    """Groups of node ids with about equal weight: the slow tests are spread out first."""
    groups = [[0, []] for _ in range(jobs)]
    for nodeid, slow in sorted(items, key=lambda x: not x[1]):
        g = min(groups, key=lambda g: g[0])
        g[0] += SLOW_WEIGHT if slow else 1
        g[1].append(nodeid)
    return [ids for _, ids in groups if ids]


def run_parallel(jobs, args):
    """Each group in its own pytest process, so in its own sessions at the same time. The emulator slows
    under the load, but every window is guest time, so only the wall time changes."""
    import subprocess
    from concurrent.futures import ThreadPoolExecutor, as_completed

    groups = split(collect(args), jobs)
    if not groups:
        print("no tests selected")
        return 5
    t0 = time.monotonic()
    print(f"running {sum(map(len, groups))} tests in {len(groups)} parallel groups", flush=True)

    def run(ids):
        # Only the node ids: with the file as well, pytest would run every test in each group.
        p = subprocess.run([sys.executable, "-m", "pytest", *PYTEST_ARGS, *args, *ids],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        return ids, p.returncode, p.stdout, time.monotonic() - t0

    results = []
    with ThreadPoolExecutor(len(groups)) as pool:
        for fut in as_completed([pool.submit(run, ids) for ids in groups]):
            ids, code, out, wall = fut.result()
            results.append((ids, code))
            names = ", ".join(i.split("::")[-1] for i in ids)
            print(f"\n===== group done after {wall:.0f} s (exit {code}): {names}\n{out.rstrip()}", flush=True)
    failed = [ids for ids, code in results if code != 0]
    print(f"\n{len(groups) - len(failed)}/{len(groups)} groups passed in {time.monotonic() - t0:.0f} s wall")
    return max(code for _, code in results)


def main(argv):
    """`--jobs N` (or `-j N`) runs the selected tests in N parallel groups; every other argument goes to
    pytest as it is."""
    jobs, args = 1, []
    it = iter(argv)
    for a in it:
        if a in ("-j", "--jobs"):
            jobs = int(next(it))
        elif a.startswith("--jobs="):
            jobs = int(a.split("=", 1)[1])
        else:
            args.append(a)
    if jobs > 1:
        return run_parallel(jobs, args)
    return pytest.main([__file__, *PYTEST_ARGS, *args])


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
