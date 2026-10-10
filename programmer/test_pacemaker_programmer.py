"""Host-side checks for pacemaker_programmer.py: the codec, frame parsing, and
an interrogation against a fake link that answers the way the firmware does.
The firmware itself is verified live (see ROADMAP.md, "Diagnostics").

    emerson run programmer/test_pacemaker_programmer.py -q    # from the repo root
"""

import struct
import sys
import time

import pytest

import pacemaker_programmer as pp


def test_crc16_ccitt_false_check_value():
    # The catalogue check value for CRC-16/CCITT-FALSE.
    assert pp.crc16(b"123456789") == 0x29B1


@pytest.mark.parametrize("data", [
    b"", b"\x00", b"\x00\x00", b"\x11\x22\x00\x33", bytes(range(1, 255)), bytes(range(1, 256)),
    bytes([1] * 254 + [0]), bytes([7] * 600), bytes(range(256)) * 3,
])
def test_cobs_round_trip_has_no_zero(data):
    enc = pp.cobs_encode(data)
    assert 0 not in enc
    assert pp.cobs_decode(enc) == data


def test_parse_frames_skips_console_text_and_bad_crc():
    good = pp.encode_frame(0x82, 7, b"\x01\x02\x00\x03")
    bad = bytearray(pp.encode_frame(0x83, 8, b"\x05"))
    bad[1] ^= 0x40
    stream = b"> telemetry\r\n[prog] telemetry session open\r\n\x00" + bytes(bad) + good + b"garbage"
    assert pp.parse_frames(stream) == [(0x82, 7, b"\x01\x02\x00\x03")]


def test_layouts_match_the_firmware():
    # firmware/main.c: build_info() writes 93 bytes; the store structures.
    assert struct.calcsize(pp.INFO_FORMAT) == 93
    assert struct.calcsize(pp.COUNTERS_FORMAT) == 128
    assert struct.calcsize(pp.HIST_FORMAT) == 320
    assert struct.calcsize(pp.EPISODE_FORMAT) == 16
    assert struct.calcsize(pp.TREND_FORMAT) == 12
    assert 16 + 4 * pp.EGM_MARKERS + 1500 * 4 == pp.EGM_SIZE


class FakeDevice:
    """A link that answers every request the way the firmware does. A
    response can still be in the answer file from an old session when the
    wand is placed."""

    def __init__(self, info, store):
        self.info, self.store = info, store
        self.out = bytearray(pp.encode_frame(0x82, 0, b"\x00" * 128))  # a stale response
        self.writes = []
        self.streaming, self.clock, self.stream_seq = False, 1000, 0

    def send(self, data):
        data = bytes(data)
        self.writes.append(data)
        for kind, seq, payload in self._requests(data):
            self.out += pp.encode_frame(*self._answer(kind, seq, payload))

    def read(self, timeout):
        if self.streaming:  # one 100 ms frame per read, with a VS marker in it
            self.out += pp.encode_frame(pp.MSG_STREAM, self.stream_seq % 256,
                                        stream_payload(self.clock, [(100, 50)] * 25, [(self.clock + 10, 4)]))
            self.clock += 25
            self.stream_seq += 1
        data, self.out = bytes(self.out), bytearray()
        return data

    @staticmethod
    def _requests(data):
        return pp.parse_frames(data)

    def _answer(self, kind, seq, payload):
        s = self.store
        if kind == pp.REQ_OPEN:
            return 0x81, seq, struct.pack(pp.INFO_FORMAT, *[self.info[n] for n, _ in pp.INFO_FIELDS])
        if kind == pp.REQ_COUNTERS:
            return 0x82, seq, s["counters"]
        if kind == pp.REQ_HISTOGRAMS:
            return 0x83, seq, s["hist"]
        if kind == pp.REQ_EPISODES:
            first, count = payload
            return 0x84, seq, bytes([first, count]) + s["episodes"][first * 16:(first + count) * 16]
        if kind == pp.REQ_TRENDS:
            first, count = struct.unpack("<HH", payload)
            return 0x85, seq, payload + s["trends"][first * 12:(first + count) * 12]
        if kind == pp.REQ_EGM:
            return 0x86, seq, payload + s["egm"]
        if kind == pp.REQ_STREAM:
            self.streaming = bool(payload[0])
            return 0x88, seq, struct.pack("<BHHI", self.streaming, 250, 16, self.clock)
        if kind == pp.REQ_SAFETY and "safety" in s:
            return 0x89, seq, s["safety"]
        if kind == pp.REQ_FWINFO and "fw" in s:
            return 0x8A, seq, s["fw"]
        if kind == pp.REQ_UPGRADE and "fw" in s:
            level = payload[0]
            if level <= struct.unpack_from("<I", s["fw"])[0] or level > pp.FW_FIXED:
                return pp.RSP_ERROR, seq, bytes([kind, 3])  # ERR_ARGS: no downgrade
            s["fw"] = struct.pack("<I", level) + s["fw"][4:]
            return 0x8B, seq, s["fw"]
        if kind in (pp.REQ_CLEAR, pp.REQ_END):
            return kind | 0x80, seq, b""
        return pp.RSP_ERROR, seq, bytes([kind, 2])


def stream_payload(first, samples, markers, mode=0, flags=0):
    body = struct.pack(pp.STREAM_HEADER, first, len(samples), len(markers), mode, flags)
    body += b"".join(struct.pack("<hh", v, a) for v, a in samples)
    return body + b"".join(struct.pack("<IB", s, c) for s, c in markers)


def make_fake():
    info = {n: 0 for n, _ in pp.INFO_FIELDS}
    info.update(proto=1, fw_major=1, eeprom_ok=1, implant_s=300_000_000, episodes=2, trends=3, egms=1,
                batt_mv=2795, mode=0, rate=60, utr=120, msr=120, pav=220, sav=190, pvarp=250, pw=400,
                amp=2500, aamp=2500, sens=2500, asens=1000, ep_slots=64, egm_slots=4, trend_slots=256,
                egm_samples=1500, egm_pre=500, egm_sps=250, egm_uv_per_lsb=16, trend_period_s=8_000_000,
                implant_s_per_guest_s=1_000_000, power_ons=1)
    counters = struct.pack("<32I", 100, 2, 3, 90, 12, 1, 4, 80, 10, 1, 1, *([0] * 21))
    hist = struct.pack("<80I", *range(80))
    episodes = (struct.pack(pp.EPISODE_FORMAT, 0, 0, pp.EP_POWER_ON, pp.EGM_NONE, 1, 0, 0)
                + struct.pack(pp.EPISODE_FORMAT, 250_000_000, 6100, pp.EP_MAGNET, 0, 1, 0, 0))
    trends = b"".join(struct.pack(pp.TREND_FORMAT, d, 2800 - d, 4, 0, 1, 70, 70, 0, 0) for d in (92, 185, 277))
    egm = bytearray(pp.EGM_SIZE)
    struct.pack_into(pp.EGM_HEADER_FORMAT, egm, 0, 250_000_000, 1, pp.EP_MAGNET, 2, 1500, 500, 250, 16)
    struct.pack_into(pp.EGM_MARKER_FORMAT, egm, 16, 480, 2, 0)
    struct.pack_into(pp.EGM_MARKER_FORMAT, egm, 20, 520, 5, 0)
    struct.pack_into("<h", egm, 16 + 128 + 4 * 600, 625)  # 10 mV on V
    return FakeDevice(info, dict(counters=counters, hist=hist, episodes=episodes, trends=trends, egm=bytes(egm)))


def test_interrogation_against_a_fake_device():
    fake = make_fake()
    session = pp.Session(fake, timeout_s=1)
    session.seq = 255  # OPEN wraps to seq 0, like the stale frame
    r = pp.interrogate(session, clear=True, now=1_800_000_000)

    assert r["info"]["epoch"] == 1_800_000_000 - 300_000_000
    assert r["counters"]["as"] == 100 and r["counters"]["pvc"] == 4
    assert r["hists"][4][15] == 79
    assert [e["kind"] for e in r["episodes"]] == [pp.EP_POWER_ON, pp.EP_MAGNET]
    magnet = r["episodes"][1]
    assert magnet["strip"] is not None and magnet["strip"]["markers"] == [(480, "AP"), (520, "VP")]
    assert magnet["strip"]["v"][600] == pytest.approx(10.0)
    assert [t["day"] for t in r["trends"]] == [92, 185, 277]
    # The OPEN request ends whatever frame a stale session was collecting.
    assert fake.writes[0].startswith(b"\x00")

    text = pp.summary_text(r)
    assert "Diagnostics cleared." in text and "Magnet" in text
    page = pp.html_report(r)
    assert page.startswith("<!doctype html>") and "id='egm0'" in page


def test_no_write_overflows_the_request_file(monkeypatch):
    fake = make_fake()
    pp.interrogate(pp.Session(fake, timeout_s=1), clear=True, now=1_800_000_000)
    # OPEN, then every other request in one batch: the file holds them all.
    assert len(fake.writes) == 2
    assert all(len(w) <= pp.MAX_WRITE for w in fake.writes)
    # A smaller file splits the same requests into more batches.
    monkeypatch.setattr(pp, "MAX_WRITE", 32)
    fake = make_fake()
    pp.interrogate(pp.Session(fake, timeout_s=1), clear=True, now=1_800_000_000)
    assert len(fake.writes) == 3
    assert all(len(w) <= 32 for w in fake.writes)


def test_a_response_is_matched_by_seq_not_by_arrival():
    # COUNTERS answered first, then a stale COUNTERS from another seq: only
    # the one with this request's seq counts.
    class Reordered(FakeDevice):
        def send(self, data):
            super().send(data)
            self.out = bytearray(pp.encode_frame(0x82, 99, b"\xff" * 128)) + self.out

    fake = Reordered(make_fake().info, make_fake().store)
    session = pp.Session(fake, timeout_s=1)
    session.seq = 0
    [counters] = session.transact([(pp.REQ_COUNTERS, b"")])
    assert counters == make_fake().store["counters"]


def test_error_response_is_raised():
    fake = make_fake()
    session = pp.Session(fake, timeout_s=1)
    with pytest.raises(RuntimeError, match="unknown request"):
        session.transact([(0x42, b"")])


def test_a_silent_device_times_out():
    class Silent(FakeDevice):
        def send(self, data):
            self.writes.append(bytes(data))

    session = pp.Session(Silent({}, {}), timeout_s=0.05)
    with pytest.raises(TimeoutError, match="0x02"):
        session.transact([(pp.REQ_COUNTERS, b"")])


def test_parse_stream_frame():
    f = pp.parse_stream(stream_payload(5000, [(625, -62), (0, 1)], [(5001, 5), (5001, 2)], mode=5, flags=0b011),
                        uv_per_lsb=16)
    assert f["first"] == 5000
    assert f["v"] == [10.0, 0.0] and f["a"] == [-0.992, 0.016]
    assert f["markers"] == [(5001, "VP"), (5001, "AP")]
    assert (f["mode"], f["magnet"], f["batt"]) == ("DDDR", True, "ERI")


class Recorder:
    def __init__(self, stop_after):
        self.events, self.stop_after = [], stop_after
        self.stop = __import__("threading").Event()

    def send(self, event, data):
        self.events.append((event, data))
        if sum(e == "egm" for e, _ in self.events) >= self.stop_after:
            self.stop.set()


def test_live_streams_frames_then_ends_the_session():
    fake = make_fake()
    fake.info["proto"] = 2
    rec = Recorder(stop_after=4)
    gaps = pp.live(pp.Session(fake, timeout_s=1), rec, now=1_800_000_000, stop=rec.stop)

    hello = [d for e, d in rec.events if e == "hello"]
    frames = [d for e, d in rec.events if e == "egm"]
    assert hello and hello[0]["sps"] == 250 and hello[0]["mode"] == "DDD"
    assert len(frames) >= 4 and gaps == 0
    assert all(b["first"] == a["first"] + len(a["v"]) for a, b in zip(frames, frames[1:]))
    assert frames[0]["v"][0] == pytest.approx(1.6) and frames[0]["markers"][0][1] == "VS"
    # The close turns the stream off and ends the session, in one batch.
    assert [k for k, _, _ in pp.parse_frames(fake.writes[-1])] == [pp.REQ_STREAM, pp.REQ_END]
    assert not fake.streaming


def test_live_refuses_firmware_without_stream():
    fake = make_fake()  # protocol 1
    with pytest.raises(RuntimeError, match="protocol 1"):
        pp.live(pp.Session(fake, timeout_s=1), Recorder(1), now=1_800_000_000)


def test_live_close_gives_up_quickly_on_a_silent_device():
    class GoesQuiet(FakeDevice):
        def send(self, data):
            if pp.REQ_END in [k for k, _, _ in pp.parse_frames(bytes(data))]:
                self.writes.append(bytes(data))
                self.streaming = False
                return  # the close is never answered
            super().send(data)

    fake = GoesQuiet(make_fake().info, make_fake().store)
    fake.info["proto"] = 2
    rec = Recorder(stop_after=2)
    pp.STREAM_CLOSE_S, saved = 0.05, pp.STREAM_CLOSE_S
    try:
        pp.live(pp.Session(fake, timeout_s=1), rec, now=1_800_000_000, stop=rec.stop)
    finally:
        pp.STREAM_CLOSE_S = saved
    assert [k for k, _, _ in pp.parse_frames(fake.writes[-1])] == [pp.REQ_STREAM, pp.REQ_END]


class FakeMachine:
    """The emerson library's Machine, as far as WandLink uses it."""

    def __init__(self):
        self.calls = []

    def get_device(self, path):
        machine = self

        class Device:
            def invoke_action(self, action, args=""):
                machine.calls.append((path, action))
                return "ok"

        return Device()

    def subscribe_broker(self, broker):
        self.calls.append(("subscribe", broker))
        machine = self

        class Subscription:
            def read(self, timeout):
                return b""

            def close(self):
                machine.calls.append(("unsubscribe", broker))

        return Subscription()

    def send_to_broker(self, broker, data):
        self.calls.append(("send", broker, data))
        return True


def test_the_wand_is_placed_after_subscribing_and_lifted_on_close():
    m = FakeMachine()
    link = pp.WandLink(m)
    link.send(b"\x00abc")
    link.close()
    assert m.calls == [
        ("subscribe", "wand"),
        ("/patient/wand", "place_wand"),
        ("send", "wand", b"\x00abc"),
        ("/patient/wand", "remove_wand"),
        ("unsubscribe", "wand"),
    ]


def test_a_lifted_wand_ends_a_waiting_request():
    class Lifted(FakeDevice):
        def send(self, data):
            self.writes.append(bytes(data))  # never answered

        def placed(self):
            return False

    session = pp.Session(Lifted({}, {}), timeout_s=5)
    t0 = time.monotonic()
    with pytest.raises(pp.WandLifted):
        session.transact([(pp.REQ_COUNTERS, b"")])
    assert time.monotonic() - t0 < 1.0, "not after the response timeout"


def test_live_stops_without_an_end_when_the_wand_is_lifted(monkeypatch):
    class LiftedMidStream(FakeDevice):
        def placed(self):
            return not self.streaming or self.stream_seq < 3

    fake = LiftedMidStream(make_fake().info, make_fake().store)
    fake.info["proto"] = 2
    rec = Recorder(stop_after=1000)
    monkeypatch.setattr(pp, "WAND_CHECK_S", 0.0)
    pp.live(pp.Session(fake, timeout_s=1), rec, now=1_800_000_000, stop=rec.stop)
    assert not rec.stop.is_set(), "it stopped for the wand, not the recorder"
    sent = [k for w in fake.writes for k, _, _ in pp.parse_frames(w)]
    assert pp.REQ_END not in sent, "the device ended the session itself"


# --- Wandless: the bedside communicator's radio link ---------------------------

class FakeCommunicatorMachine(FakeMachine):
    """FakeMachine whose communicator reports a phase, as `get_status` words it
    ("communicator <phase>; <note>; implant ..."). `phases` is what successive
    `get_status` calls say; the last one repeats."""

    def __init__(self, phases):
        super().__init__()
        self.phases = list(phases)

    def get_device(self, path):
        machine = self

        class Device:
            def invoke_action(self, action, args=""):
                machine.calls.append((path, action, args) if args else (path, action))
                if action == "get_status":
                    phase = machine.phases.pop(0) if len(machine.phases) > 1 else machine.phases[0]
                    return f"communicator {phase}; session open (base-initiated); implant awake, linked; listening on"
                return "ok"

        return Device()


def test_the_communicator_wakes_the_implant_after_subscribing_and_stops_on_close(monkeypatch):
    monkeypatch.setattr(pp.CommunicatorLink, "POLL_S", 0.0)
    m = FakeCommunicatorMachine(["idle", "waking", "linked"])
    link = pp.CommunicatorLink(m)
    link.send(b"\x00abc")
    link.close()
    assert m.calls == [
        ("subscribe", "communicator"),
        ("/patient/communicator", "get_status"),     # idle: nothing open yet
        ("/patient/communicator", "start_session"),  # the 2.45 GHz wake-up
        ("/patient/communicator", "get_status"),     # waking
        ("/patient/communicator", "get_status"),     # linked
        ("send", "communicator", b"\x00abc"),
        ("/patient/communicator", "end_session"),
        ("unsubscribe", "communicator"),
    ]


def test_an_implant_initiated_session_already_open_is_taken_as_it_is():
    m = FakeCommunicatorMachine(["linked"])
    link = pp.CommunicatorLink(m)
    assert ("/patient/communicator", "start_session") not in m.calls
    assert link.placed()
    link.close()


def test_a_wakeup_the_implant_does_not_answer_is_an_error_and_cleans_up(monkeypatch):
    monkeypatch.setattr(pp.CommunicatorLink, "POLL_S", 0.0)
    m = FakeCommunicatorMachine(["idle"])
    with pytest.raises(RuntimeError, match="did not answer the wake-up"):
        pp.CommunicatorLink(m)
    assert ("unsubscribe", "communicator") in m.calls, "the subscription is not leaked"
    assert ("/patient/communicator", "end_session") in m.calls


def test_a_wakeup_can_carry_its_own_length(monkeypatch):
    monkeypatch.setattr(pp.CommunicatorLink, "POLL_S", 0.0)
    m = FakeCommunicatorMachine(["idle", "linked"])
    pp.CommunicatorLink(m, wakeup_s=2.5).close()
    assert ("/patient/communicator", "start_session", "2.5") in m.calls


def test_a_closed_radio_link_ends_a_waiting_request():
    class Closed(FakeDevice):
        def send(self, data):
            self.writes.append(bytes(data))  # never answered

        def placed(self):
            return False

        def lost_error(self):
            return pp.LinkLost("the radio link closed")

    session = pp.Session(Closed({}, {}), timeout_s=5)
    t0 = time.monotonic()
    with pytest.raises(pp.LinkLost):
        session.transact([(pp.REQ_COUNTERS, b"")])
    assert time.monotonic() - t0 < 1.0, "not after the response timeout"
    assert issubclass(pp.LinkLost, pp.WandLifted), "everything that stops for a wand stops for this"


def test_the_link_option_picks_the_communicator(monkeypatch):
    import contextlib
    import emerson

    seen = {}

    @contextlib.contextmanager
    def attach_session(host=None, project=None, session=None):
        yield FakeCommunicatorMachine(["linked"])

    def fake_interrogate(session, clear=False, now=None, upgrade=None):
        seen["link"] = type(session.link).__name__
        raise RuntimeError("stop here")

    monkeypatch.setattr(emerson, "attach_session", attach_session, raising=False)
    monkeypatch.setattr(pp, "interrogate", fake_interrogate)
    assert pp.main(["--link", "communicator"]) == 1
    assert seen["link"] == "CommunicatorLink"


def test_the_radio_off_episode_is_named():
    assert pp.EPISODE_NAMES[pp.EP_RADIO_OFF] == "Radio telemetry disabled"
    detail = pp.episode_detail({"kind": pp.EP_RADIO_OFF, "a": 2514, "b": 0, "c": 0})
    assert "2.514 V" in detail and "off at ERI" in detail


def test_a_radio_session_that_ends_with_end_still_delivers_the_last_answers():
    """The device closes a radio session once END's answer has gone out, so by the time the
    host looks the link may already be down with END's answer waiting in the broker."""

    class EndsWithEnd(FakeDevice):
        ended = False

        def send(self, data):
            super().send(data)
            self.ended = self.ended or any(k == pp.REQ_END for k, _, _ in pp.parse_frames(bytes(data)))

        def placed(self):
            return not self.ended

        def lost_error(self):
            return pp.LinkLost("the radio link closed")

    fake = EndsWithEnd(make_fake().info, make_fake().store)
    session = pp.Session(fake, timeout_s=5)
    out = session.transact([(pp.REQ_COUNTERS, b""), (pp.REQ_END, b"")])
    assert [len(p) for p in out] == [128, 0], "both answered although the link is already down"


def test_awaiting_the_implants_own_wakeup_sends_no_wakeup(monkeypatch):
    monkeypatch.setattr(pp.CommunicatorLink, "POLL_S", 0.0)
    # idle (no wake-up of ours to fail), waking is not a thing here, then the implant calls in.
    m = FakeCommunicatorMachine(["idle", "idle", "linked"])
    link = pp.CommunicatorLink(m, await_implant_s=5)
    assert not any(c[1:2] == ("start_session",) for c in m.calls), m.calls
    assert link.placed()
    link.close()


def test_awaiting_the_implants_wakeup_gives_up_after_the_wait(monkeypatch):
    monkeypatch.setattr(pp.CommunicatorLink, "POLL_S", 0.0)
    m = FakeCommunicatorMachine(["idle"])
    with pytest.raises(RuntimeError, match="no radio link within 0.05 s"):
        pp.CommunicatorLink(m, await_implant_s=0.05)
    assert ("unsubscribe", "communicator") in m.calls


# --- Safety Mode (protocol 4) -------------------------------------------------

def safety_block(latched, resets, scale=3600, total=None):
    """The firmware's `struct safety_block`, as the device answers REQ_SAFETY."""
    total = len(resets) if total is None else total
    head = struct.pack(pp.SAFETY_HEADER_FORMAT, pp.SAFETY_LATCHED if latched else 0,
                       resets[-1][0] if latched else 0, total, scale, 48 * 3600, 3, 0, 0)
    recs = [(0, 0)] * pp.RESET_SLOTS
    for n, (when, cause) in enumerate(resets):
        recs[n % pp.RESET_SLOTS] = (when, cause)
    return head + b"".join(struct.pack(pp.RESET_REC_FORMAT, w, c) for w, c in recs)


def interrogate_with_safety(block, proto=4):
    fake = make_fake()
    fake.info["proto"] = proto
    if block is not None:
        fake.store["safety"] = block
    return pp.interrogate(pp.Session(fake, timeout_s=1), now=1_800_000_000)


def test_the_safety_block_layout_matches_the_firmware():
    assert struct.calcsize(pp.SAFETY_HEADER_FORMAT) == 32
    assert struct.calcsize(pp.RESET_REC_FORMAT) == 8
    assert 32 + 8 * pp.RESET_SLOTS == pp.SAFETY_FORMAT_SIZE == 160


def test_a_latched_device_shows_the_red_alert_and_the_reset_log():
    base = 300_000_000
    resets = [(base + 3600 * h, pp.RESET_SUPERVISOR) for h in (1, 5, 9)]
    r = interrogate_with_safety(safety_block(True, resets))
    sf = r["safety"]
    assert sf["latched"] and sf["total"] == 3 and sf["needed"] == 3 and sf["window_s"] == 172800
    assert [x["implant_s"] for x in sf["resets"]] == [x[0] for x in resets]
    assert sf["latched_s"] == resets[-1][0] and sf["scale"] == 3600

    plain = pp.summary_text(r, color=False)
    assert plain.splitlines()[0] == "*** SAFETY MODE -- replace device ***"
    assert "lower rate 72.5 ppm" in plain
    assert plain.count("supervisor reset (RUN low)") == 3
    assert "3600 implant s per guest s" in plain
    red = pp.summary_text(r, color=True)
    assert red.splitlines()[0].startswith("\x1b[1;31m") and "replace device" in red

    page = pp.html_report(r)
    assert "class='alert'>SAFETY MODE -- replace device" in page
    assert page.count("supervisor reset (RUN low)") == 3


def test_an_unlatched_device_lists_its_resets_without_the_alert():
    r = interrogate_with_safety(safety_block(False, [(300_000_000, pp.RESET_SUPERVISOR)]))
    assert not r["safety"]["latched"]
    text = pp.summary_text(r, color=False)
    assert "replace device" not in text.split("Episodes")[0].replace("SAFETY MODE latched", "")
    assert "Uncommanded resets (1 since implant" in text
    assert "class='alert'" not in pp.html_report(r)


def test_older_firmware_is_read_without_the_safety_block():
    r = interrogate_with_safety(None, proto=3)
    assert r["safety"] is None
    assert "SAFETY" not in pp.summary_text(r, color=False)


def test_the_reset_ring_is_read_oldest_first_across_a_wrap():
    resets = [(300_000_000 + 100 * n, pp.RESET_SUPERVISOR) for n in range(20)]
    sf = interrogate_with_safety(safety_block(True, resets, total=20))["safety"]
    assert len(sf["resets"]) == 16
    assert [x["num"] for x in sf["resets"]] == list(range(4, 20))
    assert sf["resets"][0]["implant_s"] == 300_000_400


def test_power_on_episodes_name_their_cause_and_the_latch_is_named():
    e = dict(kind=pp.EP_POWER_ON, a=0, b=pp.RESET_SUPERVISOR, c=1)
    assert "supervisor reset (RUN low)" in pp.episode_detail(e) and "SAFETY MODE" in pp.episode_detail(e)
    assert "commanded reboot" in pp.episode_detail(dict(kind=pp.EP_POWER_ON, a=0, b=1, c=0))
    assert pp.EPISODE_NAMES[pp.EP_SAFETY] == "SAFETY MODE latched"
    assert "3 uncommanded" in pp.episode_detail(dict(kind=pp.EP_SAFETY, a=3, b=0, c=0))


def test_noise_reversion_and_pause_episodes_are_reported():
    """Stage 5b: the counters' two new fields (formerly reserved, same layout, still protocol 4) and
    the two new episode kinds the device logs about the harm it saw itself."""
    fake = make_fake()
    fake.store["counters"] = struct.pack("<32I", *([0] * 16), 3, 7, *([0] * 14))
    fake.store["episodes"] = (
        struct.pack(pp.EPISODE_FORMAT, 400_000_000, 5200, pp.EP_PAUSE, 1, 0, 0, 0)
        + struct.pack(pp.EPISODE_FORMAT, 400_000_100, 9400, pp.EP_NOISE, pp.EGM_NONE, 12, 5, 0))
    r = pp.interrogate(pp.Session(fake, timeout_s=1), clear=False, now=1_800_000_000)
    assert (r["counters"]["noise"], r["counters"]["pauses"]) == (3, 7)
    assert [e["kind"] for e in r["episodes"]] == [pp.EP_PAUSE, pp.EP_NOISE]
    text = pp.summary_text(r, color=False)
    assert "noise reversions 3, ventricular pauses 7" in text
    assert "Ventricular pause" in text and "5.2 s" in text and "no ventricular pace or sense past the pause-detection time" in text
    assert "Noise reversion (VOO)" in text and "12 noise senses, 5 asynchronous ventricular paces" in text
    page = pp.html_report(r)
    assert "Pauses" in page and "Ventricular pause" in page


# --- Stage 6: firmware levels, the daily battery test, Code-1003, the explant indicator -----------------------

def fw_block(level=pp.FW_V204, flags=0, tests=(), alert_s=0, disabled_s=0, explant_cal=0, why=0, upgraded_s=299_000_000,
             total=None):
    """firmware/main.c `struct fw_block`: 16 words, then 16 test records. `tests` are
    (implant_s, v_pre, v_burst, v_exp, l_x100, rflags, verdict)."""
    total = len(tests) if total is None else total
    head = struct.pack(pp.FW_HEADER_FORMAT, level, upgraded_s, flags, alert_s, disabled_s, explant_cal, total,
                       0, 0, 0, why, 0, 0, 0, 0, 0)
    recs = [struct.pack(pp.BT_REC_FORMAT, *t, 0) for t in tests]
    recs += [bytes(16)] * (pp.BT_RECS - len(recs))
    return head + b"".join(recs)


def interrogate_with_fw(block, proto=5, batt_state=0, upgrade=None):
    fake = make_fake()
    fake.info["proto"] = proto
    fake.info["batt_state"] = batt_state
    if proto >= pp.MIN_SAFETY_PROTOCOL:
        fake.store["safety"] = safety_block(False, [])
    if block is not None:
        fake.store["fw"] = block
    return pp.interrogate(pp.Session(fake, timeout_s=1), now=1_800_000_000, upgrade=upgrade), fake


def test_the_firmware_block_layout_matches_the_firmware():
    assert struct.calcsize(pp.FW_HEADER_FORMAT) == 64
    assert struct.calcsize(pp.BT_REC_FORMAT) == 16
    assert pp.FW_SIZE == 64 + 16 * pp.BT_RECS == 320


BASE = 300_000_000


def alert_history():
    # day numbers are 86400 s apart on the implant clock; 3.0 y of margin falling to a disabling reading
    return [(BASE + 86400 * d, 2627, 2482 - 30 * i, 2598, 300 - 10 * i, pp.BT_REC_VALID, v)
            for i, (d, v) in enumerate([(1, 1), (2, 1), (3, 2)])]


def test_code_1003_and_the_telemetry_shutoff_are_red_alerts_with_the_test_history():
    hist = alert_history()
    r, _ = interrogate_with_fw(fw_block(flags=pp.FW_F_CODE1003 | pp.FW_F_TELE_OFF, tests=hist, alert_s=BASE + 86400 * 4,
                                        disabled_s=BASE + 86400 * 90, why=pp.DIS_IMPEDANCE))
    fw = r["fw"]
    assert fw["level"] == pp.FW_V204 and fw["code1003"] and fw["tele_off"] and not fw["explant"]
    assert [t["v_burst_mv"] for t in fw["history"]] == [2482, 2452, 2422]
    assert [t["v_exp_mv"] - t["v_burst_mv"] for t in fw["history"]] == [116, 146, 176]  # margins in mV
    text = pp.summary_text(r, color=False)
    head = text.splitlines()
    assert head[0].startswith("*** Code-1003: Voltage too low for projected remaining longevity (raised ")
    assert head[1].startswith("*** WANDLESS TELEMETRY DISABLED ") and "high battery impedance" in head[1]
    assert "Firmware level: v2.04 (battery test; with the two shipped bugs)" in text
    assert "2.482" in text and "2.598" in text and "116 mV" in text and "HIGH" in text and "ELEVATED" in text
    red = pp.summary_text(r, color=True)
    assert red.splitlines()[0].startswith("\x1b[1;31m")
    page = pp.html_report(r)
    assert page.count("class='alert'") == 2
    assert "Wandless telemetry: <span class='bad'>DISABLED</span>" in page and "Code-1003: <span class='bad'>raised" in page
    assert "<td>2.482 V</td>" in page and "<td>116 mV</td>" in page


def test_a_false_positive_shows_the_explant_indicator_dated_2000_01_01():
    """The letter's example: "explant indicator was reached January 1, 2000". The date field is the device
    calendar's zero because only the ERI path writes it."""
    hist = [(BASE + 86400, 2656, 0, 2598, 300, pp.BT_REC_MAGNET, 4)]
    r, _ = interrogate_with_fw(fw_block(flags=pp.FW_F_TELE_OFF | pp.FW_F_EXPLANT, tests=hist, why=pp.DIS_BAD_SAMPLE,
                                        disabled_s=BASE + 86400), batt_state=0)
    fw = r["fw"]
    assert fw["explant"] and fw["explant_cal"] == 0 and pp.explant_date(fw) == "2000-01-01"
    text = pp.summary_text(r, color=False)
    assert "EXPLANT INDICATOR reached 2000-01-01 -- erroneous: the battery has not reached ERI" in text
    assert "burst reading that was never taken" in text
    assert "NO SAMPLE [magnet]" in text and "---" in text
    page = pp.html_report(r)
    assert "reached 2000-01-01 (erroneous: the battery has not reached ERI)" in page and "no sample" in page


def test_a_real_explant_indicator_carries_its_date_and_is_not_called_erroneous():
    cal = int((pp.dt.datetime(2031, 5, 17, tzinfo=pp.dt.timezone.utc) - pp.CAL_EPOCH).total_seconds())
    r, _ = interrogate_with_fw(fw_block(level=pp.FW_BASE, flags=pp.FW_F_EXPLANT, explant_cal=cal), batt_state=1)
    text = pp.summary_text(r, color=False)
    assert "EXPLANT INDICATOR reached 2031-05-17" in text and "erroneous" not in text
    assert "Firmware level: base (as shipped)" in text and "Daily battery test" not in text


def test_the_fixed_level_reports_the_cleared_false_positive():
    r, _ = interrogate_with_fw(fw_block(level=pp.FW_FIXED, flags=pp.FW_F_CLEARED))
    text = pp.summary_text(r, color=False)
    assert "Firmware level: fixed (the forthcoming software)" in text
    assert "re-enabled after a false positive" in text
    assert text.splitlines()[0].startswith("Device:")  # no alert: nothing is disabled


def test_older_firmware_is_read_without_the_firmware_block():
    r, fake = interrogate_with_fw(None, proto=4)
    assert r["fw"] is None
    assert "Firmware level" not in pp.summary_text(r, color=False)
    assert all(pp.REQ_FWINFO not in [k for k, _, _ in pp.parse_frames(w)] for w in fake.writes)
    with pytest.raises(RuntimeError, match="no battery test to upgrade to"):
        interrogate_with_fw(None, proto=4, upgrade="v2.04")


def test_an_upgrade_is_the_first_request_and_the_report_is_of_the_upgraded_device():
    r, fake = interrogate_with_fw(fw_block(level=pp.FW_BASE), upgrade="v2.04")
    kinds = [k for w in fake.writes for k, _, _ in pp.parse_frames(w)]
    assert kinds[:2] == [pp.REQ_OPEN, pp.REQ_UPGRADE]
    assert r["fw"]["level"] == pp.FW_V204
    r, _ = interrogate_with_fw(fw_block(level=pp.FW_V204), upgrade="fixed")
    assert r["fw"]["level"] == pp.FW_FIXED


def test_a_refused_upgrade_says_so():
    with pytest.raises(RuntimeError, match="refused the upgrade"):
        interrogate_with_fw(fw_block(level=pp.FW_FIXED), upgrade="v2.04")  # no downgrade


def test_the_battery_test_episodes_are_named():
    e1 = dict(kind=pp.EP_CODE1003, a=2482, b=2598, c=300)
    assert pp.EPISODE_NAMES[pp.EP_CODE1003] == "Code-1003 alert"
    assert "burst 2.482 V vs 2.598 V expected, projected 3.00 years" in pp.episode_detail(e1)
    e2 = dict(kind=pp.EP_TELE_OFF, a=0, b=2598, c=pp.DIS_BAD_SAMPLE)
    assert "never taken" in pp.episode_detail(e2) and "no burst reading" in pp.episode_detail(e2)
    assert "v2.04 -> fixed; wandless telemetry re-enabled" in pp.episode_detail(
        dict(kind=pp.EP_FW_LEVEL, a=pp.FW_V204, b=pp.FW_FIXED, c=1))


if __name__ == "__main__":
    # No cache: `emerson run` mounts the repo, and .pytest_cache would land in it.
    sys.exit(pytest.main([__file__, "-p", "no:cacheprovider", *sys.argv[1:]]))
