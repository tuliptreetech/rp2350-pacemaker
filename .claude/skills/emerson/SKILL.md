---
name: Emerson SoC Emulator
description: Use and control the Emerson hardware/SoC emulator via the single `emerson` command (`emerson load` / `emerson start` / `emerson ctl`). Use when the user mentions Emerson, the emulator, emctl, flashing/loading firmware into a simulated chip, or inspecting/stepping/debugging emulated device state (registers, memory, breakpoints, device tree, brokers).
---

# Emerson SoC Emulator

Emerson is a Dockerized SoC emulator, driven by **one** host command installed
to `~/.local/bin`: **`emerson`**. It owns the whole lifecycle — installing and
updating itself, bringing up the `emerson-server` Docker container on a
firmware image, starting emulation *sessions* on it, and talking to those
sessions.

**This skill documents emerson 1.0.14 and later.** Check with
`emerson version`. 1.0.14 split firmware selection out of `emerson start` into
`emerson load`, and `emerson skills pull` always takes the latest skills, so a
skill newer than the installed script is the normal mismatch — if `emerson
start ./flash.bin` is accepted rather than refused, the install predates this
skill and the Gotchas below do not apply to it.

The five verbs that cover most work:

```
emerson load ./flash.bin    # point the emulator at a firmware image
emerson start               # a session on it
emerson ctl ls              # talk to that session; 'emerson ctl <cmd>' for everything else
emerson stop                # end the session, leave the container up
emerson shutdown            # stop the container itself
```

`load` chooses the firmware, `start` runs a session on it. `emerson start` does
not take a firmware argument and refuses to bring the container up itself —
which firmware is running has to be settled before any session exists, because
a session reads `flash.bin` at the moment it is created.

There is no standalone `emctl` command — runtime control is `emerson ctl`, and
the interactive REPL is `emerson cli`.

**The host script and the container image must be version-matched.** The script
drives the server with flags its own release understands, so a mismatched image
fails at `emerson start` with an argument error out of the in-container
`emctl` (`unrecognized arguments: --quiet`, say) followed by
`Error: failed to start a session`. `emerson update` deliberately does *not*
touch the image — pair it with `emerson image update <tarball>` (see below).
`emerson info` prints both; check them against each other when `start` fails
oddly.

Current state (version, image, container status, loaded firmware, sessions) is
stored in `~/.emerson/config` (bash-sourceable), but don't read that file
directly to check state — use `emerson info` instead:

```
$ emerson info
Emerson version: <version>
Docker image: ghcr.io/tuliptreetech/emerson/<project>:<version>-local
Peripherals file: /path/to/.emerson/peripherals.yaml
Container (emerson-server): running
Firmware loaded: /path/to/build/firmware.bin
  Timestamp: Sep  3 00:03:24 2026
  MD5 sum:   c20e8d1e7adacd54334e875924befbf1
Recorded session: 39b199a9b723428db1473b4a74672c58
Running sessions:
  39b199a9b723428db1473b4a74672c58  <project>  *
```

`Recorded session` is which session this install's commands default to;
`Running sessions` is what the server actually has (only shown while the
container is up, and `*` marks the recorded one). They can disagree — that is
the point of showing both.

For the current *project* (not shown by `info`), use `emerson ctl project` rather
than grepping the config file. The config's `emerson_project` key should match
the project name test suites pass to `run_project`/`--project`.

## Mental model / lifecycle

Three nested things, each outliving the one inside it:

1. **The Docker image** — the project, its device models, its `flash.bin`.
   Installed once (`emerson install` / `emerson image update`).
2. **The container** (`emerson-server`) — one per host, brought up on a
   *specific firmware file* that is bind-mounted in. Created only by
   `emerson load`; ended by `emerson shutdown`.
3. **A session** — one running emulated machine, created from the mounted
   firmware. Several can run at once in the same container. Ended by
   `emerson stop`.

```
emerson install             # one-time: PATH+symlinks, license, pull docker image
emerson peripherals pull    # optional: pull .emerson/peripherals.yaml, trim to devices you care about
emerson load ./flash.bin    # bring the container up on this firmware
emerson start               # a session on it, paused at reset
emerson ctl go / step / ... # drive execution, inspect state
emerson stop                # end the session; container stays up for the next 'start'
emerson shutdown            # stop the container, ending every session in it
emerson exec ...            # run a command inside the running container (bash, python3, etc.)
```

The container/session split is a speed tradeoff: restarting a *session* takes
about a second, restarting the *container* costs the image load and the
server's whole startup (~8 s here). That is why the rebuild loop is a bare
`emerson start` — it replaces only the session, and reaches for the container
only when it has to (see the stale-mount gotcha).

`emerson start` is idempotent and is meant to be the one command you re-run:

- Container down → **refuses**, naming the last-loaded firmware and pointing at
  `emerson load`. It never picks a firmware for you.
- Firmware unchanged → **reattaches** to the recorded session, leaving it
  exactly as it was (a `running` machine keeps running; the tick counter keeps
  advancing). Prints `Reusing session <token>.`
- Firmware *content* changed (md5 of the file differs from what was recorded
  when the session started) → stops that session and starts a fresh one on the
  new image. Prints
  `Firmware changed since session <token> started; replacing that session.`
- The container can no longer see the firmware through its mount — a rebuild
  replaced the file rather than rewriting it — → recreates the container
  itself and starts a fresh session. Prints `The firmware was replaced on disk
  since it was loaded, so ...; reloading it.` If other sessions would be lost
  in the process it refuses instead and names them; see Gotchas.

So the rebuild-and-rerun loop *is* a bare `emerson start`. Reach for
`emerson load` when you want a **different** firmware file, or when `start`
tells you to.

A running container is a prerequisite for every `emerson ctl` command (the
wrapper errors `Emerson Server is not running. Please start it with 'emerson
load <firmware-file>'`). Within that container, a session is a prerequisite
for nearly all of them — the exceptions are `project`, `peripherals`, and
`--help`, which read static project config and work with the container alone.

## `emerson` — host lifecycle commands

```
# Running the emulator
emerson load [firmware] [--reload] [--project N]        # bring the container up on a firmware file; no firmware = reload the last one
emerson start [--new] [--project N]                     # a session on the loaded firmware; reattaches or replaces (see lifecycle above)
emerson stop [session|--all]                            # end a session, leave the container up
emerson shutdown                                        # stop the container, ending every session in it
emerson sessions                                        # list running sessions, '*' marks the recorded one

# Talking to a session
emerson ctl <command> [args]                            # one-shot emulator command (see runtime section below)
emerson cli                                             # interactive emulator REPL (needs a TTY)
emerson serial [channel]                                # serial terminal on a broker channel; omit name to pick (needs a TTY)
emerson exec [command [args]]                           # run a command in emerson-server (bash if omitted)

# Installation and setup
emerson install [--force-license-key] [--force-pull]   # set up PATH/symlinks, license, pull image
emerson update                                          # update the emerson script itself
emerson cleanup                                         # remove downloaded script versions nothing runs any more (NOT docker images — see 'image cleanup')
emerson image update <tarball>                          # docker load a tarball, restart server from it
emerson image cleanup                                   # remove old Docker images loaded by install/'image update'
emerson license set [KEY]                               # store/overwrite license key (prompts if omitted)
emerson license show                                    # show whether a key is stored (never prints it)
emerson skills pull                                     # download the latest Emerson Claude Code skills into .claude/skills in the cwd, overwriting what's there

# Peripherals
emerson peripherals pull [path] [--force] [--catalog-only]  # pull the project's peripherals.yaml + I2C catalog from the image
emerson peripherals clear                               # clear the stored peripherals.yaml override so 'start' falls back to the image default (doesn't delete the file)

# Other
emerson version                                         # print installed emerson version
emerson info                                            # version, image, container status, firmware, sessions
emerson help
```

Global option: `--session TOKEN`, valid with `ctl`, `cli`, `serial`, and
`sessions`.

`cleanup` and `image cleanup` are different things: `cleanup` removes
downloaded *script* versions from `~/.emerson/downloads`, `image cleanup`
reclaims disk space from stale *Docker images*.

`emerson exec` is the supported way to run something inside the `emerson-server`
container (`emerson exec` alone opens an interactive bash shell; add a command
and args to run it directly, e.g. `emerson exec python3 -c "..."`; stdin is
forwarded, so `emerson exec python3 -` works for piping in a script). Prefer it
over raw `docker exec ... emerson-server ...`.

Only `load`, `start`, `shutdown`, `license`, and `skills` check for a newer
release, and at most once a day (stamped in `~/.emerson/last_update_check`,
overridable with `$EMERSON_UPDATE_CHECK_INTERVAL`). Notably `emerson ctl` does
**not** — it's the inner loop of a debugging session, so it makes no network
round trip.

## Sessions

A session is one running emulated machine. The server can run several at once,
including several of the same project, so every command that acts on one has
to know which. `emerson start` records the token it created, and the rest
default to it.

```bash
emerson start                 # records a session; prints "Session:   <token>"
emerson start --new           # an *additional* session, leaving the first alone
emerson sessions              # list them; '*' is the recorded one
emerson ctl --session <tok> state
emerson stop <tok>            # end one; --all for every one
```

Which session a command acts on, in order:

1. `--session TOKEN` on the command line
2. `$EMERSON_SESSION` — a per-shell override, which is how you drive two
   sessions from two terminals without them fighting over one config file
3. the recorded session in `~/.emerson/config`
4. the only session of this install's project, if there's exactly one — it
   gets adopted and recorded, so the next command doesn't have to work it out

With more than one session running and nothing recorded, commands fail with
`Error: more than one session is running - say which one with --session
<token>` and list the candidates, rather than guessing. Adoption is scoped to
this install's project, so a session belonging to another project in the same
image is never silently adopted.

Verified behavior: two sessions on the same project are genuinely independent —
`emerson ctl --session A go` left A `running` with an advancing tick counter
while B stayed `paused` at `0x0`. `emerson stop <other-token>` stops that
session without clearing the recorded one; `emerson stop` on the recorded
session clears it.

**Lifecycle verbs are blocked on the `ctl` passthrough.** `emerson ctl start`,
`ctl stop`, `ctl shutdown`, `ctl sessions`, and `ctl set-project` all refuse
with a message naming the `emerson`-level equivalent to use instead. That's
deliberate: `emerson ctl` injects the recorded token, so `emerson ctl stop`
would stop the recorded session while leaving it recorded as current. Use
`emerson start` / `stop` / `shutdown` / `sessions` / `load --project <name>`.

## I2C peripherals (`.emerson/peripherals.yaml`)

`emerson peripherals pull` reads the current project's peripheral config and
I2C catalog straight out of the loaded Docker image — no running container or
session needed. It writes `.emerson/peripherals.yaml` in the current
directory (fails if one's already there; pass `--force` to overwrite). Pass
`--catalog-only` to just print the available native kinds without writing
anything.

The written file lists, per I2C controller device-tree path (e.g.
`/MEM/i2c1`), every device currently attached plus a comment block of native
kinds that controller accepts:

```yaml
/MEM/i2c1:
  - name: bq25892
    address: 0x6B
    native: bq25892
  - name: max17043
    address: 0x36
    native: max17043
  - name: eeprom
    address: 0x50
    native: at24c256
```

Each entry needs `name`, `address` (hex), and either `native: <kind>` (one of
the catalog kinds for that controller) or `path: <python-file>` for a custom
target. **To scope the emulated bus down to only the devices you care about,
edit this file directly** — delete the entries you don't need, keep/add the
ones you do. `emerson load` automatically mounts `.emerson/peripherals.yaml`
into the container when it's present in the cwd, so edits take effect on the
next `emerson start`; no need to re-pull, rebuild, or reload anything. (Unlike
the firmware, `peripherals.yaml` is edited in place, so its bind mount
survives — but the container still only re-reads it when a session is
created. A peripherals file at a *new path* is a new mount, so that one does
need an `emerson load`.)

`peripherals pull` takes an optional `[path]` (a directory gets
`peripherals.yaml` appended); either way it also points Emerson's stored
peripherals reference at that file for `emerson load` to pick up. To go back
to the image's default peripherals instead of your override, run
`emerson peripherals clear` — it clears the stored reference but does not
delete the override file itself.

To inspect the catalog/current peripherals from inside a running container
without needing a started project *session*, use `emerson ctl peripherals` — see
the runtime section below.

## `emerson ctl` — runtime control commands

`emerson ctl <command>` runs one emulator command against a session and exits.
It's a passthrough to the `emctl` binary *inside* the container, with
`--host` and `--session` injected for you (both still win if you pass them
yourself).

Full built-in reference: `emerson ctl --help` (works with the container up, no
session needed). Global options: `--host HOST` (default
`http://localhost:10314`), `--project NAME` (else `$EMERSON_PROJECT`, else
`~/.emerson/config`), `--session TOKEN`.

**Config**
- `emerson ctl project` — print current default project (no session needed)
- To change the default project, use `emerson load --project <name>` — `ctl set-project` is blocked (see Sessions)
- `emerson ctl peripherals` — print the I2C peripheral catalog and the project's current `peripherals.yaml`; reads static config, so no session is needed (just the container running)

**Session/server management** — not available via `ctl`; use the `emerson`-level verbs
- `emerson load [firmware]` — bring the container up on a firmware file
- `emerson start` / `emerson start --new` — create a session
- `emerson stop [token|--all]` — end a session, container stays up
- `emerson shutdown` — stop the container, ending every session
- `emerson sessions` — list them

**Execution control**
- `emerson ctl go [counter]` — resume; optional hex tick count to run until
- `emerson ctl pause`
- `emerson ctl step [n]` — step n instructions (decimal or `0x` hex), default 1.
  **Returns before the step finishes** — see the gotcha below before reading
  state afterwards.
- `emerson ctl reset` — reset to initial state

**Status**
- `emerson ctl state` — prints `running`, `paused`, or `halted on error`. All lowercase, and the last one is spaced, not camel-cased — match it exactly if a script compares against it.
- `emerson ctl ticks` — current tick counter (hex)

**Device tree** (paths are absolute, e.g. `/PXA270`, `/PXA270/core0`, `/system/uart0`)
- `emerson ctl ls [path]` — list children (default `/`)
- `emerson ctl find [path]` — device tree as JSON
- `emerson ctl dump` — all devices + common register values
- `emerson ctl connections` — list inter-device port/pin/net wiring, e.g. a peripheral's `int`/`alrt`-style pin wired to a GPIO input (`gpioc.pin0.in -> /MEM/i2c1/<device>.int`) or an IRQ line into the NVIC. Peripheral-pin wiring is declared per-device in `.emerson/peripherals.yaml` under a `pins:` map (e.g. `pins: { int: { device: "/MEM/gpioc", pin: 0 } }`) and shows up here once configured. **A connection listed here documents intended wiring, not proof the device model drives that pin at runtime** — a peripheral's fault/alert condition (`inject_fault`, `set_soc` past the alert threshold) can leave the target GPIO's `IDR` untouched, i.e. the pin wired per `connections` but never actually toggled. Don't take `connections` output alone as proof of a working pin-level effect: verify by reading the destination GPIO's `IDR` after triggering the condition, and ideally also confirm the firmware's own interrupt callback actually runs (see below).

**Snapshots**
- `emerson ctl snap` / `emerson ctl snap save [name]` / `emerson ctl snap load <name>` (name = timestamp if omitted; no `.snap` extension in `load`)

**Checkpointing** (required for reverse stepping)
- `emerson ctl checkpoint` / `emerson ctl checkpoint enable` / `emerson ctl checkpoint disable`

**Data brokers** (named async I/O channels, e.g. UART TTYs, external displays)
- `emerson ctl broker` — list channel names
- `emerson ctl broker <name>` — read available bytes (raw to stdout; `--limit N`)
- `emerson ctl broker <name> <data>` — write a UTF-8 string

**Logs**
- `emerson ctl logs` / `emerson ctl logs -f` (stream) / `emerson ctl logs --level warn` (debug<info<warn<error)

**OS awareness** (needs project's `os_handler` configured)
- `emerson ctl os ps` / `os set <pid>` / `os unset` / `os maps [pid]` / `os modules` / `os regs [pid]` / `os scan`

**Per-device** (require `<path>`)
- `emerson ctl r <path> [reg]` — print registers, or one; `emerson ctl r <path> <reg> <val>` to set (val can be a number or another register name). **Prefer this over `read-mem`/`write-mem` for named peripheral registers** (GPIO MODER/PUPDR/IDR/ODR, timers, etc.) — `emerson ctl r <path>` with no reg lists every named register on that device, so there's no need to hand-compute byte offsets the way `read-mem`/`write-mem` require. Reserve `read-mem`/`write-mem` for genuinely address-based memory (SRAM/flash contents, GDDRAM-style framebuffers) that has no named-register abstraction.
- `emerson ctl registers <path>` — all registers
- `emerson ctl pc <path>` / `emerson ctl ic <path>` — program/instruction counter (CPU only)
- `emerson ctl details <path>` — kind, memory, registers
- `emerson ctl u <path>` / `emerson ctl ui <path>` — disassemble next 10 instrs at PC (`ui` adds p-code)
- `emerson ctl read-mem <path> <addr> <len> [-w N] [-o FILE]` — hex dump or raw write to FILE; `-w` groups into N-byte little-endian words. **`<addr>` is relative to `<path>`'s own base, not an absolute system address** — e.g. `emerson ctl read-mem /MEM/sram 0x484 4` (offset into that 0x2000-byte device), not `0x20000484`; the latter errors "out of range" against the device's own (small) size. The one path where relative-to-base and absolute happen to coincide is the CPU (`/Cortex-M0` or similar) — its address space starts at 0 and covers the whole system, so full linked addresses (from an ELF's symbol table, vector table, etc.) can be passed straight through: `emerson ctl read-mem /Cortex-M0 0x20000490 4`.
- `emerson ctl write-mem <path> <addr> (<hex>|--file FILE|--string STR)` — same relative-to-`<path>` addressing as `read-mem`. **Memory devices only** (RAM/ROM/flash) — it writes fixed-width words sized to whatever device sits at `<addr>`, which isn't a real MMIO access path; a payload that overruns the target keeps writing into whatever's mapped next. For hardware registers, use `emerson ctl r <path> <reg> <val>` instead.
- `emerson ctl db <path> <addr> [count]` — read-only alternate to `read-mem` via the raw command language; hex/decimal `<addr>`, defaults to 16 bytes, always prints a hex dump (no `-w`/`-o`). Prefer `read-mem` when you want word-grouping or file output.
- `emerson ctl write <path> <addr> <hex>` — raw-command-language alternate to `write-mem`; `<addr>` may also be a register+offset (e.g. `r1+4`, `r1-4`), but only takes a positional hex string (no `--file`/`--string`). Same memory-device-only caveat as `write-mem`.

**Debugpoints — machine-wide view**
- `emerson ctl debugpoints [path]` — walks the whole device tree and lists every breakpoint, watchpoint, and stoppoint set anywhere (kind, target, enabled/disabled, hit count, access mode), optionally filtered to devices whose path starts with `<path>`. Devices with none set, or that don't support them, are omitted. Use this to get an overview across devices instead of checking `bp`/`wp`/`sp` one path at a time.

**Breakpoints / watchpoints / stoppoints** (require `<path>`) — see the dedicated
section below for semantics, id scoping, and a real deletion bug to watch for.
- `bp <path> [addr]` — fetch breakpoint, **CPU devices only**; halts *before* the instruction runs. `bpd <path> <id>`, `enable <path> <id>`, `disable <path> <id>`.
- `wp <path> [read|write <reg|addr>]` — watchpoint; records the access (`hit=` counter) but **never halts** the machine. `wpd <path> <id>`.
- `sp <path> [read|write <reg|addr>|fetch <addr>]` — stoppoint; halts *after* the access completes. `spd <path> <id>`.

**Custom device actions** (board/peripheral models expose their own verbs)
- `emerson ctl actions <path>` — list verbs + usage
- `emerson ctl action <path> <verb> [args...]` — invoke one (args joined w/ spaces, parsed by the device)

## Non-interactive / scripted / agent use

`emerson ctl` is one-shot and non-interactive by design (per its own `--help`:
"designed for scripting, automation, and LLM agent use"), so every command runs
fine with no TTY — call `emerson ctl <args...>` directly from agents/CI.

It always passes `-i` to `docker exec`, so piped stdin (a heredoc, `< payload.hex`)
reaches the container, and only adds `-t` when both ends really are terminals.
That matters for byte-exact output: `emerson ctl broker tty0 > capture.bin`
stays uncorrupted precisely because no pseudo-TTY is allocated to translate
newlines. When you *do* have a terminal, the `-t` is what forwards Ctrl+C to a
streaming command (`emerson ctl logs -f`) instead of leaving it running inside
the container.

`emerson cli` and `emerson serial` are the two exceptions — both are terminal
programs (a REPL and a raw-mode serial console) and refuse outright without a
TTY:

```
Error: 'emerson cli' needs an interactive terminal.
  For scripted use, run one-shot commands with 'emerson ctl' instead.
```

So an agent should never reach for `emerson cli`/`emerson serial`; read a
serial channel with `emerson ctl broker <name>` instead.

## Typical session, end to end

```bash
emerson load ./flash.bin        # container on that firmware (needs a valid license)
emerson start                   # a paused session on it
emerson ctl state               # -> paused
emerson ctl ls                  # top-level device tree, e.g.: Cortex-M0  MEM
emerson ctl find                # full tree as path/kind pairs, with offsets for addressed devices
emerson ctl pc /Cortex-M0
emerson ctl bp /Cortex-M0 0x08001234
emerson ctl go
emerson ctl logs -f             # Ctrl+C to stop streaming
emerson stop                    # end the session, container stays up
```

Then the edit-build-run loop, which is a bare `emerson start`. It replaces the
session, and silently repairs the container's firmware mount first if the
rebuild replaced the file rather than rewriting it (see Gotchas):

```bash
make                            # or ./docker-build.sh
emerson start
```

## Breakpoints, watchpoints, and stoppoints — which to use

All three require a device tree `<path>` (`emerson ctl ls`/`find` to locate one). What
they have in common: creating one prints `id=N`; listing (`bp`/`wp`/`sp` with no
further args) shows `id=N [Type] {...} (access) [REG] enabled hit=N`; `hit=`
only increments on a genuine access made *by the emulated CPU/bus* — see the
gotcha below, host-side pokes don't count. IDs are scoped **per device path**,
and `wp`/`sp` share one counter on a given path (e.g. on `/MEM/crc`, a `wp`
then an `sp` then another `wp` came back `id=0`, `id=1`, `id=2`); a different
device path starts its own counter at 0.

Pick by what you're trying to catch:

- **`bp` (breakpoint)** — you know *which instruction* you want to stop at
  (an ELF symbol, a disassembled address) and want execution to stop *before*
  it runs. CPU devices only (`/Cortex-M0`) — pointing `bp` at a peripheral
  errors `this device does not have address break points`. Best for "stop
  when this function/line is reached," regardless of what data it's about to
  touch.
- **`wp` (watchpoint)** — you want to know *whether/how often* a register or
  address is touched, without perturbing timing. It never halts the machine,
  so it's safe to leave armed across a timing-sensitive stretch (watchdogs,
  bus timeouts) and check the `hit=` counter afterwards. Good for "is this
  register even read by the firmware" before spending time on a real
  breakpoint hunt.
- **`sp` (stoppoint)** — you want a hard stop *right after* a specific
  register/address is read, written, or fetched, but don't know (or don't
  want to hunt for) which instruction does it. Halts after the access
  completes, so the access has already happened when you inspect state —
  read the *new* value, not the pre-access one. Good for catching the first
  unexpected write to a region, or the exact moment a peripheral register
  changes during a fault-injection run (see custom device actions above,
  e.g. a peripheral's own `inject_fault`-style verb).

Confirmed by testing:

- A CPU-register watchpoint (`emerson ctl wp /Cortex-M0 read r0`) accumulated real
  hits just from normal execution (`hit=5` within a second, since r0 is
  touched on nearly every call/return) — watchpoints do track genuine guest
  activity, not just theoretically.
- **Host-initiated register writes don't trigger wp/sp.** Arming
  `sp /MEM/crc write POL` and then writing that same register from the host
  via `emerson ctl r /MEM/crc POL 0x7` left `hit=0` and the machine `running` —
  the stoppoint only fires on an access driven by the emulated CPU/bus, not
  on a debug-interface poke from `emerson ctl r`/`write-mem`. Don't use `emerson ctl r`
  to "test" that a stoppoint is wired up; you have to make the firmware do
  the access.
- **`sp` on a CPU device genuinely halts the machine** — arming
  `sp /Cortex-M0 read r0` then `emerson ctl go` came back `paused` almost
  immediately (r0 is touched on nearly every call/return), confirming a
  stoppoint really stops execution rather than just logging. But **it can
  overshoot**: the `hit=` counter read right after the halt was `3` one run
  and `5` on a repeat, not `1` — a few extra matching accesses can happen
  before the halt is actually enforced, the same class of async/batching lag
  as the documented `step`-returns-early gotcha. Don't assume you're stopped
  at the *first* matching access; check `hit=` and treat a low overshoot as
  normal. (A stoppoint armed on a peripheral register — `/MEM/i2c1` `ISR`
  read — was left running for several minutes of wall-clock time without
  ever firing in this session; it's unclear whether that's because the
  firmware simply wasn't touching that register in that stretch, or a
  peripheral-specific quirk, so treat peripheral-scoped `sp` as unverified to
  actually halt until you've seen it happen for your own case.)
- **`wpd`/`spd`/`enable`/`disable` don't work against watchpoints or
  stoppoints set on a non-CPU peripheral device.** Creating and listing a
  `wp`/`sp` on a peripheral path (e.g. `/MEM/crc`, `/MEM/i2c1`) works fine,
  but deleting or disabling that same id errors
  `this device does not have debug points` — even though the id clearly
  exists in the `wp`/`sp` listing. The identical operation against a
  `/Cortex-M0`-scoped watchpoint (by id) deleted cleanly. No workaround was
  found short of ending the session (`emerson stop` + `emerson start`); a stray
  peripheral watch/stoppoint is otherwise harmless to leave in place (`wp`
  never halts, and an un-hit `sp` never halts either), but budget for not
  being able to remove it mid-session.

## Gotchas

- **A rebuild often invalidates the firmware bind mount — `emerson start`
  detects and repairs that, but can be blocked by other sessions.**

  The firmware reaches the container as a **single-file bind mount**, which
  Docker pins to the file's *inode* when the container is created.
  `arm-none-eabi-objcopy` (and most build tools) writes its output by creating
  a **new** file rather than rewriting the old one in place, so after `make`
  the mount points at an inode the host has unlinked. It shows up two ways
  depending on the host:

  - Docker Desktop's file sharing drops the file, so the server gets
    `No such file or directory` when it reads `flash.bin` — which it only does
    as a session is created.
  - A native Linux bind mount keeps the unlinked inode alive and serves its
    **old contents**, so nothing errors and the session silently runs stale
    firmware.

  `docker inspect` reports the configured source path in both cases, so the
  mount looks healthy from outside. `emerson start` therefore asks the
  container directly — readability plus an md5 of what it actually sees —
  *before* it touches any session, and recreates the container when the answer
  is wrong:

  ```
  $ make && emerson start
  The firmware was replaced on disk since it was loaded, so the container can no
  longer read it; reloading it.
  Session:   0e6358a24ad4bd27
  ```

  Recreating the container ends **every** session in it, which is free only for
  the session `start` was going to discard anyway. When anything else is
  running — another shell's session, or the one `--new` was meant to sit
  beside — `start` refuses instead and names them:

  ```
  Error: the container is not reading the firmware on disk - it was replaced
  since it was loaded, so the container can no longer read it.
    /work/build/f030-i2c-charger.bin
    Fixing that means recreating the container, which would end these sessions:
      4953d2b04416bbfe  stm32f030r8
    Run 'emerson load --reload' to do that, or 'emerson shutdown' first.
  ```

  **Nothing is destroyed by that refusal** — the sessions it names are still
  running. Do what it says (`emerson load --reload`, accepting their loss) or
  stop them first. Don't reach for `emerson shutdown && emerson start` as a
  reflex the way older releases required; it costs the full container startup
  (~8 s) on every rebuild, which is exactly what the session/container split
  exists to avoid.

  A build that writes its output in place (`dd conv=notrunc`, or `cp` over the
  existing file) preserves the inode and never triggers any of this — the new
  bytes propagate straight through the live mount. The standard objcopy-based
  one doesn't. Fixed in 1.0.14 ([emerson-issues#22](https://github.com/tuliptreetech/emerson-issues/issues/22));
  on 1.0.13 and earlier a bare `emerson start` after a rebuild stops the
  working session and then fails to replace it, leaving none, and `--reload`
  does not help because it was gated on the firmware *path* differing.

- **The host script and the container image must be version-matched.** The
  script drives the server with flags its own release understands, so a
  mismatched image makes `emerson start` die with an argument error out of the
  in-container `emctl` (`unrecognized arguments: --quiet`, say) followed by
  `Error: failed to start a session`. `emerson update` updates only the script,
  so pair it with `emerson image update <tarball>`. `emerson info` shows both
  versions — check them against each other when `start` fails oddly.
- Almost every `emerson ctl` command needs both: (1) the `emerson-server` container running, which is `emerson load`, and (2) a session started, which is `emerson start`. The error messages name exactly which precondition is missing — read them, don't guess. The exceptions that need no session are `ctl project`, `ctl peripherals`, and `ctl --help`.
- `emerson load` requires a valid stored license (`emerson license show`/`emerson license set`), as does `emerson start` on the path where it reloads the container itself.
- `action` vs top-level commands: an unrecognized top-level verb is a hard error, never silently treated as a device action — you must type `emerson ctl action <path> <verb>` explicitly. Use `emerson ctl actions <path>` first to see what a device supports.
- `snap load <name>` takes the snapshot name *without* the `.snap` extension.
- `sp` (stoppoint) halts the emulator *after* the access completes, unlike a breakpoint which halts before executing.
- **`emerson ctl step` returns before the step has finished.** It dispatches the command and exits while the emulator is still executing, so anything you read immediately afterwards may be sampled mid-step. Poll `emerson ctl state` until it reports `paused` before inspecting:

  ```bash
  emerson ctl step 2000000
  until [ "$(emerson ctl state)" = "paused" ]; do sleep 1; done
  emerson ctl ticks   # only now is this a settled value
  ```

  Small steps hide this: they finish faster than the next `docker exec` round trip (~250 ms), so `emerson ctl step 100` looks perfectly synchronous. Scale up and it stops being. Measured: after `emerson ctl step 2000000` the call returned in 284 ms, `emerson ctl state` reported `running`, and three successive `emerson ctl ticks` gave `0x407a5`, `0x62e6d`, `0x8032d`. After polling to `paused`, three reads all gave `0x3d3075`.

  This bites hardest when you read **two or more** locations per step and compare them: each read lands at a different point in emulated time, so a correlation between two counters can be destroyed (or manufactured) by the sampling alone. Tracked as [emerson-issues#11](https://github.com/tuliptreetech/emerson-issues/issues/11).
- `emerson update` only refreshes the `emerson` host script, not the running Docker image — use `emerson image update <tarball>` for that. See the version-matching gotcha above: updating one without the other breaks `emerson start`.
- **GPIO register writes that change pin drive are rejected outright**, via either `write-mem` or `emerson ctl r <path> <reg> <val>` — e.g. clearing `PUPDR` bits to fake a broken pull-up/corroded connector fails with `Error while writing to device gpiob: a write here changes what the pins drive onto the external circuit`. This is a deliberate guardrail (the emulator models the electrical consequence of the write), not a bug, and it isn't bypassed by using one command over the other. There's no supported way to fault-inject at the raw GPIO/bus-electrical level; use a peripheral's own `emerson ctl actions <path>` fault-injection verbs instead (e.g. `inject_fault <condition>`) to simulate a damaged/glitching sensor.

## Debugging firmware state without instrumentation

Prefer breakpoints + direct register/memory inspection over adding temporary
`printf`/UART logging to the firmware under test. It's faster to iterate
(no rebuild-reflash cycle per guess), and it doesn't risk the debug code
itself perturbing timing-sensitive behavior (watchdogs, bus timeouts) you're
trying to diagnose.

General recipe for "is this C global/struct field what I expect it to be right
now":

1. **Find the address.** Symbols aren't loaded into the emulator — get them from the
   ELF instead: `arm-none-eabi-nm build/firmware.elf | grep -i <symbol>`. If
   that binary isn't installed on the host, check whether the project's own
   build toolchain already has it (e.g. a Docker-based build container) before
   installing a separate copy — `docker compose run --rm <build-service>
   arm-none-eabi-nm ...` works exactly the same way. For a
   struct field (e.g. `hi2c1.ErrorCode`), `nm` only gives you the struct's base
   address; add the field's byte offset by hand from the struct's typedef
   (count each member's size, respecting natural alignment — e.g. on a Cortex-M0
   `HAL_StatusTypeDef`/enum members are 4 bytes, pointers are 4 bytes, and a
   `uint16_t` pair packs into 4 bytes without padding). Recompute this offset
   fresh after any rebuild if the struct layout could plausibly have changed —
   but note **the addresses of file-scope globals themselves can also shift
   between rebuilds** (a change elsewhere in the same translation unit,
   or even in an unrelated file, can shift `.bss`/`.data` layout), so re-run
   `nm` for the base symbol after every rebuild rather than assuming it's
   stable — don't just reuse offsets computed against a stale build.
2. **Set a breakpoint past the code you care about**, e.g. at the entry of the
   next function called after it (`emerson ctl bp /Cortex-M0 <addr>`), then
   `emerson ctl go` and wait for `emerson ctl state` to report `paused`.
3. **Read the value** with `emerson ctl read-mem /Cortex-M0 <addr> <len>` (see the
   `read-mem` addressing note above — use the CPU device path so linked/ELF
   addresses work unmodified). Sanity-check the technique on a known-good
   value first if the result looks surprising (e.g. read a handle's
   `Instance` pointer field and confirm it equals the peripheral's known base
   address, like `0x40005400` for `I2C1` on an F0) before trusting a field
   you don't have an independent way to verify.

Worked example: an I2C driver was silently failing (no errors surfaced, but
nothing appeared on a simulated display). Rather than adding `printf` calls,
`arm-none-eabi-nm` found `hi2c1`/`hi2c2` (`I2C_HandleTypeDef` handles), a
breakpoint was set just after the failing calls, and `read-mem` on each
handle's `ErrorCode` field (offset 76 into the struct: `Instance` (4) +
`Init` (8 × `uint32_t` = 32) + `pBuffPtr` (4) + `XferSize`+`XferCount`
(2+2) + `XferOptions` (4) + `PreviousState` (4) + `XferISR` (4) + `hdmatx`
(4) + `hdmarx` (4) + `Lock` (4) + `State` (4) + `Mode` (4) = 76) showed
`0x00000004` — `HAL_I2C_ERROR_AF` (ack failure). That pointed straight at an
electrical cause (missing GPIO pull-ups on the I2C pins) instead of a
protocol/logic bug, without touching the firmware at all.

### Verifying a peripheral-driven interrupt actually reaches firmware

A peripheral model asserting a fault/alert pin doesn't by itself prove the
interrupt fires in firmware — the pin has to toggle the right GPIO bit *and*
that has to propagate through EXTI/NVIC *and* the firmware's own callback has
to run with the right pin argument. Check all three layers rather than
inferring the last two from the first:

1. **Pin level.** `emerson ctl connections` to find which GPIO pin the peripheral's
   pin is wired to, then `emerson ctl r <gpio-path> IDR` for a baseline. Trigger the
   condition via the peripheral's own `emerson ctl action <path> <verb>` (e.g. an
   `inject_fault`/`set_soc`-style verb), then re-read `IDR` — an active-low
   pin should flip from 1 to 0.
2. **Firmware level.** Find the firmware's interrupt callback symbol with
   `arm-none-eabi-nm` (e.g. `HAL_GPIO_EXTI_Callback` on STM32 HAL-based
   firmware), set a CPU breakpoint on it (`emerson ctl bp /Cortex-M0 <addr>`), `go`,
   and trigger the condition. Hitting the breakpoint confirms the interrupt
   actually reached the ISR; `emerson ctl pc` should match the breakpoint address,
   and the callback's own arguments (in `r0`, `r1`, ... per AAPCS on Cortex-M0)
   confirm *which* pin/line it thinks fired — cross-check that against the
   pin bit you saw flip in step 1, rather than assuming they're the same one.
3. **Timing context.** If the goal is explaining a slow-feeling update (e.g.
   "the display took a while to reflect a fault"), sample `emerson ctl ticks`
   across a wall-clock `sleep` to get the emulator's actual simulated
   cycles-per-second. Comparing that to the core's real clock speed
   quantifies how many real seconds a full polling interval costs in the
   emulator — useful for judging whether an interrupt-driven fast path (vs.
   waiting for the next poll) is worth the firmware complexity.

## Scripting beyond `emerson ctl`

For control flow (polling loops, conditionals) or capabilities `emerson ctl` doesn't
expose (checkpoint step-back, blocking waits on debugger/serial events, etc.),
the emulator also has a Python binding installed inside the `emerson-server`
container — see the [emerson-python skill](../emerson-python/SKILL.md).
