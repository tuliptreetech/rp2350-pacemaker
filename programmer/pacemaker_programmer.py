#!/usr/bin/env python3
"""Interrogate the rp2350 pacemaker demo the way a clinic programmer does.

Holds the programmer's wand over the implant (`/patient/wand place_wand`),
opens a telemetry session through it (the `wand` broker), reads back
everything the diagnostic store holds -- counters, rate histograms, the
episode log, long-term trends and stored electrograms -- and prints a
follow-up summary plus a self-contained HTML report. Lifting the wand
(`remove_wand`) at the end is what closes the session on the device's side
too.
With `--link communicator` there is no wand: the bedside communicator wakes the
implant over its 2.45 GHz wake-up channel (`/patient/communicator start_session`)
and the same session runs over the radio (the `communicator` broker); the device
ends it on END or when the link is lost.

    python3 pacemaker_programmer.py                    # interrogate
    python3 pacemaker_programmer.py --clear            # ...then clear diagnostics
    python3 pacemaker_programmer.py --html report.html
    python3 pacemaker_programmer.py --live             # live EGM + markers in a browser
    python3 pacemaker_programmer.py --live --battery-probe  # ...and the cell's rail and current at /battery
    python3 pacemaker_programmer.py --battery-probe    # the cell alone: no wand, no session
    python3 pacemaker_programmer.py --link communicator  # wandless, over the radio
    python3 pacemaker_programmer.py --upgrade v2.04    # install the battery-test firmware level, then interrogate

It talks to the running session through the emerson library: `--session`
(or EMERSON_SESSION) names one by token, otherwise it uses the first session
of `--project` (EMERSON_PROJECT, by default this project). `--host` (or
EMERSON_HOST) is the franklin server. All three are what `emerson run` sets, so
from an installed Emerson it runs as

    emerson run programmer/pacemaker_programmer.py
    emerson run -p 8765 programmer/pacemaker_programmer.py --live

The wire protocol and the store's layout are the firmware's (firmware/main.c,
"Diagnostic store" and "Telemetry session"); this file mirrors both.
"""

import argparse
import collections
import datetime as dt
import html
import json
import os
import queue
import threading
import random
import re
import struct
import sys
import time
from pathlib import Path

# --- Protocol --------------------------------------------------------------

REQ_OPEN = 0x01
REQ_COUNTERS = 0x02
REQ_HISTOGRAMS = 0x03
REQ_EPISODES = 0x04
REQ_TRENDS = 0x05
REQ_EGM = 0x06
REQ_CLEAR = 0x07
REQ_STREAM = 0x08  # protocol 2
REQ_SAFETY = 0x09  # protocol 4: the Safety Mode latch, the time scale, the uncommanded-reset ring
REQ_FWINFO = 0x0A  # protocol 5: the firmware level, the alerts, the daily battery test history
REQ_UPGRADE = 0x0B  # protocol 5: move the firmware level forward (a modelling simplification: no download)
REQ_END = 0x7F
MSG_STREAM = 0x48  # unsolicited live EGM frames while STREAM is on
RSP_ERROR = 0xEE
ERRORS = {1: "bad CRC", 2: "unknown request", 3: "bad arguments", 4: "no session (OPEN first)"}

# The wand writes requests into the implant's request file, which holds 254
# bytes (firmware/main.c, DOWN_DATA_MAX); the host never has more than that in
# flight (see Session).
MAX_WRITE = 254


def crc16(data: bytes) -> int:
    """CRC-16/CCITT-FALSE: poly 0x1021, init 0xffff, no reflection."""
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else crc << 1
            crc &= 0xFFFF
    return crc


def cobs_encode(data: bytes) -> bytes:
    out = bytearray([0])
    code_at, code = 0, 1
    for b in data:
        if b:
            out.append(b)
            code += 1
        if not b or code == 0xFF:
            out[code_at] = code
            code_at, code = len(out), 1
            out.append(0)
    out[code_at] = code
    return bytes(out)


def cobs_decode(data: bytes) -> bytes:
    out = bytearray()
    i = 0
    while i < len(data):
        code = data[i]
        i += 1
        if code == 0 or i + code - 1 > len(data):
            raise ValueError("malformed COBS")
        out += data[i:i + code - 1]
        i += code - 1
        if code < 0xFF and i < len(data):
            out.append(0)
    return bytes(out)


def encode_frame(kind: int, seq: int, payload: bytes = b"") -> bytes:
    body = bytes([kind, seq]) + payload
    return cobs_encode(body + struct.pack("<H", crc16(body))) + b"\x00"


def parse_frames(stream: bytes):
    """Every well-formed frame in a byte stream, as (type, seq, payload).

    Anything between delimiters that is not a frame -- console text, a frame
    cut short at the start of the history window -- is skipped."""
    frames = []
    for chunk in stream.split(b"\x00"):
        if not chunk:
            continue
        try:
            raw = cobs_decode(chunk)
        except ValueError:
            continue
        if len(raw) < 4 or struct.unpack("<H", raw[-2:])[0] != crc16(raw[:-2]):
            continue
        frames.append((raw[0], raw[1], raw[2:-2]))
    return frames


# --- The store's layout (firmware/main.c, "Diagnostic store") ---------------

INFO_FIELDS = [
    ("proto", "B"), ("fw_major", "B"), ("fw_minor", "B"), ("eeprom_ok", "B"),
    ("implant_s", "I"), ("epoch_at_implant", "I"), ("last_clear_s", "I"), ("last_session_s", "I"),
    ("power_ons", "I"), ("lifetime_ap", "I"), ("lifetime_vp", "I"),
    ("episodes", "I"), ("trends", "I"), ("egms", "I"),
    ("batt_mv", "H"), ("batt_state", "B"), ("mode", "B"), ("magnet", "B"), ("magnet_response", "B"),
    ("rate", "B"), ("utr", "B"), ("msr", "B"),
    ("pav", "H"), ("sav", "H"), ("pvarp", "H"), ("pw", "H"),
    ("amp", "H"), ("aamp", "H"), ("sens", "H"), ("asens", "H"),
    ("ep_slots", "B"), ("egm_slots", "B"), ("trend_slots", "H"),
    ("egm_samples", "H"), ("egm_pre", "H"), ("egm_sps", "H"), ("egm_uv_per_lsb", "H"),
    ("trend_period_s", "I"), ("act_mg", "H"), ("sir", "H"), ("implant_s_per_guest_s", "I"),
]
INFO_FORMAT = "<" + "".join(f for _, f in INFO_FIELDS)

COUNTER_NAMES = ["as", "ap", "ar", "vs", "vp", "vr", "pvc", "as_vs", "as_vp", "ap_vs", "ap_vp",
                 "magnet", "mode_changes", "ahr", "vhr", "lead_off", "noise", "pauses"]
COUNTERS_FORMAT = "<32I"

HIST_NAMES = ["Atrial sensed", "Atrial paced", "Ventricular sensed", "Ventricular paced", "Sensor rate"]
HIST_BINS = ["<40"] + [f"{lo}-{lo + 9}" for lo in range(40, 180, 10)] + [">=180"]
HIST_FORMAT = "<80I"

# firmware/main.c `struct safety_block`: latched, latched_s, total, scale, window_s, resets_needed, 2 reserved,
# then RESET_SLOTS x reset_rec (implant_s, cause, 3 reserved).
SAFETY_HEADER_FORMAT = "<8I"
RESET_REC_FORMAT = "<IB3x"
RESET_SLOTS = 16
SAFETY_FORMAT_SIZE = 32 + 8 * RESET_SLOTS
SAFETY_LATCHED = 0x45464153  # "SAFE"
RESET_SUPERVISOR = 2
RESET_CAUSES = {0: "power-on", 1: "commanded reboot", 2: "supervisor reset (RUN low)"}
MIN_SAFETY_PROTOCOL = 4

# firmware/main.c `struct fw_block`: 16 words (level, upgraded_s, flags, alert_s, disabled_s, explant_cal, tests,
# next_test_s, run_1003, run_dis, disable_why, 5 reserved), then BT_RECS x bt_rec.
MIN_FW_PROTOCOL = 5
FW_HEADER_FORMAT = "<16I"
BT_REC_FORMAT = "<IHHHHBBH"  # implant_s, v_pre, v_burst, v_exp, l_x100, flags, verdict, reserved
BT_RECS = 16
FW_SIZE = 64 + 16 * BT_RECS
FW_BASE, FW_V204, FW_FIXED = 0, 1, 2
FW_LEVELS = {FW_BASE: "base", FW_V204: "v2.04", FW_FIXED: "fixed"}
FW_LEVEL_NAMES = {FW_BASE: "base (as shipped)", FW_V204: "v2.04 (battery test; with the two shipped bugs)",
                  FW_FIXED: "fixed (the forthcoming software)"}
FW_UPGRADE_ARGS = {"v2.04": FW_V204, "fixed": FW_FIXED}
FW_F_CODE1003, FW_F_TELE_OFF, FW_F_EXPLANT, FW_F_CLEARED = 1, 2, 4, 8
DIS_IMPEDANCE, DIS_BAD_SAMPLE = 1, 2
DISABLE_REASONS = {0: "", DIS_IMPEDANCE: "high battery impedance",
                   DIS_BAD_SAMPLE: "the battery test evaluated a burst reading that was never taken"}
BT_REC_VALID, BT_REC_MAGNET = 1, 2
BT_VERDICTS = {0: "OK", 1: "ELEVATED", 2: "HIGH", 3: "not sent (rail too low)", 4: "NO SAMPLE",
               5: "discarded (invalid sample)"}
CAL_EPOCH = dt.datetime(2000, 1, 1, tzinfo=dt.timezone.utc)  # the device calendar's zero
CODE_1003 = "Code-1003: Voltage too low for projected remaining longevity"

EPISODE_FORMAT = "<IIBBHHH"   # start_s, dur_ms, kind, egm, a, b, c
TREND_FORMAT = "<HHHBBBBBB"   # day, batt_mv, act_mg, pct_ap, pct_vp, a_rate, v_rate, mode, flags
EGM_HEADER_FORMAT = "<IHBBHHHH"  # start_s, episode, kind, n_markers, n_samples, pre, sps, uv_per_lsb
EGM_MARKERS = 32
EGM_MARKER_FORMAT = "<HBB"
EGM_SIZE = 0x1800

MODES = ["DDD", "DOO", "VVI", "VOO", "OOO", "DDDR", "VVIR"]
BATT_STATES = ["OK", "ERI", "EOL"]
MARKERS = {1: "AS", 2: "AP", 3: "AR", 4: "VS", 5: "VP", 6: "VR"}
EP_ONGOING = 0xFFFFFFFF
EGM_NONE = 0xFF

EP_POWER_ON, EP_LEAD_OFF, EP_ERI, EP_EOL, EP_MAGNET, EP_AHR, EP_VHR, EP_CI, EP_MODE, EP_RADIO_OFF, EP_SAFETY, \
    EP_NOISE, EP_PAUSE, EP_CODE1003, EP_TELE_OFF, EP_FW_LEVEL = range(1, 17)
EPISODE_NAMES = {
    EP_POWER_ON: "Power-on reset", EP_LEAD_OFF: "Lead off", EP_ERI: "ERI", EP_EOL: "EOL",
    EP_MAGNET: "Magnet", EP_AHR: "Atrial high rate", EP_VHR: "Ventricular high rate",
    EP_CI: "Chronotropic diagnostic", EP_MODE: "Mode change", EP_RADIO_OFF: "Radio telemetry disabled",
    EP_SAFETY: "SAFETY MODE latched", EP_NOISE: "Noise reversion (VOO)", EP_PAUSE: "Ventricular pause",
    EP_CODE1003: "Code-1003 alert", EP_TELE_OFF: "Wandless telemetry disabled", EP_FW_LEVEL: "Firmware level changed",
}


def mode_name(m):
    return MODES[m] if m < len(MODES) else f"mode {m}"


def episode_detail(e):
    k, a, b, c = e["kind"], e["a"], e["b"], e["c"]
    if k == EP_POWER_ON:
        what = "new device: diagnostic store formatted" if a else "diagnostics restored from EEPROM"
        return f"{what}; cause: {RESET_CAUSES.get(b, b)}" + ("; SAFETY MODE" if c else "")
    if k == EP_SAFETY:
        return f"{a} uncommanded system resets within the 48 h window: permanent VVI 72.5 ppm"
    if k == EP_LEAD_OFF:
        leads = [n for bit, n in ((0x03, "ventricular"), (0x0C, "atrial")) if a & bit]
        return f"{' and '.join(leads) or 'unknown'} lead (LOFF_STAT 0x{a:02x})"
    if k in (EP_ERI, EP_EOL):
        return f"battery {a / 1000:.3f} V loaded"
    if k == EP_MAGNET:
        return f"asynchronous {mode_name(a)}"
    if k in (EP_AHR, EP_VHR):
        return f"max {a} bpm" + (f", {b} beats" if b else "")
    if k == EP_CI:
        verdict = "incompetence suspected" if a < 80 else "normal"
        return f"index {a}% ({verdict}): exertion {b / 10:.1f} bpm vs rest {c / 10:.1f}"
    if k == EP_MODE:
        return f"{mode_name(a)} -> {mode_name(b)}" + (" (forced by ERI)" if c else " (programmed)")
    if k == EP_RADIO_OFF:
        return f"wandless telemetry off at ERI, battery {a / 1000:.3f} V loaded"
    if k == EP_NOISE:
        return f"{a} noise senses, {b} asynchronous ventricular paces"
    if k == EP_PAUSE:
        return "no ventricular pace or sense past the pause-detection time (the duration is the whole gap)"
    if k == EP_CODE1003:
        return f"voltage too low for projected remaining longevity: burst {a / 1000:.3f} V vs {b / 1000:.3f} V expected, " \
               f"projected {c / 100:.2f} years"
    if k == EP_TELE_OFF:
        burst = f"burst {a / 1000:.3f} V" if a else "no burst reading"
        return f"{DISABLE_REASONS.get(c, c)} ({burst}, expected {b / 1000:.3f} V)"
    if k == EP_FW_LEVEL:
        return f"{FW_LEVELS.get(a, a)} -> {FW_LEVELS.get(b, b)}" + ("; wandless telemetry re-enabled" if c else "")
    return f"a={a} b={b} c={c}"


# --- The link: the wand, through the emerson library -------------------------

DEFAULT_PROJECT = "rp2350"


class WandLink:
    """The programmer's wand, through the emerson library: placing it holds its
    magnet and its reader over the implant, writes go out through the reader,
    and reads return what the implant has sent since. Closing lifts the wand,
    which ends the device's session if END has not already."""

    def __init__(self, machine, wand="/patient/wand", broker="wand"):
        self.machine = machine
        self.broker = broker
        self.wand = machine.get_device(wand)
        # Subscribe before placing the wand, so nothing it brings is missed.
        self.subscription = machine.subscribe_broker(broker)
        self.wand.invoke_action("place_wand", "")

    def send(self, data):
        self.machine.send_to_broker(self.broker, bytes(data))

    def read(self, timeout):
        return self.subscription.read(timeout=timeout)

    def placed(self):
        """Whether the wand is still over the implant: someone may have lifted
        it (`emctl action /patient/wand remove_wand`), which ends the session."""
        return self.wand.invoke_action("get_status", "").startswith("wand placed")

    def close(self):
        try:
            self.wand.invoke_action("remove_wand", "")
        finally:
            self.subscription.close()


class WandLifted(RuntimeError):
    """The wand left the implant mid-session."""


class LinkLost(WandLifted):
    """The radio link closed mid-session: the implant, or the communicator, ended it. A kind of
    WandLifted so that everything that stops cleanly for a lifted wand stops for this too."""


class CommunicatorLink:
    """Wandless telemetry: the bedside communicator (`/patient/communicator`) wakes the implant
    over the 2.45 GHz wake-up channel and carries bytes over the `communicator` broker, through
    the emerson library. Opening it starts a base-initiated session and waits for the implant to
    answer (an implant-initiated session already open -- its automatic remote-monitoring wakeup
    -- is taken as it is). Closing stops the communicator's transmissions; the device ends its
    own side on END or when the link is lost.

    The same interface as WandLink: send, read, placed, close."""

    OPEN_TIMEOUT_S = 15.0
    POLL_S = 0.1

    def __init__(self, machine, communicator="/patient/communicator", broker="communicator",
                 wakeup_s=None, open_timeout_s=None, await_implant_s=None):
        self.machine = machine
        self.broker = broker
        self.device = machine.get_device(communicator)
        # Subscribe before waking the implant, so nothing it sends is missed.
        self.subscription = machine.subscribe_broker(broker)
        try:
            if await_implant_s is not None:
                # Do not wake it: wait for its own remote-monitoring wakeup (the communicator is
                # listening), the implant-initiated session.
                self._wait_linked(await_implant_s, wake_failed_is_fatal=False)
            else:
                if self.phase() != "linked":
                    self.device.invoke_action("start_session", "" if wakeup_s is None else str(wakeup_s))
                self._wait_linked(self.OPEN_TIMEOUT_S if open_timeout_s is None else open_timeout_s)
        except BaseException:
            self.close()
            raise

    def status(self):
        return self.device.invoke_action("get_status", "")

    def phase(self):
        """`idle`, `waking` or `linked`: the first word after `communicator ` in its status."""
        status = self.status()
        words = status.split(";", 1)[0].split()
        return words[1] if len(words) > 1 and words[0] == "communicator" else "unknown"

    def _wait_linked(self, timeout_s, wake_failed_is_fatal=True):
        deadline = time.monotonic() + timeout_s
        while True:
            status = self.status()
            phase = status.split(";", 1)[0].split()[1:2]
            if phase == ["linked"]:
                return
            if phase == ["idle"] and wake_failed_is_fatal:
                raise RuntimeError(f"the implant did not answer the wake-up: {status}")
            if time.monotonic() >= deadline:
                raise RuntimeError(f"no radio link within {timeout_s:g} s: {status}")
            time.sleep(self.POLL_S)

    def send(self, data):
        self.machine.send_to_broker(self.broker, bytes(data))

    def read(self, timeout):
        return self.subscription.read(timeout=timeout)

    def placed(self):
        """Whether the radio link is still up: the implant ends it on END, on losing the
        link, or after a short silence."""
        return self.phase() == "linked"

    def lost_error(self):
        return LinkLost("the radio link closed: the device has ended the session")

    def close(self):
        try:
            self.device.invoke_action("end_session", "")
        except Exception:  # an already-ended session is not an error when leaving
            pass
        finally:
            self.subscription.close()


class Session:
    """Request/response over the link.

    The wand writes the host's bytes into the implant's request file, which
    holds MAX_WRITE of them and which firmware empties before the wand may
    write it again. Requests therefore go out in batches of whole frames no
    bigger than that, and each batch waits for its responses before the next
    is sent."""

    def __init__(self, link, timeout_s=60.0):
        self.link = link
        self.timeout_s = timeout_s
        self.seq = random.randrange(256)
        self.buf = bytearray()   # received bytes not yet split into frames
        self.on_stream = None    # called with each live-stream frame's (seq, payload)

    def check_link(self):
        """Raise if the link can say the wand was lifted."""
        placed = getattr(self.link, "placed", None)
        if placed and not placed():
            lost = getattr(self.link, "lost_error", None)
            raise lost() if lost else WandLifted("the wand was lifted: the device has ended the session")

    def _receive(self, timeout):
        """The response frames received within `timeout`; live-stream frames
        go to `on_stream` instead."""
        data = self.link.read(timeout)
        self.buf += data
        end = self.buf.rfind(b"\x00") + 1
        frames = parse_frames(bytes(self.buf[:end]))
        del self.buf[:end]
        responses = []
        for kind, seq, payload in frames:
            if kind == MSG_STREAM:
                if self.on_stream:
                    self.on_stream(seq, payload)
            else:
                responses.append((kind, seq, payload))
        return responses

    def pump(self, timeout):
        """Take in whatever arrives within `timeout`, between requests."""
        self._receive(timeout)

    def open(self, unix_now):
        # OPEN is what starts the session. The leading delimiter ends any
        # half-frame the device was still collecting.
        return self.transact([(REQ_OPEN, struct.pack("<I", unix_now))], lead=b"\x00")[0]

    def transact(self, requests, lead=b"", timeout_s=None):
        """Send every request, a batch at a time; returns their response
        payloads in order. Each batch waits up to `timeout_s` (default: the
        session's) for its responses."""
        timeout_s = self.timeout_s if timeout_s is None else timeout_s
        frames = []
        for kind, payload in requests:
            self.seq = (self.seq + 1) % 256
            frames.append((kind, self.seq, encode_frame(kind, self.seq, payload)))
        results = []
        batch, size = [], len(lead)
        for frame in frames:
            if batch and size + len(frame[2]) > MAX_WRITE:
                results += self._exchange(batch, lead, timeout_s)
                batch, size, lead = [], 0, b""
            batch.append(frame)
            size += len(frame[2])
        results += self._exchange(batch, lead, timeout_s)
        return results

    def _collect(self, batch, answers, timeout):
        """Take in what arrives within `timeout` and file the responses to `batch` by seq."""
        for rkind, rseq, payload in self._receive(timeout):
            for kind, seq, _ in batch:
                if rseq == seq and rkind in (kind | 0x80, RSP_ERROR):
                    if rkind == RSP_ERROR:
                        raise RuntimeError(f"request 0x{payload[0]:02x}: {ERRORS.get(payload[1], payload[1])}")
                    answers[seq] = payload

    def _exchange(self, batch, lead, timeout_s):
        self.link.send(lead + b"".join(f for _, _, f in batch))
        answers = {}
        deadline = time.monotonic() + timeout_s
        while len(answers) < len(batch):
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                missing = [f"0x{k:02x}" for k, s, _ in batch if s not in answers]
                raise TimeoutError(f"no response to request(s) {', '.join(missing)} -- is the session running, "
                                   "and the wand still over the implant?")
            self._collect(batch, answers, min(remaining, 0.5))
            if len(answers) == len(batch):
                break
            try:
                self.check_link()
            except WandLifted:
                # A radio session ends once its END has gone out, and the last of what it sent
                # can still be on its way: take it in before calling the link lost.
                self._collect(batch, answers, 0.5)
                if len(answers) < len(batch):
                    raise
        return [answers[seq] for _, seq, _ in batch]


# --- Interrogation ----------------------------------------------------------

def unpack_list(fmt, data):
    size = struct.calcsize(fmt)
    return [struct.unpack_from(fmt, data, i * size) for i in range(len(data) // size)]


def interrogate(session, clear=False, now=None, upgrade=None):
    """Read everything. `upgrade` ("v2.04" or "fixed", or a level number) first asks the device to move its
    firmware level forward -- the model's stand-in for the clinic's firmware download -- so the report is of
    the upgraded device."""
    now = int(time.time()) if now is None else now
    raw_info = session.open(now)
    info = dict(zip([n for n, _ in INFO_FIELDS], struct.unpack_from(INFO_FORMAT, raw_info)))
    # The device stamps everything with its implant clock; this session set
    # that clock's epoch to now.
    info["epoch"] = now - info["implant_s"]
    info["session_unix"] = now

    n_ep = min(info["episodes"], info["ep_slots"])
    n_tr = min(info["trends"], info["trend_slots"])
    n_egm = min(info["egms"], info["egm_slots"])
    # Protocol 5 added the firmware block and the upgrade; an older device is read without them.
    with_fw = info["proto"] >= MIN_FW_PROTOCOL
    requests = []
    if upgrade is not None:
        if not with_fw:
            raise RuntimeError(f"this firmware (protocol {info['proto']}) has no battery test to upgrade to")
        level = FW_UPGRADE_ARGS[upgrade] if isinstance(upgrade, str) else upgrade
        requests.append((REQ_UPGRADE, bytes([level])))
    first = len(requests)
    requests += [(REQ_COUNTERS, b""), (REQ_HISTOGRAMS, b""),
                 (REQ_EPISODES, struct.pack("<BB", 0, n_ep)),
                 (REQ_TRENDS, struct.pack("<HH", 0, n_tr))]
    requests += [(REQ_EGM, bytes([slot])) for slot in range(n_egm)]
    # Protocol 4 added the Safety Mode block; an older device is read without it.
    with_safety = info["proto"] >= MIN_SAFETY_PROTOCOL
    if with_safety:
        requests.append((REQ_SAFETY, b""))
    if with_fw:
        requests.append((REQ_FWINFO, b""))
    if clear:
        requests.append((REQ_CLEAR, b""))
    requests.append((REQ_END, b""))
    try:
        replies = session.transact(requests)
    except RuntimeError as err:
        if upgrade is not None and "0x0b" in str(err):
            raise RuntimeError("the device refused the upgrade (a downgrade, the same level, or Safety Mode)") from err
        raise
    replies = replies[first:]

    counters = dict(zip(COUNTER_NAMES, struct.unpack(COUNTERS_FORMAT, replies[0])))
    hist_flat = struct.unpack(HIST_FORMAT, replies[1])
    hists = [list(hist_flat[i * 16:(i + 1) * 16]) for i in range(5)]

    # Episodes, oldest first: the ring's newest is at (episodes - 1) % slots.
    ep_records = unpack_list(EPISODE_FORMAT, replies[2][2:])
    episodes = []
    for num in range(info["episodes"] - n_ep, info["episodes"]):
        start_s, dur_ms, kind, egm, a, b, c = ep_records[num % info["ep_slots"]]
        episodes.append(dict(num=num, start_s=start_s, dur_ms=dur_ms, kind=kind, egm=egm, a=a, b=b, c=c))

    tr_records = unpack_list(TREND_FORMAT, replies[3][4:])
    trends = []
    for num in range(info["trends"] - n_tr, info["trends"]):
        day, batt, act, pap, pvp, ar, vr, mode, flags = tr_records[num % info["trend_slots"]]
        trends.append(dict(day=day, batt_mv=batt, act_mg=act, pct_ap=pap, pct_vp=pvp,
                           a_rate=ar, v_rate=vr, mode=mode, flags=flags))

    egms = []
    for slot in range(n_egm):
        egm = parse_egm(replies[4 + slot][1:])
        if egm:
            egm["slot"] = slot
            egms.append(egm)
    # A strip belongs to an episode only if that episode still points at its
    # slot and the slot still holds that episode's strip.
    by_episode = {e["episode"]: e for e in egms}
    for ep in episodes:
        strip = by_episode.get(ep["num"] & 0xFFFF)
        ep["strip"] = strip if ep["egm"] != EGM_NONE and strip and strip["slot"] == ep["egm"] else None
    safety = parse_safety(replies[4 + n_egm]) if with_safety else None
    fw = parse_fw(replies[4 + n_egm + (1 if with_safety else 0)]) if with_fw else None
    return dict(info=info, counters=counters, hists=hists, episodes=episodes, trends=trends, egms=egms,
                safety=safety, fw=fw, cleared=clear)


def parse_fw(raw):
    """The firmware block: the level, Code-1003 and telemetry-disabled flags, the explant indicator and the
    daily battery test history, oldest first."""
    (level, upgraded_s, flags, alert_s, disabled_s, explant_cal, tests, next_test_s, run_1003, run_dis,
     why) = struct.unpack_from(FW_HEADER_FORMAT, raw)[:11]
    recs = [struct.unpack_from(BT_REC_FORMAT, raw, 64 + 16 * i) for i in range(BT_RECS)]
    history = []
    for n in range(max(0, tests - BT_RECS), tests):
        implant_s, v_pre, v_burst, v_exp, l_x100, rflags, verdict, _ = recs[n % BT_RECS]
        history.append(dict(num=n + 1, implant_s=implant_s, v_pre_mv=v_pre, v_burst_mv=v_burst, v_exp_mv=v_exp,
                            longevity_y=l_x100 / 100, valid=bool(rflags & BT_REC_VALID),
                            magnet=bool(rflags & BT_REC_MAGNET), verdict=verdict))
    return dict(level=level, upgraded_s=upgraded_s, code1003=bool(flags & FW_F_CODE1003), alert_s=alert_s,
                tele_off=bool(flags & FW_F_TELE_OFF), disabled_s=disabled_s, why=why,
                explant=bool(flags & FW_F_EXPLANT), explant_cal=explant_cal, cleared=bool(flags & FW_F_CLEARED),
                tests=tests, next_test_s=next_test_s, run_1003=run_1003, run_dis=run_dis, history=history)


def parse_safety(raw):
    """The Safety Mode block: the latch, the time scale and the uncommanded-reset ring, oldest first."""
    latched, latched_s, total, scale, window_s, needed, _, _ = struct.unpack_from(SAFETY_HEADER_FORMAT, raw)
    recs = [struct.unpack_from(RESET_REC_FORMAT, raw, 32 + 8 * i) for i in range(RESET_SLOTS)]
    resets = [dict(num=n, implant_s=recs[n % RESET_SLOTS][0], cause=recs[n % RESET_SLOTS][1])
              for n in range(max(0, total - RESET_SLOTS), total)]
    return dict(latched=latched == SAFETY_LATCHED, latched_s=latched_s, total=total,
                scale=scale or 1_000_000, window_s=window_s, needed=needed, resets=resets)


# --- Live telemetry (STREAM) -----------------------------------------------

STREAM_HEADER = "<IBBBB"  # first sample, n samples, n markers, mode in effect, flags
STREAM_KEEPALIVE_S = 5.0  # well inside the device's 30 s session timeout
STREAM_CLOSE_S = 3.0      # how long the close waits; the device times out by itself anyway


def parse_stream(payload, uv_per_lsb):
    """One 0x48 frame: samples in mV and markers by sample number."""
    first, n, n_mk, mode, flags = struct.unpack_from(STREAM_HEADER, payload)
    off = struct.calcsize(STREAM_HEADER)
    samples = struct.unpack_from(f"<{2 * n}h", payload, off)
    off += 4 * n
    markers = [struct.unpack_from("<IB", payload, off + 5 * i) for i in range(n_mk)]
    mv = uv_per_lsb / 1000.0
    return dict(first=first, v=[round(x * mv, 3) for x in samples[0::2]],
                a=[round(x * mv, 3) for x in samples[1::2]],
                markers=[(sample, MARKERS.get(code, "?")) for sample, code in markers],
                mode=mode_name(mode), magnet=bool(flags & 1), batt=BATT_STATES[(flags >> 1) & 3])


class Broadcast:
    """Fan events out to every connected page, each only the events it asked
    for; a page that falls behind loses events rather than holding the
    others up."""

    # The latest of each of these is sent first to every page that connects:
    # the session's hello, and the battery probe's.
    STICKY = ("hello", "probe")
    # And the recent history of these, as one `<event>_backlog` list, so a
    # page opened (or reloaded) late still has it: the battery probe's
    # samples, about five minutes of them at its default rate.
    BACKLOG = {"battery": 6000}

    def __init__(self):
        self.lock = threading.Lock()
        self.queues = {}  # queue -> the events its page wants, or None for all
        self.sticky = {}
        self.backlog = {event: collections.deque(maxlen=n) for event, n in self.BACKLOG.items()}

    def subscribe(self, events=None):
        q = queue.Queue(maxsize=256)
        with self.lock:
            self.queues[q] = events
            for event, message in self.sticky.items():
                if events is None or event in events:
                    q.put_nowait(message)
            for event, items in self.backlog.items():
                if items and (events is None or event in events):
                    q.put_nowait(f"event: {event}_backlog\ndata: {json.dumps(list(items))}\n\n".encode())
        return q

    def unsubscribe(self, q):
        with self.lock:
            self.queues.pop(q, None)

    def send(self, event, data):
        message = f"event: {event}\ndata: {json.dumps(data)}\n\n".encode()
        with self.lock:
            if event in self.STICKY:
                self.sticky[event] = message
            if event in self.backlog:
                self.backlog[event].append(data)
            for q, events in self.queues.items():
                if events is not None and event not in events:
                    continue
                try:
                    q.put_nowait(message)
                except queue.Full:
                    pass


def serve_live_view(broadcast, port, bind="127.0.0.1", pages=None):
    """Local pages fed by Server-Sent Events at /events: `pages` maps each
    path to its file beside this one (default: the live EGM, live_view.html,
    at /). `/events?events=a,b` streams only those events (and their
    backlogs), so each page gets what it draws.

    Loopback by default: the page streams patient telemetry and has no
    authentication. `emerson run` binds 0.0.0.0 (EMERSON_BIND_HOST), because
    Docker publishes a port to the container's network interface, not its
    loopback -- and publishes it on the host's loopback only."""
    from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

    from urllib.parse import parse_qs, urlsplit

    pages = {path: Path(__file__).with_name(name).read_bytes()
             for path, name in (pages or {"/": "live_view.html"}).items()}

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_GET(self):
            url = urlsplit(self.path)
            if url.path in pages:
                page = pages[url.path]
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(page)))
                self.end_headers()
                self.wfile.write(page)
            elif url.path == "/events":
                wanted = parse_qs(url.query).get("events")
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Cache-Control", "no-cache")
                self.end_headers()
                q = broadcast.subscribe(set(",".join(wanted).split(",")) if wanted else None)
                try:
                    while True:
                        try:
                            self.wfile.write(q.get(timeout=10))
                        except queue.Empty:
                            self.wfile.write(b": keep-alive\n\n")
                        self.wfile.flush()
                except OSError:
                    pass
                finally:
                    broadcast.unsubscribe(q)
            else:
                self.send_error(404)

    server = ThreadingHTTPServer((bind, port), Handler)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


# How often a live session asks whether the wand is still placed.
WAND_CHECK_S = 1.0


def live(session, broadcast, now=None, stop=None):
    """Open a session, start the stream and pass every frame to `broadcast`
    until `stop` is set (or Ctrl+C); then stop the stream and end the
    session."""
    now = int(time.time()) if now is None else now
    stop = stop or threading.Event()
    raw_info = session.open(now)
    info = dict(zip([n for n, _ in INFO_FIELDS], struct.unpack_from(INFO_FORMAT, raw_info)))
    if info["proto"] < 2:
        raise RuntimeError(f"the device speaks protocol {info['proto']}; live telemetry needs 2 (firmware 1.1)")
    gaps = {"last": None, "count": 0}

    def on_stream(seq, payload):
        frame = parse_stream(payload, info["egm_uv_per_lsb"])
        if gaps["last"] is not None and frame["first"] != gaps["last"]:
            gaps["count"] += 1
            frame["gap"] = True
        gaps["last"] = frame["first"] + len(frame["v"])
        broadcast.send("egm", frame)

    session.on_stream = on_stream
    [ack] = session.transact([(REQ_STREAM, b"\x01")])
    _, sps, uv_per_lsb, clock = struct.unpack("<BHHI", ack)
    broadcast.send("hello", dict(sps=sps, uv_per_lsb=uv_per_lsb, clock=clock, mode=mode_name(info["mode"]),
                                 rate=info["rate"], utr=info["utr"], pav=info["pav"], sav=info["sav"],
                                 amp_v=info["amp"] / 1000, aamp_v=info["aamp"] / 1000, pw=info["pw"],
                                 batt_v=info["batt_mv"] / 1000, batt=BATT_STATES[info["batt_state"]]))
    next_keepalive = time.monotonic() + STREAM_KEEPALIVE_S
    next_check = time.monotonic() + WAND_CHECK_S
    lifted = False
    try:
        while not stop.is_set():
            session.pump(0.2)
            if time.monotonic() >= next_check:
                session.check_link()
                next_check = time.monotonic() + WAND_CHECK_S
            if time.monotonic() >= next_keepalive:
                session.transact([(REQ_STREAM, b"\x01")])
                next_keepalive = time.monotonic() + STREAM_KEEPALIVE_S
    except KeyboardInterrupt:
        pass
    except WandLifted as err:
        lifted = True
        print(err, file=sys.stderr)
    finally:
        # Best effort, and short: a second Ctrl+C skips it, and a device that
        # never confirms ends the session itself after its 30 s timeout.
        session.on_stream = None
        try:
            if not lifted:  # with the wand gone, the device has already ended it
                session.transact([(REQ_STREAM, b"\x00"), (REQ_END, b"")], timeout_s=STREAM_CLOSE_S)
        except (RuntimeError, TimeoutError):
            print("warning: the device did not confirm the end of the session; it will time out by itself",
                  file=sys.stderr)
    return gaps["count"]


# --- Bench battery probe ---------------------------------------------------
#
# Not telemetry: the emulator's own view of the cell, read straight from the
# models (/board/battery, /board/supervisor) the way a bench supply's meter
# would, so it keeps reading while the device is held in reset and shows the
# load current the board has no part to measure. Its page is battery_view.html
# (/battery), its own window beside the live EGM's, labelled as a probe.
#
# The rail's dips come from the cell's min-hold, which every sample restarts
# (`reset_min`): a sag far shorter than the poll interval still shows. That
# min-hold is shared, so while the probe runs, anything else reading it --
# scenarios/accolade_safety_mode.py's "rail minimum" at each reset -- sees
# only the time since the probe's last sample.

BATTERY_PROBE_HZ = 20.0
# The firmware's thresholds on its loaded VBAT reading (firmware/main.c ERI_MV, EOL_MV).
ERI_V = 2.550
EOL_V = 2.300
# The draws on the rail that are the radio's: the ZL70102, its load switch,
# and `set_draw radio`'s stand-in.
RADIO_DRAWS = ("zl70102", "radio_vsup", "radio")

RAIL_RE = re.compile(r"rail ([0-9.]+) V at guest ([0-9.]+) s; draws: (.*?); min ([0-9.]+) V")
DRAW_RE = re.compile(r"(\w+) ([0-9.]+) uA")
CELL_RE = {"dod": re.compile(r"([0-9.]+)% discharged"), "rint": re.compile(r"R_int ([0-9.e+]+) ohm"),
           "load": re.compile(r"load ([0-9.]+) uA"), "pacing": re.compile(r"\(([0-9.]+) uA pacing\)")}
DEFECT_RE = re.compile(r"defect ([^;]+)")
SUPERVISOR_RE = re.compile(r"VIT- ([0-9.]+) V \(\+([0-9.]+) mV hysteresis\), RESET (high|low)[^;]*; (\d+) resets")


def probe_sample(battery, supervisor):
    """One reading of the cell and its supervisor, or None if either answered
    in a form this does not know."""
    rail = RAIL_RE.search(battery.invoke_action("get_rail", ""))
    battery.invoke_action("reset_min", "")
    cell = battery.invoke_action("get_status", "")
    sup = SUPERVISOR_RE.search(supervisor.invoke_action("get_status", ""))
    if not rail or not sup:
        return None
    draws = {name: float(ua) for name, ua in DRAW_RE.findall(rail.group(3))}
    sample = dict(t=float(rail.group(2)), v=float(rail.group(1)), vmin=float(rail.group(4)),
                  radio=round(sum(ua for name, ua in draws.items() if name in RADIO_DRAWS), 2), draws=draws,
                  reset_low=sup.group(3) == "low", resets=int(sup.group(4)),
                  vit=float(sup.group(1)), hyst=float(sup.group(2)) / 1000)
    for key, rx in CELL_RE.items():
        m = rx.search(cell)
        sample[key] = float(m.group(1)) if m else None
    defect = DEFECT_RE.search(cell)
    sample["defect"] = defect.group(1) if defect else None
    return sample


def battery_probe(machine, broadcast, hz=BATTERY_PROBE_HZ, stop=None):
    """Sample the cell `hz` times a wall second and pass each reading to
    `broadcast` as a `battery` event, until `stop` is set (or Ctrl+C)."""
    stop = stop or threading.Event()
    battery = machine.get_device("/board/battery")
    supervisor = machine.get_device("/board/supervisor")
    period = 1.0 / hz
    hello = None
    next_t = time.monotonic()
    while not stop.is_set():
        sample = probe_sample(battery, supervisor)
        if sample:
            limits = dict(hz=hz, vit=sample.pop("vit"), hyst=sample.pop("hyst"), eri=ERI_V, eol=EOL_V)
            if limits != hello:
                broadcast.send("probe", limits)
                hello = limits
            broadcast.send("battery", sample)
        next_t = max(next_t + period, time.monotonic())
        stop.wait(next_t - time.monotonic())


def start_battery_probe(args, broadcast, stop):
    """battery_probe on a thread with its own connection to the session, so
    it never waits on the telemetry session's broker reads."""
    import emerson

    def run():
        try:
            with emerson.attach_session(args.host, args.project, args.session) as machine:
                battery_probe(machine, broadcast, args.probe_hz, stop)
        except Exception as err:  # the session ended under it; the telemetry side reports its own errors
            print(f"battery probe stopped: {err}", file=sys.stderr)

    thread = threading.Thread(target=run, name="battery-probe", daemon=True)
    thread.start()
    return thread


def parse_egm(raw):
    if len(raw) < EGM_SIZE:
        return None
    start_s, episode, kind, n_markers, n_samples, pre, sps, uv = struct.unpack_from(EGM_HEADER_FORMAT, raw)
    if kind == 0 or n_samples == 0:
        return None  # empty, or still recording
    markers = []
    for i in range(min(n_markers, EGM_MARKERS)):
        sample, code, _ = struct.unpack_from(EGM_MARKER_FORMAT, raw, 16 + 4 * i)
        markers.append((sample, MARKERS.get(code, "?")))
    samples = struct.unpack_from(f"<{2 * n_samples}h", raw, 16 + 4 * EGM_MARKERS)
    v = [s * uv / 1000.0 for s in samples[0::2]]  # mV
    a = [s * uv / 1000.0 for s in samples[1::2]]
    return dict(start_s=start_s, episode=episode, kind=kind, pre=pre, sps=sps, v=v, a=a, markers=markers)


# --- Reporting --------------------------------------------------------------

def implant_date(info, implant_s):
    return dt.datetime.fromtimestamp(info["epoch"] + implant_s, dt.timezone.utc)


def fmt_date(info, implant_s):
    return implant_date(info, implant_s).strftime("%Y-%m-%d")


def fmt_datetime(info, implant_s):
    return implant_date(info, implant_s).strftime("%Y-%m-%d %H:%M")


RED = "\x1b[1;31m"
RESET_COLOUR = "\x1b[0m"
SAFETY_ALERT = "SAFETY MODE -- replace device"


def safety_lines(r):
    """The alert and the reset log, for the terminal summary."""
    sf, info = r.get("safety"), r["info"]
    if not sf:
        return []
    lines = []
    if sf["latched"]:
        lines.append(f"{SAFETY_ALERT}: {sf['needed']} system resets within "
                     f"{sf['window_s'] // 3600} h latched {fmt_datetime(info, sf['latched_s'])} "
                     "(implant clock); VVI 72.5 ppm, RV unipolar 5.0 V @ 1.0 ms, 0.25 mV, magnet response off;"
                     " permanent")
    if sf["resets"]:
        lines.append(f"Uncommanded resets ({sf['total']} since implant, {sf['needed']} within "
                     f"{sf['window_s'] // 3600} h latches Safety Mode):")
        for x in sf["resets"]:
            lines.append(f"  #{x['num']:<3} {fmt_datetime(info, x['implant_s'])}  "
                         f"{RESET_CAUSES.get(x['cause'], x['cause'])}")
    lines.append(f"Implant clock {sf['scale']:g} implant s per guest s")
    return lines


def explant_date(fw):
    """The explant indicator's date on the device calendar (zero is 2000-01-01: the letter's bogus date)."""
    return (CAL_EPOCH + dt.timedelta(seconds=fw["explant_cal"])).strftime("%Y-%m-%d")


def fw_alerts(r):
    """The red alerts of the battery test: Code-1003, telemetry disabled, the explant indicator."""
    fw, info = r.get("fw"), r["info"]
    out = []
    if not fw:
        return out
    if fw["code1003"]:
        out.append(f"{CODE_1003} (raised {fmt_date(info, fw['alert_s'])}); contact Technical Services")
    if fw["tele_off"]:
        out.append(f"WANDLESS TELEMETRY DISABLED {fmt_date(info, fw['disabled_s'])} "
                   f"({DISABLE_REASONS.get(fw['why'], fw['why'])}); wanded telemetry still works")
    if fw["explant"]:
        text = f"EXPLANT INDICATOR reached {explant_date(fw)}"
        if info["batt_state"] == 0:
            text += " -- erroneous: the battery has not reached ERI"
        out.append(text)
    return out


def fw_lines(r):
    """The firmware level and the daily battery test's history, for the terminal summary."""
    fw, info = r.get("fw"), r["info"]
    if not fw:
        return []
    lines = [f"Firmware level: {FW_LEVEL_NAMES.get(fw['level'], fw['level'])}"
             + (f", installed {fmt_date(info, fw['upgraded_s'])}" if fw["level"] else "")
             + ("; wandless telemetry re-enabled after a false positive" if fw["cleared"] else "")]
    if not fw["level"]:
        return lines
    lines.append(f"Daily battery test: {fw['tests']} run, {fw['run_1003']}/4 in a row past the Code-1003 margin, "
                 f"{fw['run_dis']}/2 past the disable margin")
    if fw["history"]:
        lines.append("  date        projected  idle V   burst V  expected  margin  verdict")
        for t in fw["history"][-8:]:
            burst = f"{t['v_burst_mv'] / 1000:.3f}" if t["v_burst_mv"] else " --- "
            margin = f"{t['v_exp_mv'] - t['v_burst_mv']:4d} mV" if t["v_burst_mv"] else "   --  "
            lines.append(f"  {fmt_date(info, t['implant_s'])}  {t['longevity_y']:5.2f} y  {t['v_pre_mv'] / 1000:.3f}   "
                         f"{burst:>6}   {t['v_exp_mv'] / 1000:.3f}   {margin}  {BT_VERDICTS.get(t['verdict'], '?')}"
                         + (" [magnet]" if t["magnet"] else ""))
    return lines


def fmt_duration(dur_ms):
    if dur_ms == EP_ONGOING:
        return "ongoing"
    if dur_ms == 0:
        return "--"
    s = dur_ms / 1000
    return f"{s:.1f} s" if s < 120 else f"{s / 60:.1f} min"


def pct(n, d):
    return 100.0 * n / d if d else 0.0


def summary_text(r, color=None):
    info, c = r["info"], r["counters"]
    color = sys.stdout.isatty() if color is None else color
    lines = []
    sf = r.get("safety")
    if sf and sf["latched"]:
        banner = f"*** {SAFETY_ALERT} ***"
        lines.append(f"{RED}{banner}{RESET_COLOUR}" if color else banner)
    for alert in fw_alerts(r):
        banner = f"*** {alert} ***"
        lines.append(f"{RED}{banner}{RESET_COLOUR}" if color else banner)
    implanted = fmt_date(info, 0)
    years = info["implant_s"] / (365.25 * 86400)
    lines.append(f"Device: rp2350 pacemaker demo, firmware {info['fw_major']}.{info['fw_minor']}, "
                 f"protocol {info['proto']}")
    lines.append(f"Implanted {implanted} ({years:.1f} years ago); {info['power_ons']} power-on(s)")
    if info["last_session_s"]:
        lines.append(f"Last interrogated {fmt_date(info, info['last_session_s'])}")
    lines.append(f"Diagnostics since {fmt_date(info, info['last_clear_s'])}"
                 + ("" if info["eeprom_ok"] else "  [EEPROM not responding: RAM only]"))
    lines.append(f"Battery {info['batt_mv'] / 1000:.3f} V loaded, {BATT_STATES[info['batt_state']]}")
    rate = "72.5" if sf and sf["latched"] else info["rate"]
    lines.append(f"Mode {mode_name(info['mode'])}, lower rate {rate} ppm, UTR {info['utr']}, "
                 f"MSR {info['msr']}; PAV/SAV {info['pav']}/{info['sav']} ms, PVARP {info['pvarp']} ms; "
                 f"A {info['aamp'] / 1000:.2f} V, V {info['amp'] / 1000:.2f} V @ {info['pw'] / 1000:.2f} ms; "
                 f"magnet {'async' if info['magnet_response'] else 'off'}"
                 + (" (magnet present)" if info["magnet"] else ""))
    a_all = c["as"] + c["ap"] + c["ar"]
    v_all = c["vs"] + c["vp"]
    lines.append("")
    lines.append(f"Atrial:      {a_all} events  AS {c['as']}  AP {c['ap']}  AR {c['ar']}  "
                 f"-> {pct(c['ap'], a_all):.1f}% paced")
    lines.append(f"Ventricular: {v_all} events  VS {c['vs']}  VP {c['vp']}  VR {c['vr']}  "
                 f"-> {pct(c['vp'], v_all):.1f}% paced")
    seqs = c["as_vs"] + c["as_vp"] + c["ap_vs"] + c["ap_vp"]
    lines.append(f"AV sequences: AS-VS {pct(c['as_vs'], seqs):.1f}%  AS-VP {pct(c['as_vp'], seqs):.1f}%  "
                 f"AP-VS {pct(c['ap_vs'], seqs):.1f}%  AP-VP {pct(c['ap_vp'], seqs):.1f}%")
    lines.append(f"PVCs {c['pvc']}; episodes: AHR {c['ahr']}, VHR {c['vhr']}, lead-off {c['lead_off']}, "
                 f"magnet {c['magnet']}, mode changes {c['mode_changes']}, noise reversions {c['noise']}, "
                 f"ventricular pauses {c['pauses']}")
    lines.append(f"Lifetime paces: AP {info['lifetime_ap']}, VP {info['lifetime_vp']}")
    fwl = fw_lines(r)
    if fwl:
        lines.append("")
        lines += fwl
    extra = safety_lines(r)
    if extra:
        lines.append("")
        lines += [f"{RED}{x}{RESET_COLOUR}" if color and x.startswith(SAFETY_ALERT) else x for x in extra]
    lines.append("")
    lines.append(f"Episodes ({len(r['episodes'])} of {info['episodes']} since the last clear):")
    for e in r["episodes"]:
        strip = f"  [EGM {e['strip']['slot']}]" if e["strip"] else ""
        lines.append(f"  #{e['num']:<3} {fmt_date(info, e['start_s'])}  {EPISODE_NAMES.get(e['kind'], e['kind']):<24}"
                     f" {fmt_duration(e['dur_ms']):>9}  {episode_detail(e)}{strip}")
    if r["trends"]:
        t0, t1 = r["trends"][0], r["trends"][-1]
        lines.append("")
        lines.append(f"Trends: {len(r['trends'])} points, implant day {t0['day']} to {t1['day']}; battery "
                     f"{t0['batt_mv'] / 1000:.3f} -> {t1['batt_mv'] / 1000:.3f} V")
    if r["cleared"]:
        lines.append("")
        lines.append("Diagnostics cleared.")
    return "\n".join(lines)


CSS = """
:root { --bg:#ffffff; --fg:#1d232b; --muted:#5f6b7a; --line:#d9dee5; --panel:#f5f7fa;
  --v:#1f6fd1; --a:#c2410c; --sir:#6d4fc2; --ok:#16794a; --warn:#b45309; --bad:#b91c1c; }
@media (prefers-color-scheme: dark) { :root { --bg:#14181d; --fg:#e6e9ee; --muted:#9aa6b4; --line:#2c333c;
  --panel:#1b2027; --v:#6ea8ff; --a:#fb923c; --sir:#a78bfa; --ok:#4ade80; --warn:#fbbf24; --bad:#f87171; } }
* { box-sizing: border-box; }
body { margin:0; background:var(--bg); color:var(--fg);
  font:14px/1.45 system-ui,-apple-system,"Segoe UI",sans-serif; }
main { max-width:1100px; margin:0 auto; padding:24px 16px 48px; }
h1 { font-size:22px; margin:0 0 4px; } h2 { font-size:16px; margin:32px 0 10px; }
.muted { color:var(--muted); }
.grid { display:grid; grid-template-columns:repeat(auto-fit,minmax(160px,1fr)); gap:10px; }
.tile { background:var(--panel); border:1px solid var(--line); border-radius:8px; padding:10px 12px; }
.tile .k { color:var(--muted); font-size:12px; } .tile .v { font-size:20px; font-weight:600; }
table { border-collapse:collapse; width:100%; font-variant-numeric:tabular-nums; }
th,td { text-align:left; padding:6px 8px; border-bottom:1px solid var(--line); vertical-align:top; }
th { color:var(--muted); font-weight:500; font-size:12px; }
.scroll { overflow-x:auto; }
svg text { fill:var(--muted); font-size:11px; }
.badge { display:inline-block; padding:1px 6px; border-radius:4px; font-size:12px; border:1px solid var(--line); }
.alert { background:var(--bad); color:#fff; padding:12px 16px; border-radius:8px; font-size:18px;
  font-weight:700; margin:12px 0; }
.ok { color:var(--ok); } .warn { color:var(--warn); } .bad { color:var(--bad); }
figure { margin:0 0 18px; } figcaption { font-size:13px; margin-bottom:4px; }
"""


def svg_bars(series, colors, labels, width=520, height=160):
    """Stacked bars: `series` is a list of equal-length count lists."""
    n = len(labels)
    totals = [sum(s[i] for s in series) for i in range(n)]
    peak = max(totals) or 1
    pad_l, pad_b = 34, 28
    bw = (width - pad_l) / n
    out = [f'<svg viewBox="0 0 {width} {height}" width="100%" role="img">']
    plot_h = height - pad_b - 8
    out.append(f'<text x="0" y="12">{peak}</text><text x="0" y="{height - pad_b}">0</text>')
    for i in range(n):
        y = height - pad_b
        for s, color in zip(series, colors):
            h = plot_h * s[i] / peak
            if h > 0:
                out.append(f'<rect x="{pad_l + i * bw + 2:.1f}" y="{y - h:.1f}" width="{bw - 4:.1f}" '
                           f'height="{h:.1f}" fill="{color}"><title>{labels[i]} bpm: {s[i]}</title></rect>')
            y -= h
        if i % 2 == 0:
            out.append(f'<text x="{pad_l + i * bw + bw / 2:.1f}" y="{height - 10}" '
                       f'text-anchor="middle">{labels[i]}</text>')
    out.append("</svg>")
    return "".join(out)


def svg_line(xs, ys, color, width=520, height=140, y_fmt="{:.0f}", x_label="implant day"):
    if not xs:
        return '<p class="muted">No data yet.</p>'
    pad_l, pad_b = 44, 24
    x0, x1 = min(xs), max(xs)
    y0, y1 = min(ys), max(ys)
    if y1 == y0:
        y0, y1 = y0 - 1, y1 + 1
    if x1 == x0:
        x1 = x0 + 1
    sx = lambda x: pad_l + (width - pad_l - 8) * (x - x0) / (x1 - x0)
    sy = lambda y: 8 + (height - pad_b - 8) * (1 - (y - y0) / (y1 - y0))
    pts = " ".join(f"{sx(x):.1f},{sy(y):.1f}" for x, y in zip(xs, ys))
    return (f'<svg viewBox="0 0 {width} {height}" width="100%" role="img">'
            f'<text x="0" y="14">{y_fmt.format(y1)}</text>'
            f'<text x="0" y="{height - pad_b}">{y_fmt.format(y0)}</text>'
            f'<text x="{pad_l}" y="{height - 6}">{x0}</text>'
            f'<text x="{width - 8}" y="{height - 6}" text-anchor="end">{x1} ({x_label})</text>'
            f'<polyline points="{pts}" fill="none" stroke="{color}" stroke-width="1.6"/></svg>')


def svg_egm(egm, width=1040):
    """Both channels, the trigger and the marker annotations."""
    n = len(egm["v"])
    lane, gap, top = 90, 18, 22
    height = top + 2 * lane + gap + 26
    def lane_path(samples, y_mid, scale):
        step = width / n
        return " ".join(f"{i * step:.1f},{y_mid - max(-lane / 2, min(lane / 2, s * scale)):.1f}"
                        for i, s in enumerate(samples))
    def scale_for(samples):
        # Scale to the cardiac signal, not the pacing artefact: the 98th
        # percentile of |x| fills the lane, and the pulses clip.
        mags = sorted(abs(s) for s in samples)
        ref = mags[int(len(mags) * 0.98)] if mags else 1.0
        return (lane / 2 - 4) / max(ref, 0.5)
    sv, sa = scale_for(egm["v"]), scale_for(egm["a"])
    y_a = top + lane / 2
    y_v = top + lane + gap + lane / 2
    out = [f'<svg viewBox="0 0 {width} {height}" width="100%" role="img">']
    for sec in range(int(n / egm["sps"]) + 1):
        x = sec * egm["sps"] * width / n
        out.append(f'<line x1="{x:.1f}" x2="{x:.1f}" y1="{top}" y2="{height - 26}" stroke="var(--line)"/>')
        out.append(f'<text x="{x + 2:.1f}" y="{height - 10}">{sec - egm["pre"] / egm["sps"]:+.0f} s</text>')
    tx = egm["pre"] * width / n
    out.append(f'<line x1="{tx:.1f}" x2="{tx:.1f}" y1="{top - 6}" y2="{height - 26}" stroke="var(--bad)" '
               f'stroke-dasharray="4 3"/>')
    out.append(f'<text x="2" y="{top - 8}">A (CH2)</text><text x="2" y="{top + lane + gap - 4}">V (CH1)</text>')
    out.append(f'<polyline points="{lane_path(egm["a"], y_a, sa)}" fill="none" stroke="var(--a)" stroke-width="1"/>')
    out.append(f'<polyline points="{lane_path(egm["v"], y_v, sv)}" fill="none" stroke="var(--v)" stroke-width="1"/>')
    last_x = {True: -100.0, False: -100.0}
    for sample, code in egm["markers"]:
        x = sample * width / n
        atrial = code.startswith("A")
        # A label too close to the last one in its lane drops a line.
        nudge = 12 if x - last_x[atrial] < 22 else 0
        last_x[atrial] = x if not nudge else -100.0
        y = (top + 10 if atrial else top + lane + gap + 10) + nudge
        color = "var(--a)" if atrial else "var(--v)"
        out.append(f'<text x="{x:.1f}" y="{y}" text-anchor="middle" style="fill:{color};font-weight:600">{code}</text>')
    out.append("</svg>")
    return "".join(out)


def html_report(r):
    info, c = r["info"], r["counters"]
    e = html.escape
    a_all = c["as"] + c["ap"] + c["ar"]
    v_all = c["vs"] + c["vp"]
    seqs = c["as_vs"] + c["as_vp"] + c["ap_vs"] + c["ap_vp"]
    batt_cls = ["ok", "warn", "bad"][info["batt_state"]]
    session = dt.datetime.fromtimestamp(info["session_unix"], dt.timezone.utc)
    years = info["implant_s"] / (365.25 * 86400)

    def tile(k, v, cls=""):
        return f'<div class="tile"><div class="k">{e(k)}</div><div class="v {cls}">{v}</div></div>'

    parts = [f"<!doctype html><html lang='en'><head><meta charset='utf-8'>"
             f"<meta name='viewport' content='width=device-width,initial-scale=1'>"
             f"<title>Pacemaker Interrogation</title><style>{CSS}</style></head><body><main>",
             "<h1>Pacemaker interrogation</h1>"
             + (f"<div class='alert'>{e(SAFETY_ALERT)}</div>" if r.get("safety") and r["safety"]["latched"] else "")
             + "".join(f"<div class='alert'>{e(a)}</div>" for a in fw_alerts(r)),
             f"<p class='muted'>rp2350 pacemaker demo, firmware {info['fw_major']}.{info['fw_minor']} &middot; "
             f"session {session:%Y-%m-%d %H:%M} UTC &middot; implanted {fmt_date(info, 0)} "
             f"({years:.1f} years) &middot; diagnostics since {fmt_date(info, info['last_clear_s'])}"
             + (" &middot; <span class='bad'>EEPROM not responding</span>" if not info["eeprom_ok"] else "")
             + "</p>",
             "<div class='grid'>",
             tile("Battery (loaded)", f"{info['batt_mv'] / 1000:.3f} V", batt_cls),
             tile("Battery status", BATT_STATES[info["batt_state"]], batt_cls),
             tile("Mode", e(mode_name(info["mode"]))),
             tile("Atrial paced", f"{pct(c['ap'], a_all):.1f}%"),
             tile("Ventricular paced", f"{pct(c['vp'], v_all):.1f}%"),
             tile("PVCs", str(c["pvc"])),
             "</div>"]

    parts.append("<h2>Programmed parameters</h2><div class='scroll'><table><tr>"
                 + "".join(f"<th>{h}</th>" for h in ["Lower rate", "UTR", "MSR", "PAV", "SAV", "PVARP",
                                                       "A amp", "V amp", "Pulse width", "A sens", "V sens",
                                                       "Magnet"])
                 + "</tr><tr>"
                 + "".join(f"<td>{v}</td>" for v in [
                     f"{info['rate']} ppm", f"{info['utr']} ppm", f"{info['msr']} ppm", f"{info['pav']} ms",
                     f"{info['sav']} ms", f"{info['pvarp']} ms", f"{info['aamp'] / 1000:.2f} V",
                     f"{info['amp'] / 1000:.2f} V", f"{info['pw'] / 1000:.2f} ms",
                     f"{info['asens'] / 1000:.1f} mV", f"{info['sens'] / 1000:.1f} mV",
                     "async" if info["magnet_response"] else "off"])
                 + "</tr></table></div>")

    parts.append("<h2>Counters</h2><div class='scroll'><table>"
                 "<tr><th>Chamber</th><th>Sensed</th><th>Paced</th><th>Refractory</th><th>% paced</th></tr>"
                 f"<tr><td>Atrium</td><td>{c['as']}</td><td>{c['ap']}</td><td>{c['ar']}</td>"
                 f"<td>{pct(c['ap'], a_all):.1f}%</td></tr>"
                 f"<tr><td>Ventricle</td><td>{c['vs']}</td><td>{c['vp']}</td><td>{c['vr']}</td>"
                 f"<td>{pct(c['vp'], v_all):.1f}%</td></tr></table></div>"
                 "<div class='scroll'><table><tr><th>AS-VS</th><th>AS-VP</th><th>AP-VS</th><th>AP-VP</th>"
                 "<th>PVCs</th><th>AHR</th><th>VHR</th><th>Lead-off</th><th>Magnet</th><th>Mode changes</th>"
                 "<th>Noise reversions</th><th>Pauses</th></tr><tr>"
                 + "".join(f"<td>{v}</td>" for v in [
                     f"{pct(c['as_vs'], seqs):.1f}%", f"{pct(c['as_vp'], seqs):.1f}%",
                     f"{pct(c['ap_vs'], seqs):.1f}%", f"{pct(c['ap_vp'], seqs):.1f}%",
                     c["pvc"], c["ahr"], c["vhr"], c["lead_off"], c["magnet"], c["mode_changes"],
                     c["noise"], c["pauses"]])
                 + f"</tr></table></div><p class='muted'>Lifetime paces: AP {info['lifetime_ap']}, "
                 f"VP {info['lifetime_vp']}. Power-ons: {info['power_ons']}.</p>")

    h = r["hists"]
    parts.append("<h2>Rate histograms</h2><div class='grid' style='grid-template-columns:repeat(auto-fit,minmax(300px,1fr))'>")
    parts.append("<figure><figcaption>Atrial rate <span style='color:var(--a)'>&#9632; sensed</span> "
                 "<span class='muted'>&#9632; paced</span></figcaption>"
                 + svg_bars([h[0], h[1]], ["var(--a)", "var(--muted)"], HIST_BINS) + "</figure>")
    parts.append("<figure><figcaption>Ventricular rate <span style='color:var(--v)'>&#9632; sensed</span> "
                 "<span class='muted'>&#9632; paced</span></figcaption>"
                 + svg_bars([h[2], h[3]], ["var(--v)", "var(--muted)"], HIST_BINS) + "</figure>")
    parts.append("<figure><figcaption>Sensor-indicated rate (one count per 2 s)</figcaption>"
                 + svg_bars([h[4]], ["var(--sir)"], HIST_BINS) + "</figure></div>")

    fw = r.get("fw")
    if fw:
        parts.append("<h2>Firmware and battery test</h2>")
        parts.append(f"<p>Firmware level <b>{e(FW_LEVEL_NAMES.get(fw['level'], str(fw['level'])))}</b>"
                     + (f", installed {fmt_date(info, fw['upgraded_s'])}" if fw["level"] else "")
                     + (". <span class='ok'>Wandless telemetry was re-enabled after a false positive.</span>"
                        if fw["cleared"] else "")
                     + f". Wandless telemetry: "
                     + ("<span class='bad'>DISABLED</span>" if fw["tele_off"] else "<span class='ok'>enabled</span>")
                     + f". Code-1003: " + ("<span class='bad'>raised</span>" if fw["code1003"] else "none")
                     + f". Explant indicator: "
                     + (f"<span class='bad'>reached {explant_date(fw)}"
                        + (" (erroneous: the battery has not reached ERI)" if info["batt_state"] == 0 else "")
                        + "</span>" if fw["explant"] else "not reached") + ".</p>")
        if fw["level"] and fw["history"]:
            parts.append("<p class='muted'>The daily test sends a ~3 ms radio burst and reads the cell during it; "
                         "the burst voltage is compared with what a healthy cell shows for the projected "
                         "remaining longevity.</p><div class='scroll'><table><tr><th>#</th><th>Date</th>"
                         "<th>Projected longevity</th><th>Idle V</th><th>Burst V</th><th>Expected V</th>"
                         "<th>Margin</th><th>Verdict</th></tr>")
            for t in reversed(fw["history"]):
                burst = f"{t['v_burst_mv'] / 1000:.3f} V" if t["v_burst_mv"] else "no sample"
                margin = f"{t['v_exp_mv'] - t['v_burst_mv']} mV" if t["v_burst_mv"] else "&mdash;"
                cls = "bad" if t["verdict"] in (2, 4) else "warn" if t["verdict"] in (1, 3) else ""
                parts.append(f"<tr><td>{t['num']}</td><td>{fmt_date(info, t['implant_s'])}</td>"
                             f"<td>{t['longevity_y']:.2f} y</td><td>{t['v_pre_mv'] / 1000:.3f} V</td>"
                             f"<td>{burst}</td><td>{t['v_exp_mv'] / 1000:.3f} V</td><td>{margin}</td>"
                             f"<td class='{cls}'>{e(BT_VERDICTS.get(t['verdict'], '?'))}"
                             + (" (magnet response in force)" if t["magnet"] else "") + "</td></tr>")
            parts.append("</table></div>")
    sf = r.get("safety")
    if sf:
        parts.append("<h2>Resets and Safety Mode</h2>")
        parts.append("<p>" + " ".join(e(x) for x in safety_lines(r)[:1] if x.startswith(SAFETY_ALERT)) + "</p>")
        parts.append(f"<p class='muted'>{sf['needed']} uncommanded system resets within {sf['window_s'] // 3600} "
                     f"implant hours latch Safety Mode, which nothing clears. Implant clock: "
                     f"{sf['scale']:g} implant s per guest s.</p>")
        if sf["resets"]:
            parts.append("<div class='scroll'><table><tr><th>#</th><th>Implant time</th><th>Cause</th></tr>"
                         + "".join(f"<tr><td>{x['num']}</td><td>{fmt_datetime(info, x['implant_s'])}</td>"
                                   f"<td>{e(RESET_CAUSES.get(x['cause'], str(x['cause'])))}</td></tr>"
                                   for x in sf["resets"]) + "</table></div>")
        else:
            parts.append("<p class='muted'>No uncommanded resets logged.</p>")
    parts.append(f"<h2>Episodes</h2><p class='muted'>{len(r['episodes'])} shown of {info['episodes']} "
                 "logged since the last clear. Dates are on the implant clock; durations are heart time.</p>"
                 "<div class='scroll'><table><tr><th>#</th><th>Date</th><th>Episode</th><th>Duration</th>"
                 "<th>Detail</th><th>EGM</th></tr>")
    for ep in reversed(r["episodes"]):
        strip = f"<a href='#egm{ep['strip']['slot']}'>strip {ep['strip']['slot']}</a>" if ep["strip"] else ""
        parts.append(f"<tr><td>{ep['num']}</td><td>{fmt_date(info, ep['start_s'])}</td>"
                     f"<td>{e(EPISODE_NAMES.get(ep['kind'], str(ep['kind'])))}</td>"
                     f"<td>{fmt_duration(ep['dur_ms'])}</td><td>{e(episode_detail(ep))}</td><td>{strip}</td></tr>")
    parts.append("</table></div>")

    t = r["trends"]
    years_x = [round(p["day"] / 365.25, 2) for p in t]
    parts.append(f"<h2>Trends</h2><p class='muted'>One point per {info['trend_period_s'] / 86400:.1f} implant days "
                 f"({len(t)} points).</p><div class='grid' style='grid-template-columns:repeat(auto-fit,minmax(300px,1fr))'>")
    parts.append("<figure><figcaption>Battery, loaded (V)</figcaption>"
                 + svg_line(years_x, [p["batt_mv"] / 1000 for p in t], "var(--ok)", y_fmt="{:.3f}",
                            x_label="implant years") + "</figure>")
    parts.append("<figure><figcaption>Activity (mg)</figcaption>"
                 + svg_line(years_x, [p["act_mg"] for p in t], "var(--sir)", x_label="implant years") + "</figure>")
    parts.append("<figure><figcaption>% ventricular paced</figcaption>"
                 + svg_line(years_x, [p["pct_vp"] for p in t], "var(--v)", x_label="implant years") + "</figure>")
    parts.append("<figure><figcaption>Mean ventricular rate (bpm)</figcaption>"
                 + svg_line(years_x, [p["v_rate"] for p in t], "var(--v)", x_label="implant years") + "</figure>")
    parts.append("</div>")

    parts.append("<h2>Stored electrograms</h2>")
    if not r["egms"]:
        parts.append("<p class='muted'>None stored.</p>")
    for egm in sorted(r["egms"], key=lambda g: g["start_s"]):
        parts.append(f"<figure id='egm{egm['slot']}'><figcaption>Strip {egm['slot']}: "
                     f"{e(EPISODE_NAMES.get(egm['kind'], str(egm['kind'])))}, episode {egm['episode']}, "
                     f"{fmt_date(info, egm['start_s'])} &middot; {egm['sps']} SPS, "
                     f"{egm['pre'] / egm['sps']:.0f} s before the trigger (dashed)</figcaption>"
                     + svg_egm(egm) + "</figure>")
    if r["cleared"]:
        parts.append("<p><span class='badge'>Diagnostics cleared at the end of this session</span></p>")
    parts.append("</main></body></html>")
    return "".join(parts)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--host", default=os.environ.get("EMERSON_HOST", "http://localhost:10314"),
                   help="franklin server (default: EMERSON_HOST, else %(default)s)")
    p.add_argument("--project", default=os.environ.get("EMERSON_PROJECT", DEFAULT_PROJECT),
                   help="use this project's session (default: EMERSON_PROJECT, else %(default)s)")
    p.add_argument("--session", default=os.environ.get("EMERSON_SESSION"), metavar="TOKEN",
                   help="use the session with this token (default: EMERSON_SESSION)")
    p.add_argument("--html", default="pacemaker-interrogation.html", metavar="PATH",
                   help="where to write the HTML report (default: %(default)s)")
    p.add_argument("--clear", action="store_true",
                   help="clear counters, histograms, episodes and EGMs after reading them, as at the end "
                        "of a follow-up")
    p.add_argument("--link", choices=("wand", "communicator"), default="wand",
                   help="how to reach the implant: the NFC wand (default), or wandless through the bedside "
                        "communicator's radio (/patient/communicator, broker `communicator`)")
    p.add_argument("--await-implant", type=float, default=None, metavar="SECONDS",
                   help="with --link communicator: do not send the wake-up; wait up to this long for the "
                        "implant's own automatic remote-monitoring wakeup and take that session")
    p.add_argument("--upgrade", choices=sorted(FW_UPGRADE_ARGS), default=None,
                   help="first move the device's firmware level forward, as the clinic's programmer does with a "
                        "download (here one telemetry request; a modelling simplification), then interrogate it")
    p.add_argument("--timeout", type=float, default=60.0, help="seconds to wait for each response batch")
    p.add_argument("--live", action="store_true",
                   help="stream the live electrogram and markers to a local page instead of interrogating")
    p.add_argument("--battery-probe", action="store_true",
                   help="serve a page at /battery of the cell's rail voltage and load current, read straight from "
                        "the emulator's battery model: a bench probe, not telemetry, so it keeps reading while the "
                        "device is in reset. With --live it sits beside the EGM page (/) for a window of its own; "
                        "without, no wand is placed")
    p.add_argument("--probe-hz", type=float, default=BATTERY_PROBE_HZ,
                   help="battery probe samples per wall second (default: %(default)s)")
    p.add_argument("--port", type=int, default=8765, help="the live page's port (default: %(default)s)")
    p.add_argument("--bind", default=os.environ.get("EMERSON_BIND_HOST", "127.0.0.1"), metavar="ADDR",
                   help="the address the live page listens on (default: EMERSON_BIND_HOST, else %(default)s)")
    args = p.parse_args(argv)

    import emerson

    try:
        with emerson.attach_session(args.host, args.project, args.session) as machine:
            if args.battery_probe and not args.live:
                # The probe alone: no wand, no telemetry session.
                broadcast = Broadcast()
                server = serve_live_view(broadcast, args.port, args.bind,
                                         {"/": "battery_view.html", "/battery": "battery_view.html"})
                print(f"battery probe: http://localhost:{args.port}/battery  (Ctrl+C to stop)", flush=True)
                try:
                    battery_probe(machine, broadcast, args.probe_hz)
                except KeyboardInterrupt:
                    pass
                finally:
                    server.shutdown()
                return 0
            link = (CommunicatorLink(machine, await_implant_s=args.await_implant)
                    if args.link == "communicator" else WandLink(machine))
            try:
                if args.live:
                    broadcast = Broadcast()
                    pages = {"/": "live_view.html"}
                    if args.battery_probe:
                        pages["/battery"] = "battery_view.html"
                    server = serve_live_view(broadcast, args.port, args.bind, pages)
                    stop_probe = threading.Event()
                    if args.battery_probe:
                        start_battery_probe(args, broadcast, stop_probe)
                        print(f"battery probe: http://localhost:{args.port}/battery", flush=True)
                    print(f"live telemetry: http://localhost:{args.port}/  (Ctrl+C to end the session)", flush=True)
                    try:
                        live(Session(link, timeout_s=args.timeout), broadcast)
                    finally:
                        stop_probe.set()
                        server.shutdown()
                    return 0
                report = interrogate(Session(link, timeout_s=args.timeout), clear=args.clear,
                                     upgrade=args.upgrade)
            finally:
                link.close()
    except (RuntimeError, TimeoutError) as err:
        print(f"error: {err}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130
    print(summary_text(report))
    Path(args.html).write_text(html_report(report))
    print(f"\nHTML report: {args.html}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
