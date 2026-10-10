---
name: Emerson SoC Emulator
description: Use and control the Emerson hardware/SoC emulator via the single `emerson` command (`emerson load` / `emerson start` / `emerson ctl` / `emerson run`). Use when the user mentions Emerson, the emulator, emctl, flashing/loading firmware into a simulated chip, or inspecting/stepping/debugging emulated device state (registers, memory, breakpoints, device tree, brokers).
---

# Emerson SoC Emulator

Emerson is a Dockerized SoC emulator driven by one host command, `emerson`.
Runtime control is `emerson ctl <cmd>`; there is no separate `emctl`.

## Model

Three nested things, each outliving the one inside it:

1. **The image** — the project, its device models. `emerson install` /
   `emerson image update <tarball>`.
2. **The container** (`emerson-server`) — brought up on one firmware file,
   bind-mounted in. Created only by `emerson load`, ended by `emerson shutdown`.
3. **Sessions** — running emulated machines, created from the mounted firmware
   (read at the moment the session starts). Several can run at once.
   `emerson start` / `emerson stop`.

Restarting a session takes about a second; restarting the container takes
several. So the edit-build-run loop is just a rebuild followed by
`emerson start`.

## Commands

```
emerson load [firmware] [--reload]   # container up on a firmware file (no arg: the last one); --reload ends running sessions
                                     # load/start take --project N for images with several projects
emerson start [--new]                # session on the loaded firmware (see below); --new adds one beside the recorded one
emerson stop [token|--all]           # end a session; container stays up
emerson shutdown                     # stop the container and every session in it
emerson sessions                     # running sessions, '*' = recorded
emerson info                         # version, image, container state, firmware (path, md5), sessions

emerson ctl <cmd> [args]             # one emulator command against the session (below)
emerson run [-p PORT] <script.py> [args]   # Python script against the session (below)
emerson exec [cmd [args]]            # command inside emerson-server (bash if none)
emerson cli | emerson serial [chan]  # interactive REPL / serial console; need a TTY, so not for agents

emerson peripherals pull [path] [--force] [--catalog-only]
emerson peripherals clear
emerson install                      # one-time: PATH, license, image
emerson update                       # the host script only
emerson image update <tarball>       # the image; keep it matched with the script
emerson license set|show, emerson skills pull, emerson cleanup, emerson image cleanup
```

`emerson start` is the one command to re-run:

- Container down → refuses and points at `emerson load`.
- Firmware unchanged → reattaches to the recorded session as it was.
- Firmware content changed → replaces the session.
- The rebuild replaced the file (new inode), so the container's bind mount
  can't see it → recreates the container and starts a fresh session. If that
  would end other sessions, it refuses and names them. Run
  `emerson load --reload` to accept losing them.

Use `emerson load` only for a different firmware file, or when `start` says
to. `emerson load` needs a stored license.

**Script and image versions must match.** If they don't, `emerson start` fails
with an argument error from inside the container (`unrecognized arguments`).
`emerson update` doesn't touch the image; `emerson info` shows both versions.

## Sessions

Commands act on, in order: `--session TOKEN`, `$EMERSON_SESSION`, the session
`emerson start` recorded, or the only running session of this project (which
is then recorded). If several are running and none is recorded, they refuse
and list them. Sessions are independent of each other.

`emerson ctl start/stop/shutdown/sessions/set-project` are blocked. Use the
`emerson` verbs instead.

## `emerson ctl`

One-shot and non-interactive, so it is safe for agents and CI. Stdin is
forwarded, and output is byte-exact when redirected. It needs the container
up, and a session for everything except `project`, `peripherals` and
`--help`. `emerson ctl --help` is the full reference.

```
go [counter] | pause | step [n] | reset    # step n = instructions; returns before it finishes (see Gotchas)
state                                      # exactly: running | paused | halted on error
ticks                                      # tick counter (hex)
ls [path] | find [path] | dump             # device tree (absolute paths, e.g. /MEM/i2c0)
connections                                # pin/net wiring (declared, not proof the model drives it)
r <path> [reg [val]] | registers <path>    # named registers: prefer over read-mem for peripherals
pc <path> | ic <path> | u <path> | ui <path> | details <path>
read-mem <path> <addr> <len> [-w N] [-o FILE]
write-mem <path> <addr> (<hex>|--file F|--string S)   # memory devices only
bp <path> [addr] | bpd | enable | disable  # breakpoint: CPU only, halts before the instruction
wp <path> [read|write <reg|addr>] | wpd    # watchpoint: counts hits, never halts
sp <path> [read|write <reg|addr>|fetch <addr>] | spd   # stoppoint: halts after the access
debugpoints [path]                         # every bp/wp/sp in the tree
actions <path> | action <path> <verb> [args]   # device-specific verbs (fault injection, stimuli)
broker | broker <name> [--limit N] | broker <name> <data>   # serial/data channels
logs [-f] [--level warn]
snap | snap save [name] | snap load <name>     # load takes the name without .snap
checkpoint [enable|disable]                     # needed for reverse stepping
project | peripherals
```

**`read-mem`/`write-mem` addresses are relative to `<path>`'s base.** Use
`/MEM/sram 0x484`, not `0x20000484`. On the CPU path the address space is the
whole system, so linked (ELF) addresses work as-is:
`read-mem <cpu-path> 0x20000490 4`.

## `emerson run`

```
emerson run tools/check.py --verbose      # options before the script, script args after
emerson run -p 8765 tools/live.py         # publish a port on host 127.0.0.1
emerson run --session <tok> tools/check.py
```

Runs `python3 <script>` in a throwaway container from the server's own image,
so the `emerson` library matches the server. Details:

- The current directory is mounted as the working directory. The script must
  be inside it, and files it writes land on the host.
- Sets `EMERSON_HOST` (`http://emerson-server:10314`; `localhost` is the
  script's own container), `EMERSON_SESSION`, `EMERSON_PROJECT`, and
  `EMERSON_BIND_HOST=0.0.0.0`. `emerson.attach_session()` reads the first
  three. A script with its own `--host` should default it to `$EMERSON_HOST`.
- A served port needs `-p` and must listen on `0.0.0.0`, not `127.0.0.1`.
- It runs as the host uid, with no bytecode written.
- Needs a session it can resolve, even for a script that starts its own.
- Only runs a script, so `-m pytest` won't work. Make the test file runnable
  instead:
  `if __name__ == "__main__": sys.exit(pytest.main([__file__, "-p", "no:cacheprovider", *sys.argv[1:]]))`.
- **Stopping:** Ctrl+C, or `docker kill -s INT <id>` with no terminal, raises
  `KeyboardInterrupt` and runs `finally`. `docker stop` (SIGTERM) exits
  without cleanup.

For the library itself, see the [emerson-python skill](../emerson-python/SKILL.md).

## Hardware configuration (`.emerson/peripherals.yaml`)

The chip's on-chip peripherals come with the image. Everything off-chip (the
sensors, switches, DACs, radios and cells on the board, and the pins and buses
they're wired to) is defined in `.emerson/peripherals.yaml`. Read the project's
file before changing firmware that touches hardware. Its comments are usually
the board's wiring documentation.

**Workflow:**

1. **Start from the image's file.** If the project has none, run
   `emerson peripherals pull`. It writes `.emerson/peripherals.yaml` plus a
   header listing every controller and the `native:` kinds it accepts, and
   points `emerson load` at the file. Use `--force` to overwrite and
   `--catalog-only` to just print. `emerson ctl peripherals [--bus spi]`
   prints the same catalog alongside the current file.
2. **Edit entries under the controller's device-tree path.** Every entry has
   `name:` (becomes its device name) and `native: <kind>` from the catalog
   for *that* controller (on i2c, `path: <python-file>` instead for a
   scripted target). Then add the wiring its bus needs:

   ```yaml
   /MEM/i2c0:            # i2c: bus address
     - {name: eeprom, native: at24c256, address: 0x50}
   /MEM/spi0:            # spi: the part's own pins: cs_pin, plus busy_pin / dc_pin where it has them
     - {name: adxl362, native: adxl362, cs_pin: 22, busy_pin: 27}
   /MEM/adc:             # adc: channel
     - {name: battery, native: implant_cell, channel: 0}
   /MEM/sio:             # pin: a map of the part's signal names to GPIO numbers
     - {name: magnet_sensor, native: drv5032, pins: {out: 21}}
   ```

   rmt entries take `channel` and `num-leds`. Some parts take extra options,
   such as `chemistry:` on `implant_cell` or `output_gain:` on `mcp4822`.
   **The catalog doesn't list these options or the `pins:` signal names.**
   Get them from an existing entry in this or another project's file, or ask
   the user. Never guess a name.
3. **Apply it.** An in-place edit takes effect at the next session creation:
   `emerson stop && emerson start`, or `emerson start --new`. No `load`
   needed. A file at a new path needs `emerson load`. `emerson peripherals
   clear` reverts to the image's default file.
4. **Verify. Unknown keys and wrong pin names are silently ignored.** A
   misspelled option falls back to its default, and a misnamed pin is left
   unwired, with no error and no log line. After every change, check:
   - `emerson ctl ls <controller>`: the device exists. Pin, adc and supervisor
     parts may appear under `/board` instead; check `ls /`.
   - `emerson ctl connections`: each pin you wired shows up.
   - The options took effect, not just that the device exists. Where
     `emerson ctl actions <device>` lists a status verb, read it (the cell's
     `get_status` reports its chemistry). Where it lists none (e.g.
     `mcp4822`), check the effect downstream, such as the voltage the part
     drives into the next device.

Write the board facts into the file's comments next to each entry: part
number, pin functions, why an option has its value. That keeps the file the
single description of the hardware.

## Gotchas

- **`emerson ctl step` returns before the step finishes**, so reads right
  after it land mid-step. That breaks any comparison of two values. Wait for
  it first:
  `until [ "$(emerson ctl state)" = paused ]; do sleep 1; done`.
- **wp/sp hits count only accesses by the emulated CPU or bus.** A host-side
  `ctl r`/`write-mem` doesn't trigger them, so you can't test a stoppoint
  that way.
- **`sp` can overshoot**: `hit=` may already be a few past 1 when the
  machine halts.
- **wp/sp on a peripheral path can be created but not deleted or disabled**
  (`this device does not have debug points`); only a new session clears them.
  CPU-path ones delete normally.
- **GPIO writes that change pin drive are rejected** (`a write here changes
  what the pins drive`). Fault-inject through a device's own `actions`
  instead.
- An unknown top-level `ctl` verb is an error; device verbs always need
  `ctl action <path> <verb>`.

## Debugging without instrumentation

Prefer breakpoints and memory reads over adding firmware logging:

1. Get symbol addresses from the ELF with `nm`. Re-run it after every
   rebuild, because addresses shift. Add field offsets by hand from the
   struct layout.
2. Set `bp` at the next function after the code of interest, run `go`, and
   wait for `paused`.
3. Read with `read-mem <cpu-path> <addr> <len>`. Check the technique first on
   a value you already know.

To prove an interrupt reaches firmware, check each layer: the destination
GPIO's input register flips after the device `action`; a `bp` on the ISR or
callback is hit; and its arguments name the pin you saw flip.
