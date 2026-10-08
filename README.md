# rp2350-pico2-pacemaker-demo

A dual-chamber cardiac pacemaker on a Raspberry Pi Pico 2 (RP2350, Hazard3
RISC-V core). It runs closed-loop against a simulated patient. Every
peripheral is a real datasheet part: there are no invented status
registers, and every fault is something a real part, lead, cell or heart can
do.

The project also reproduces the Boston Scientific ACCOLADE Safety Mode
recall (BSC physician letter, Sept 2025, Appendix A) end to end. A
high-impedance cell defect, wandless-telemetry bursts, supervisor resets
and the irreversible Safety Mode latch each come out of the emulated
hardware and firmware. The v2.04 software mitigation and its two shipped
bugs are modelled as well.

This file is the reference for working on the project. The project was
brought over from the emerson source tree, where it ran on a locally built
franklin-server. Here it runs on the packaged `emerson` tool and its
`emerson-server` Docker image. [Moving from the emerson source
tree](#moving-from-the-emerson-source-tree) lists what changed.

## Layout

| Path | What it is |
| --- | --- |
| `firmware/` | Bare-metal RV32IMAC firmware: `main.c`, `start.S` (with the boot ROM's IMAGE_DEF block), `rp2350.ld`, `Makefile` |
| `build.sh`, `Dockerfile.build` | Builds `firmware/firmware.bin` in a Debian container (clang 14, lld, llvm, make), so the host needs no toolchain |
| `.emerson/peripherals.yaml` | Board wiring: every off-chip part and its pins. `emerson load` mounts it over the image's copy |
| `emerson-py.sh` | Runs the Python host tools inside the `emerson-server` container, where the `emerson` module lives |
| `programmer/` | Host-side clinic programmer (`pacemaker_programmer.py`), its live-view page and its pytest suite |
| `scenarios/` | The ACCOLADE reproductions: `accolade_safety_mode.py`, `accolade_v204.py` |
| `.claude/skills/` | The `emerson` and `emerson-python` Claude Code skills (`emerson skills pull`) |

The emulator project itself (`emerson.yaml`, the boot ROM and the device
models) ships inside the Docker image as the project `rp2350`, at
`/opt/tuliptree/emerson/projects/rp2350/`. It uses the `rp2350` preset with
the real boot ROM, `boot_arch: riscv`, the block executor, the deferred
scheduler and `realtime: true`. The design notes (`notes/stage-*.md`) and
`emerson-description.md` were not brought over.

## Running it

Requires Docker and the `emerson` tool (1.0.14 or later, with a license key
stored) and the `ghcr.io/tuliptreetech/emerson/rp2350` image installed. Last
verified with emerson 1.0.15 and image 1.0.16; `emerson info` shows both.

```
./build.sh                          # firmware/firmware.bin; ./build.sh clean, ./build.sh --rebuild (toolchain image)
emerson load firmware/firmware.bin  # bring up emerson-server on it (project rp2350)
emerson start                       # a session, paused at reset
emerson ctl go
emerson ctl broker tty1             # marker channel, alarms, status
emerson ctl broker tty0 $'help\r'   # the programmer console
```

After editing the firmware, run `./build.sh && emerson start`.
`emerson start` sees that the firmware changed and replaces the session. If
the rebuild replaced the file (objcopy does), it also recreates the container.
To begin again from power-on without rebuilding, run `emerson stop &&
emerson start`.

The firmware runs off the 12 MHz crystal with no PLL. That suits a battery
device, and it keeps guest time near wall time. The emulator's speed depends
on the host, so measure it with
`emerson ctl action /board/battery get_rail` (`at guest <s>`) across a wall
interval:

- Natively on x86-64 it ran at about 0.5× wall.
- On an Apple Silicon Mac it runs at about 0.23× wall, because the image is
  linux/amd64 and Docker emulates it.

## Board wiring

| Signal | Part | RP2350 pin | Device tree |
| --- | --- | --- | --- |
| SPI0 SCK / MOSI / MISO | shared by the next two parts | GP18 / GP19 / GP16 | `/MEM/spi0` |
| Sensing front end, CS / DRDY (active low) | TI ADS1292R | GP17 / GP20 | `/MEM/spi0/ads1292r` |
| Activity sensor, CS / INT1 (FIFO watermark) | Analog Devices ADXL362 | GP22 / GP27 | `/MEM/spi0/adxl362` |
| SPI1 SCK / MOSI / MISO | shared by the next three parts | GP10 / GP11 / GP12 | `/MEM/spi1` |
| Pacing amplitude DAC, CS (LDAC tied low) | Microchip MCP4822, `output_gain: 1.25` | GP13 | `/MEM/spi1/mcp4822` |
| NFC telemetry tag, CS / INTO (active low) | TI RF430CL330H | GP2 / GP3 | `/MEM/spi1/rf430cl330h` |
| Implant radio, CS / IRQ | Microsemi ZL70102 | GP5 / GP6 | `/MEM/spi1/radio` |
| Radio WU_EN | (wire) | GP7 | `/board/radio_wu_en` |
| Radio VSUP load switch ON (board pull-down) | TI TPS22860 | GP25 | `/board/radio_vsup` |
| Ventricular pacing switch IN | TI TS5A3166 | GP14 | `/board/vpace` |
| Atrial pacing switch IN | TI TS5A3166 | GP15 | `/board/apace` |
| RV lead polarity IN (low = ring/bipolar, high = can/unipolar) | TI TS5A3159 | GP28 | `/board/polarity` |
| Magnet sensor OUT (low = magnet) | TI DRV5032FA | GP21 | `/board/magnet_sensor` |
| Battery VBAT (no divider) | Li/CFx primary cell, `aging_factor: 1e6` | GP26 (ADC0) | `/board/battery`, read through `/MEM/adc/battery` |
| Diagnostic EEPROM, SDA / SCL, address 0x50 | Microchip AT24C256C | GP8 / GP9 | `/MEM/i2c0/at24c256` |
| Supply supervisor RESET (2.325 V, td 200 ms) | TI TPS3839G25 | RUN | `/board/supervisor` (`/MEM/run`) |
| Programmer console (echoed input) | UART0 | GP0 / GP1 | `tty0` |
| Telemetry: markers, alarms, status (output only) | UART1 | GP4 | `tty1` |

Analog wiring that the model fixes:

- ADS1292R CH1 is the ventricular lead and CH2 the atrial lead. Both are
  lead-off monitored in every mode.
- MCP4822 VOUTA charges the ventricular output capacitor and VOUTB the
  atrial one, through a 1.25x amplifier: capacitor volts = 1.25 mV × code,
  so code 4000 is 5.0 V. The firmware converts every programmed mV with
  `dac_code_for_mv`. Each TS5A3166 dumps its capacitor into the lead for as
  long as IN is high, so the pulse width is however long the firmware holds
  IN.
- The TS5A3159 switches the ventricular lead's return for both sensing and
  pacing. Firmware leaves GP28 low except in Safety Mode.
- The pacing output rail is a charge-pump doubler (2 × cell volts), so a
  collapsed cell cannot reach the programmed amplitude.
- The RP2350's VREG needs VREG_VIN ≥ 2.7 V, which is above the Li/CFx
  plateau. The board is described as feeding DVDD externally and running
  IOVDD straight off the cell. Neither regulator is modelled.
- SPI1 carries three parts with different formats. The firmware reformats
  the bus per transaction, and the RF430CL330H runs at ≤ 100 kHz in mode 0.

## Device tree

```
/Hazard3Cpu
/MEM/...                       SoC blocks; bus parts under their controller
/board                         off-chip parts wired to pads
  vpace, apace, polarity, magnet_sensor, battery, supervisor,
  radio_vsup, radio_wu_en
/patient                       the simulated world the board senses
  heart                        rhythm, conduction, capture, leads, PVCs, myopotentials
  body                         activity, posture, peak METs, arm effort
  magnet                       the clinician's magnet
  wand                         the programmer's wand (magnet + NFC reader), broker `wand`
  communicator                 the bedside radio base station, broker `communicator`
```

`/patient/heart` and `/patient/body` are two nodes over one shared heart
(`cardiac_physics` + `body_physics`). Every sensing and pacing part reads and
drives that same state. The heart advances lazily, to the current guest time,
whenever a part samples or paces it. Guest time comes from ticks and the
live clk_sys, never a hard-coded clock.

## The simulated patient

- **Heart** (`cardiac_physics`): an SA node (rate 0 = sinus arrest); AV
  conduction with a PR interval and first, second (Mobitz II, drop 1 in N) or
  third degree block; a ventricular escape rhythm; refractory periods; PVCs;
  and capture by the Lapicque strength-duration curve
  `V_th(pw) = V_rheobase × (1 + chronaxie / pw)`. It synthesizes an
  electrogram per chamber, with intrinsic and evoked morphologies, T waves,
  pacing artifacts and afterpotentials, 5 % crosstalk and far-field signals.
  Each lead has an impedance (500 Ω), a polarity (bipolar or unipolar) and a
  state (intact or fractured). The heart counts pocket stimulations and
  measures ventricular gaps as ground truth: `get_status`, `reset_gap_stats`.
- **Body** (`body_physics`): activity in METs (`rest`, `walk`, `brisk`,
  `jog`, `cycle`, `car`), posture, and the metabolic-chronotropic
  relationship. **Chronotropic competence** scales the patient's sinus
  response (1.0 is healthy, < 0.8 is incompetent). The accelerometer feels
  gravity plus a synthesized gait or road vibration. `set_arm_effort` drives
  pectoral myopotentials, which a unipolar lead picks up about 20 times more
  strongly than a bipolar one.
- **Cell** (`implant_cell`, `chemistry: li_cfx`): a 1.6 Ah Li/CFx cell. Its
  OCV is a flat 2.6–2.72 V plateau that falls off at depletion, behind
  internal resistance that rises from ~10 Ω to ~1 kΩ. A 47 µF capacitor sits
  on the rail. Each pace is charged `V² · pw / R_lead` to the cell. Parts that
  draw from the rail (`set_draw`, the radio, the load switch, the supervisor)
  sag it instantaneously. The defect (`set_defect <severity> [onset_years]`)
  models excess lithium salt in the cathode: from about 4 years of longevity
  left it adds `severity × 2000 Ω × x³`. That is invisible at quiescent load
  and decisive under a radio burst. Aging runs 10⁶× fast by default, so one
  guest second is 11.6 implant days and a fresh cell reaches ERI about seven
  guest minutes in.

## Firmware

Bare-metal C with raw MMIO: no pico-sdk, integer only, clang/lld. The
firmware is version 1.4 and the telemetry protocol is version 5.

### Pacing

Modes: **DDD** (the power-on default), **DDDR**, **DOO**, **VVI**,
**VVIR**, **VOO** and **OOO**. Timing is ventricular-based, VA = LRI − PAV.

| Parameter | Default | Console command |
| --- | --- | --- |
| Lower rate | 60 ppm | `rate` |
| Upper tracking rate | 120 ppm | `utr` |
| Paced / sensed AV delay | 220 / 190 ms | `pav` / `sav` |
| PVARP | 250 ms | `pvarp` |
| Pulse width | 400 µs | `pw` |
| V / A amplitude | 2500 / 2500 mV | `amp` / `aamp` |
| V / A sensitivity | 2500 / 1000 µV | `sens` / `asens` |
| VRP | 250 ms | -- |
| Blanking | PVAB 100 ms after any V event; 50 ms after each chamber's own pace; V 30 ms after AP | -- |

Sensing compares a 4 ms first difference of each electrogram against the
chamber's sensitivity. A fast atrium gets pacemaker Wenckebach: VP is held
to the upper-rate interval until a P wave falls in PVARP. A sense within
120 ms of an AR or VR is never treated as a new AS or VS. There is no
ventricular safety pace, no retrograde-conduction handling (PMT) and no
automatic mode switching, because the heart does not model retrograde
conduction.

**Rate response (DDDR, VVIR).** Every 2 s the firmware drains the
ADXL362's FIFO (25 Hz, ±2 g). It high-passes each axis and takes the mean
|x|+|y|+|z| as the activity level in mg. Above `athresh` (50 mg), each
100 mg adds `rrslope` (15 ppm), capped at `msr` (120 ppm). The
sensor-indicated rate (SIR) reaches 90 % of its target in `rrup` (30 s)
going up and `rrdown` (150 s) going down. In DDD, DDDR and OOO the same
sensor feeds a **chronotropic-incompetence diagnostic**: during sustained
exertion, if the rise in the intrinsic atrial rate is under 80 % of the
SIR's rise, tty1 prints `DIAG: chronotropic incompetence suspected`. DDDR
paces faster than the SIR when AV conduction is intact, because VA restarts
at the conducted VS. There is no rate-adaptive AV delay.

**Magnet.** With a magnet over the device (DRV5032 OUT low), the firmware
paces asynchronously: DOO from a dual-chamber mode, VOO from a
single-chamber one. The first three beats run at 100 ppm, and the third is
at 80 % pulse width as the threshold margin test. After that it paces at
85 ppm (65 ppm after ERI) until the magnet is removed. `magnet off` disables
the response. The response is suspended from the moment a wand session
opens until the Hall switch releases.

**Battery.** Once a second the firmware reads ADC0 as the average of 8
conversions. Three readings in a row below 2.550 V latch **ERI**: `ALARM:
ERI`, DDD/DOO/DDDR/VVIR fall back to VVI at 65 ppm, the magnet rate drops to
65 ppm, and the radio is switched off at its load switch. Three readings
below 2.300 V latch **EOL**. Only a reset clears either state.

### Diagnostics

The firmware keeps an image of the store in RAM and checkpoints it to the
AT24C256C one dirty 32-byte chunk at a time. The layout is in `main.c`
under "Diagnostic store". The EEPROM starts with the `PMDS` magic.

- Counters since the last clear: events, AV sequences, PVCs, noise
  reversions, pauses, episode counts.
- Rate histograms, 16 bins of 10 bpm each: atrial and ventricular sensed and
  paced, and the SIR.
- An episode log, a ring of 64 entries: power-on (with the reset cause),
  lead-off, ERI/EOL, magnet, atrial and ventricular high rate (170 bpm),
  the chronotropic diagnostic, mode change, radio off, Safety Mode, noise
  reversion, pause (over 2 s, set with `pause <ms>`), Code-1003, telemetry
  disabled and firmware level changed.
- Trends, a ring of 256 points, one every 8 × 10⁶ implant seconds (92.6
  days): battery, activity, % paced, mean rates.
- Four stored 6 s EGMs (2 s pre-trigger, 250 SPS) with marker annotations.
  They are triggered by lead-off, magnet, high-rate, noise and pause
  episodes. When all four are full, the oldest strip is overwritten.

Dates are implant time, locked to the cell's aging. The console's
`timescale <n>` changes it at run time, and `/board/battery
set_aging_factor <n>` must be set to the same number. Durations and EGMs are
in heart time. There is no RTC, so an uncommanded reset loses up to one
clock checkpoint. `reboot` flushes the store first.

### Telemetry session

A binary session of COBS frames with a CRC-16/CCITT-FALSE. The requests are
OPEN, COUNTERS, HISTOGRAMS, EPISODES, TRENDS, EGM, CLEAR, STREAM (live EGM
and markers), SAFETY, FWINFO, UPGRADE and END. The frame layouts are in
`main.c` under "Telemetry session". There are two links, and both carry the
same session:

- **Wand (NFC).** `/patient/wand place_wand` holds the wand over the
  implant: its magnet closes the Hall switch and its reader talks to the
  RF430CL330H. The firmware moves whole tag files over SPI1 on DMA. DAC
  writes wait for a transaction in progress to finish. A session ends on
  END, when the wand is lifted, or after 30 s of silence.
- **Radio (wandless).** The ZL70102 runs on the MICS band and wakes on
  2.45 GHz. Either the communicator wakes the implant, or the implant wakes
  on its own schedule (`radio wakeup <implant s>`, default daily). A wakeup
  that fails is retried 4 implant hours later. A session ends on END, a lost
  link or 3 s of silence. The radio is the device's highest-power load: it is
  switched off at ERI, and its supply is cut at the load switch whenever the
  chip is in reset. The ZL70102 Design Manual is not public, so the register
  map is the model's own. Names and behaviour follow the datasheet.

### Resets and Safety Mode

At boot the firmware reads the reset cause: WATCHDOG REASON means a
commanded `reboot`, POWMAN `CHIP_RESET.HAD_RUN_LOW` means a supervisor
reset, and neither means a cold power-on. Uncommanded resets go into a
16-entry ring at EEPROM 0x1200. A third one within 48 implant hours
latches **Safety Mode** (letter Table 3), which is permanent: VVI at
72.5 ppm, 5.0 V at 1.0 ms, unipolar (GP28 high), RVRP 250 ms, magnet
response off, and 0.25 mV automatic gain control sensing with noise
reversion to VOO. The AGC and noise reversion follow the conventional
designs in the patents cited in `main.c`, because BSC's are not public.
Every boot after the latch raises a `SAFETY MODE` alarm, and pacing
parameter commands are refused. `reboot` and `--clear` leave the latch in
place. Only `emerson ctl reset`, which blanks the EEPROM, removes it. So does
ending the session (`emerson stop && emerson start`).

### Firmware levels and the daily battery test

One image holds three levels, persisted at EEPROM 0x1300. The programmer's
UPGRADE request moves the level forward only. On a real device the upgrade
is a firmware download; here every level is already in the image.

| Level | Behaviour |
| --- | --- |
| `base` | As shipped: no battery test. |
| `v2.04` | Once per implant day, a 3.4 ms TX-burst battery test samples the ADC during the burst and compares the reading with what a healthy cell gives at the same projected longevity. Four tests ≥ 60 mV low raise **Code-1003**. Two tests ≥ 140 mV low **disable wandless telemetry**. Both shipped bugs are present: automatic wakeups still run (bug 1), and a magnet over the device gives a false positive with an explant date of 2000-01-01 (bug 2). |
| `fixed` | No false positive, and the radio is cut at the load switch when disabled. Upgrading from v2.04 clears an earlier false positive. |

The letter does not describe the mechanism of bug 2, or how bug 1 maps onto
this radio. The firmware, the scenario output and `notes/stage-6.md` label
both as hypotheses.

### Console (tty0) and telemetry (tty1)

Both UARTs run at 115200 8N1. tty0 takes typed commands at the `> ` prompt,
echoes them back, and supports backspace:

```
mode DDD|DDDR|DOO|VVI|VVIR|VOO|OOO   rate  utr  pav  sav  pvarp  pw  amp  aamp  sens  asens
msr  athresh  rrslope  rrup  rrdown  magnet async|off  activity  battery  status
battest [now] / fw   radio [wakeup <implant s>]   pause [<ms>]   timescale [<n>]   reboot  help
```

tty1 prints one marker line per event. The markers are `AP`/`VP` (paced),
`AS`/`VS` (sensed) and `AR`/`VR` (sensed in a refractory period); tracked
ventricular events also carry the AV delay. Alarms (`LEAD_OFF`, `ERI`,
`EOL`, `SAFETY MODE`, ...), `MAGNET`, `NOISE`, `WAND`, `RADIO` and `DIAG`
lines go to tty1 as well. Every 5 s a status line ends with
`act=<mg> sir=<ppm> | batt=<V> OK|ERI|EOL`.

## Host tools

**`programmer/pacemaker_programmer.py`** is the clinic programmer. It uses
the emerson library only.

```
pacemaker_programmer.py                        # interrogate through the wand: summary + pacemaker-interrogation.html
pacemaker_programmer.py --clear                # interrogate, then clear (end of follow-up)
pacemaker_programmer.py --live                 # live EGM + markers at http://localhost:8765/
pacemaker_programmer.py --link communicator    # wandless; --await-implant S waits for the implant's own wakeup
pacemaker_programmer.py --upgrade v2.04|fixed  # move the firmware level, then interrogate
```

Other options: `--host`, `--project` (default `rp2350`), `--session TOKEN`,
`--html PATH`, `--timeout`, `--port` and `--bind`. Run its tests with
`./emerson-py.sh -m pytest -q programmer`. They import the `emerson` module,
so they run in the container. In this README, `pacemaker_programmer.py` and
the scenario scripts are run through `./emerson-py.sh`; see [Running the host
tools here](#running-the-host-tools-here).

**`scenarios/accolade_safety_mode.py`** reproduces the recall. A defective
cell about 2 years from replacement takes automatic radio wakeups at
3600×. Each one resets the device, and the third latches Safety Mode. The
harm phase then steps pectoral effort and measures the patient's
ventricular gaps against what the device logged. `--healthy` runs the same
period without the defect, and `--control` runs the harm table with bipolar
sensing. `--no-harm` stops after the latch checks.

**`scenarios/accolade_v204.py`** compares the firmware levels over implant
months, fast-forwarding at 800 000× and slowing to 3600× around the
wakeups:

- `a`: base until it latches.
- `b`: v2.04 from Code-1003 through the telemetry shut-off to the bug-1
  latch.
- `c`: fixed, which never resets.
- `d`: the magnet false positive, and how it plays out with `magnet off`,
  under fixed, and after upgrading to fixed.

Choose sub-scenarios with `--only a,b,c,d`.

Both scripts start their own session unless you pass `--session`. A session
passed in is reset first, which blanks the EEPROM. Stop your own session
(`emerson stop`) before you run one: the emerson-python skill warns that a
stray session can stall a new one. Their windows are paced in guest time
where that matters, but some waits are still wall seconds tuned on a host
that ran at 0.5× wall.

### Running the host tools here

The `emerson` Python module is only inside the `emerson-server` container,
and the container mounts only `flash.bin` and `peripherals.yaml` from this
repo. `./emerson-py.sh` copies `programmer/` and `scenarios/` into a fresh
temp directory in the container and runs `python3` there with your arguments.
Any `*.html` the run writes (the interrogation report) is copied back into
the current directory. `--host` keeps its default
`http://localhost:10314`, which is the server as seen from inside the
container.

```
emerson load firmware/firmware.bin && emerson start && emerson ctl go
./emerson-py.sh programmer/pacemaker_programmer.py          # -> ./pacemaker-interrogation.html
./emerson-py.sh -m pytest -q -p no:cacheprovider programmer
emerson stop                                                 # the scenarios start their own session
./emerson-py.sh scenarios/accolade_safety_mode.py --no-harm
./emerson-py.sh scenarios/accolade_v204.py --only c
```

Verified on an Apple M3 Max (2026-10-08, emerson 1.0.15, image 1.0.16), with
the two scenarios running in parallel:

| Run | Result | Wall time |
| --- | --- | --- |
| programmer pytest | 51 passed | < 1 s |
| programmer interrogation (wand) | summary + HTML report | ~10 s |
| `accolade_safety_mode.py --no-harm` | PASS: reproduced | 224 s |
| `accolade_safety_mode.py` (with the harm phase) | PASS: reproduced | 848 s |
| `accolade_v204.py` (a–d) | every check PASS | 1320 s |

Ctrl+C or killing `emerson-py.sh` does not stop the Python process in the
container. Stop it with `emerson exec pkill -INT -f pacemaker_programmer.py`
(or the scenario's name). The programmer's `finally` then lifts the wand.

**Live view.** The container publishes only port 10314, so the live page
needs `--bind 0.0.0.0` and a port forward into the container:

```
./emerson-py.sh programmer/pacemaker_programmer.py --live --bind 0.0.0.0 &
docker run -d --rm --name emerson-live-fwd -p 8765:8765 alpine/socat \
    tcp-listen:8765,fork,reuseaddr \
    tcp-connect:$(docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' emerson-server):8765
open http://localhost:8765/
# afterwards:
docker stop emerson-live-fwd; emerson exec pkill -INT -f pacemaker_programmer.py
```

## Quick checks

Run these against a fresh session. The default patient is in 70 bpm sinus
rhythm with a PR of 160 ms, so the device is fully inhibited: `AS` then
`VS` 159–162 ms later, every 855–858 ms.

```
emerson ctl action /patient/heart set_av_block third                # AS then VP at AV=190 ms, 70 bpm
emerson ctl action /patient/heart set_sinus_rate 0                  # + sinus arrest: AP-VP every 1000 ms
emerson ctl action /patient/heart set_av_block none                 # sinus arrest only: AP-VS at 173 ms
emerson ctl action /patient/heart set_av_block second 3             # Mobitz II: VP on the dropped beats only
emerson ctl action /patient/heart set_capture_threshold atrium 1.5  # AP fails to capture; `aamp 3500` on tty0 restores it
emerson ctl action /patient/heart set_lead atrium fractured         # ALARM: LEAD_OFF (atrial lead, LOFF_STAT=0x0c)
emerson ctl action /patient/magnet apply_magnet                     # 3 beats at 600 ms, then 705 ms (85 ppm)
emerson ctl action /patient/body set_activity walk                  # act ~200 mg, SIR -> 83 ppm; DIAG after ~80 s
emerson ctl action /patient/heart set_competence 0.3                # DIAG: chronotropic incompetence suspected
emerson ctl action /board/battery set_depletion 97                  # ALARM: ERI within seconds, then VVI 65
emerson ctl action /board/battery set_defect 1                      # the ACCOLADE high-impedance defect
emerson ctl action /board/battery get_rail                          # instantaneous rail and every named draw
emerson ctl action /MEM/i2c0/at24c256 dump 0                        # the raw diagnostic store
```

In complete block with `set_capture_threshold ventricle 1.2`, the magnet's
80 % threshold-margin pulse fails to capture: `/patient/heart get_status`
counts one non-captured pace, and `/board/vpace get_status` shows a 320 µs
width. Pacing in complete block with sinus arrest (AP+VP every second)
cuts projected longevity from about 13 years to about 8.

## Tests

- Programmer: `./emerson-py.sh -m pytest -q -p no:cacheprovider programmer`
  (51 tests, no session needed).
- The firmware has no unit tests. Verify it live with `emerson ctl` and the
  scenarios, and check magnitudes, not just signs.
- The device models' Rust tests (`rp2350-model`'s `test_pacemaker`,
  `test_supervisor` and `test_radio`, and the part crates such as
  `cardiac_physics`, `implant_cell` and `zl70102`) live in the emerson source
  tree. They are not part of this repo or the image.

## Known limits and traps

- **Model choices that shape the ACCOLADE results:** the defect's shape,
  the Li/CFx ERI/EOL voltages, the 47 µF rail capacitor, the 4 h retry, the
  AGC constants, and the v2.04 thresholds (60/140 mV, 4 and 2 tests). Under
  bug 1 the device latches only 0.2–0.3 implant years after base does, a
  thinner margin than the letter's "much lower observed occurrence".
- **The battery test holds the main loop for ~40 ms** once per implant day,
  inside the ventricular refractory period. Console characters typed during
  it are lost, because the PL011 RX FIFO overruns.
- **The wand's magnet races OPEN.** The DRV5032 samples every 50 ms, and
  when it wins, a follow-up logs a 0 s magnet episode and uses an EGM slot.
  A clinic magnet put on right after the wand is lifted reads as the wand
  still being there. Wait 2 s between the two.
- **`emerson ctl reset` rebuilds the board**, which blanks the EEPROM, Safety
  Mode and the firmware level. After it the firmware may no longer see
  ADS1292R DRDY. Use the console `reboot` to demonstrate persistence.
- **Emulated SPI and UART are faster than hardware.** The PL022 does not
  clock bits, and the PL011 pulls a host byte every tick. The firmware is
  written for the real rates (100 kHz to the tag), and hosts write at most
  32 bytes at a time.
- **Brokers:** `emerson ctl broker <tty>` returns the whole history, so use `tail`
  and `grep -a` (tty0 carries binary after a session). Only console-type
  brokers are reachable from hosts, which is why `wand` and `communicator`
  are consoles.
- **Implant dates drift between sessions.** OPEN anchors the implant clock
  to the programmer's "now", and the implant clock runs much faster than
  wall time.
- **Not modelled:** RP2350 brown-out as the cell collapses (the CPU runs
  on), impedance-based ERI, lead-impedance trends, EGM trigger priorities,
  ADXL362 anti-aliasing and self-test, and the regulators.

## Moving from the emerson source tree

What had to change to run on the packaged `emerson` tool, and why:

- **Boot ROM and IMAGE_DEF.** The source tree's project reset the Hazard3
  straight to 0x10000000. The image's `rp2350` project runs the real RP2350
  mask boot ROM (`bootrom: bootrom.bin`, `boot_arch: riscv`). That ROM only
  launches a flash image with a picobin IMAGE_DEF block in its first 4 KB. With
  no block, it falls back to USB boot: the PC parks in the ROM around 0x740e,
  the UARTs stay silent, and `emerson ctl logs` shows `USBCTRL unimplemented
  peripheral` warnings. `start.S` now ends with a minimal IMAGE_DEF (EXE,
  secure, RISC-V, RP2350). With no ENTRY_POINT item the ROM enters a RISC-V
  image at its first byte, so `_start` stays at 0x10000000.
- **Project name.** The project is `rp2350`, not
  `rp2350-pico2-pacemaker-demo`. `DEFAULT_PROJECT` in the programmer and
  `PROJECT` in the scenarios follow it. `emerson ctl project` prints it.
- **No `emctl`, franklin-server or `hydrate.py`.** `emerson load` / `start`
  / `stop` replace `emctl set-project` / `start`, and `emerson ctl <cmd>`
  replaces `emctl <cmd>`. `./build.sh` replaces hydration's docker build, and
  `.emerson/peripherals.yaml` replaces `peripherals.yaml`.
- **Python binding.** The image ships emerson's Python binding 1.0.11, which
  has no `Machine.subscribe_broker`. `pacemaker_programmer.subscribe_broker`
  uses it where a binding has it. Otherwise it falls back to
  `HistorySubscription`, which polls `get_broker_history` and returns only the
  bytes after its starting point. The programmer and the scenarios both go
  through it. Each read fetches the broker's whole history, so a poll costs
  more as tty1 grows over a long scenario.
- **Host tools run in the container** (`./emerson-py.sh`), and the live view
  gained `--bind` for the port forward.
- **Wall-clock timing.** `accolade_safety_mode.py`'s post-latch checks used
  wall-second windows, sized for an emulator at 0.5× wall. At 0.23× wall the
  pocket-stimulation check saw only one pace in its 5 s window and failed. Those
  checks now wait for guest time (`wait_guest`).
