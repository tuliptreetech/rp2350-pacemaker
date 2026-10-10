---
name: Emerson Python Library
description: Script the Emerson emulator directly via its Python bindings (the `emerson` module shipped in the `emerson-server` image, run from the host with `emerson run script.py`) instead of shelling out to individual `emerson ctl` commands. Use when the user wants a Python script or REPL against Emerson, mentions `import emerson`, `emerson run`, `attach_session`, `EmulatorController`, `Connection`, `Machine`, `Device`, or needs control flow (polling loops, conditionals, parsing broker/UART data) that a single `emerson ctl` call can't express.
---

# Emerson Python Library

The `emerson` package is a binding to the same engine `emerson ctl` drives,
and it exposes more than `ctl` does: reverse stepping, broker subscriptions,
snapshot bytes, `run_command_string`. Use it for control flow (poll until X,
branch on state) or for many operations in one process. For one-off
inspection, `emerson ctl` is simpler (see the [emerson skill](../emerson/SKILL.md)).

It exists only in the server's image; don't `pip install` it on the host.
Write scripts in the project and run them from a directory containing them,
after `emerson load` and `emerson start`:

```sh
emerson run tools/check.py --verbose       # script args after the script
emerson run -p 8765 tools/live.py          # publish a port the script serves
emerson exec python3 -c "import emerson; help(emerson)"   # browse the API
```

`help()` on the module or any class is the authoritative reference. This file
is a map.

## Example

```python
import emerson, time

with emerson.attach_session() as machine:   # the session `emerson start` recorded
    tty = machine.subscribe_broker("tty0")  # subscribe before causing output
    machine.send_to_broker("tty0", b"help\r")
    out, end = b"", time.monotonic() + 2
    while time.monotonic() < end:           # read() returns whatever has arrived so far
        out += tty.read(timeout=0.2)
    tty.close()

    cpu = machine.get_device("/Hazard3Cpu") # the CPU path from `emerson ctl ls`
    machine.pause()                         # step() from paused
    machine.step(8000)
    machine.wait()                          # step() returns early, so always wait()
    print(cpu.get_register("pc").value, machine.tick_count)
    machine.go()
```

## API map

- **`emerson.attach_session(host=None, project=None, session=None)`** — a
  context manager that yields an attached `Machine`. Unset arguments come
  from `EMERSON_HOST`/`EMERSON_SESSION`/`EMERSON_PROJECT`, which
  `emerson run` sets. Helpers: `default_host()`, and
  `find_session(conn, project, session)` (raises `RuntimeError` naming what's
  missing).
- **`EmulatorController(host).connect()`** → **`Connection`** (a context
  manager): `run_project(name)` → token, `stop_project(token)`,
  `get_instance_list()` → `[(token, project)]`, `attach(token)` → `Machine`.
  One `attach` per `Connection` at a time.
- **`Machine`**:
  - Execution: `go()`, `pause()`, `reset()`, `step(n)` in **ticks** (not
    instructions), `wait()`, `go_to_counter(n)`, `step_back(n)` (needs
    `enable_checkpointing()`), `state`, `tick_count`.
  - Brokers: `subscribe_broker(name)` → `.read(timeout=)` / `.close()`
    (only bytes sent after subscribing), `send_to_broker`,
    `get_broker_history`, `list_brokers`.
  - Devices: `get_device(path)`, `get_device_tree_paths()`.
  - Snapshots: `save_snapshot_to_data_store`, `load_snapshot_from_data_store`,
    `get_current_snapshot`, `load_snapshot_from_bytes`.
  - Also `run_command_string(cmd, path=None)` and `log(msg)`.
- **`Device`**:
  - Registers and memory: `get_register(name).value`,
    `get_common_registers()`, `get_memory(addr, size)`, `set_memory(addr, data)`.
  - Device verbs: `invoke_action(verb, args="")`, `list_actions()`.
  - Debugpoints: `break_on_address`, `watch_*` and `stop_on_*`, with
    `enable_`/`disable_`/`delete_`/`get_` variants. They return `Debugpoint`
    objects (`id`, `hit_count`).
  - Also `get_disassembly` and the exception-halting controls.

## Gotchas

- **`step(n)` returns before the step finishes.** Call `wait()` before any
  read, or two reads land at different emulated times. Small steps hide this.
- **Stop sessions you create, in `finally`** (`conn.stop_project(token)`). A
  leaked session can stall a new one on the same project: it looks like it's
  running, but `pc` doesn't move. To clean up leaks, stop them once before a
  run. Never do it per test, and never stop sessions you didn't create.
- **For parallel tests, use one `Connection` and one session per test**
  (pytest-xdist workers satisfy this). Sessions of the same project run truly
  in parallel.
- **To read output as it arrives, use `subscribe_broker`.** `read_from_broker`
  can return `b""` for data that has already been written;
  `get_broker_history` is the reliable way to read what was written earlier.
- **`await_serial_event()`/`await_debugger_event()` need a running asyncio
  loop.** From a plain script, poll `subscribe_broker`/`state` instead.
- **A served port must bind `$EMERSON_BIND_HOST`** (`0.0.0.0` under
  `emerson run`), not `127.0.0.1`.
- **In tests, fake `emerson.attach_session` itself.** Patching
  `emerson.EmulatorController` doesn't affect it, so the test silently
  reaches the real server.
