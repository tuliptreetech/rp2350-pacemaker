---
name: Emerson Python Library
description: Script the Emerson emulator directly via its Python bindings (the `emerson` module installed inside the `emerson-server` container) instead of shelling out to individual `emerson ctl` commands. Use when the user wants a Python script or REPL against Emerson, mentions `import emerson`, `EmulatorController`, `Connection`, `Machine`, `Device`, or needs control flow (polling loops, conditionals, parsing broker/UART data) that a single `emerson ctl` call can't express.
---

# Emerson Python Library

`emerson-server` ships a Python package, `emerson`, that's a real binding to the
same emulator engine `emerson ctl` drives over HTTP — not just a wrapper around the
CLI. It exposes strictly more than `emerson ctl` does (e.g. checkpoint step-back,
`run_command_string`, blocking waits on debugger/serial events), and lets you
express loops/conditionals in one process instead of many `emerson ctl` invocations.

Requires the [emerson skill](../emerson/SKILL.md)'s prerequisites:
`emerson-server` running on a firmware image (`emerson load ./flash.bin`) and a
project session on it (`emerson start`).

## Getting a Python shell

The library only exists inside the container — there's nothing to `pip install`
on the host. Get to it with `emerson exec` (the host lifecycle tool's
convenience wrapper around `docker exec` into `emerson-server`, same container
`emerson ctl` itself execs into):

```sh
emerson exec                                        # interactive shell, then: python3
emerson exec python3 -c "import emerson; help(emerson)"   # one-liner
emerson exec python3 - < local_script.py                  # run a host-side script without copying it in (stdin is forwarded)
```

Equivalent raw `docker exec -it/-i emerson-server ...` forms still work, but
`emerson exec` is the supported entry point — prefer it.

## Discovering the API

`help(emerson)` (or `help()` on any class/method below) is authoritative and
always reflects the installed version — treat this doc as a map, not a
reference. Module lives at
`/usr/local/lib/python3.12/dist-packages/emerson/__init__.py` in the image.

## Key classes

- **`EmulatorController(host)`** — entry point, e.g.
  `EmulatorController("http://localhost:10314")`. `.connect()` → `Connection`
  (context manager); `.shutdown()`.
- **`Connection`** — `run_project(name)` / `stop_project(session_id)`,
  `project_list()`, `get_instance_list()` (running sessions as
  `(session_id, name)` pairs), `attach(session_id)` → `Machine` (context
  manager; auto-detaches on exit).
- **`Machine`** — the live emulator: `go()`, `step(n)` (ticks, not
  instructions; **returns before the step completes — pair it with `wait()`**),
  `wait()` (blocks until the machine is no longer running),
  `step_back(n)` (needs checkpointing), `pause()`, `reset()`,
  `state`/`is_paused()`/`is_running()`/`is_halted_on_error()`/`ignore_error()`,
  `tick_count`/`get_tick_counter()`, `go_to_counter(count)`,
  `get_device(path)` / `get_device_tree_paths()`, brokers
  (`read_from_broker`, `send_to_broker`, `get_broker_history`,
  `list_brokers`), snapshots (`save_snapshot_to_data_store`,
  `load_snapshot_from_data_store`, `list_snapshots`,
  `get_current_snapshot`/`load_snapshot_from_bytes`), checkpointing
  (`enable_checkpointing`/`disable_checkpointing`/`checkpointing_is_enabled`),
  `await_debugger_event()` / `await_serial_event()` (blocking waits),
  `run_command_string(command, path=None)`, `log(message, level=None)`.
- **`Device`** — one node in the device tree (from `get_device`): registers
  (`get_register`, `get_common_registers`, `registers`), memory
  (`get_memory(address, size)`, `set_memory(address, data)`),
  breakpoints/watchpoints/stoppoints (`break_on_address`,
  `watch_address_{read,write,fetch}`, `watch_register_{read,write}`,
  `stop_on_address_{read,write,fetch}`, `stop_on_register_{read,write}`, plus
  matching `enable_*`/`disable_*`/`delete_*`/`get_*`(s) accessors),
  disassembly (`get_disassembly`, `get_disassembly_with_pcode`), exceptions
  (`get_exception_names`, `get_any_raised_exceptions`,
  `enable_exception_halting`/`disable_exception_halting`),
  `translate_address(address)`, and **`invoke_action(action, args='')`** /
  `list_actions()` — the same custom board/peripheral verbs `emerson ctl action`
  exposes, callable directly.
- **`Debugpoint`** / **`DebugpointInfo`** — handles returned by the
  breakpoint/watchpoint/stoppoint creators above (`id`, `target`, `access`,
  `enabled`, `hit_count`).

## Example: attach to an already-running session and poke at state

```python
from emerson import EmulatorController

with EmulatorController("http://localhost:10314").connect() as conn:
    # find the session 'emerson start' created for your project
    session_id = next(sid for sid, name in conn.get_instance_list())
    with conn.attach(session_id) as machine:
        print(machine.state, machine.tick_count)

        gpioa = machine.get_device("/MEM/gpioa")
        print(gpioa.get_common_registers())

        # Walk forward in fixed increments, sampling as you go. step() returns
        # while the machine is still executing, so wait() before every read --
        # without it the two get_register calls below land at different points
        # in emulated time and their relationship is meaningless.
        tim1 = machine.get_device("/MEM/tim1")
        tim3 = machine.get_device("/MEM/tim3")
        for _ in range(40):
            machine.step(8000)
            machine.wait()
            cnt1 = int(tim1.get_register("CNT").value)
            cnt3 = int(tim3.get_register("CNT").value)
            print(cnt1, cnt3)

        print(machine.read_from_broker if False else machine.get_broker_history("tty0"))
```

## When to reach for this vs. `emerson ctl`

Use `emerson ctl` for one-off inspection and simple scripts — it's less ceremony and
already non-interactive/agent-friendly (see the [emerson skill](../emerson/SKILL.md)).
Reach for the Python API when you need:

- Control flow — polling a register/broker in a loop until a condition holds,
  branching on emulator state, retry logic.
- Many reads/actions combined in one process instead of N separate `docker exec`
  round-trips.
- Capabilities `emerson ctl` doesn't surface at all: `step_back`, `run_command_string`,
  `await_debugger_event`/`await_serial_event` (see Gotchas), direct snapshot
  bytes (`get_current_snapshot`/`load_snapshot_from_bytes`).
- Parsing broker/UART output programmatically rather than eyeballing raw bytes
  from `emerson ctl broker <name>` (see Gotchas for which broker method to use).

## Running tests in parallel

The server supports many concurrent sessions of the *same* project, and
they run genuinely in parallel (not just concurrently-scheduled on one
core) — measured on `stm32f030r8`: one session doing a fixed amount
of `step()` work took ~33s; four of those sessions run at once, each in its
own process, took ~35s total, not ~130s. This is the basis for running a
test suite's tests concurrently, one emulator session per test, instead of
booting a fresh session per test *sequentially*.

Two things make this work correctly:

- **Give each concurrent session its own `Connection`.** A single
  `Connection` can only `attach()` to one session at a time — a second
  `attach()` on the same `Connection` while another is active raises
  `RuntimeError: Usage Error: Invalid Connection State`. This is naturally
  satisfied by `pytest-xdist` (`pytest -n N`): each worker is a separate
  process, so each ends up with its own `EmulatorController(...).connect()`
  call and its own `Connection`, as long as your fixture creates the
  connection itself rather than sharing one across the whole test run.

  ```python
  # conftest.py -- one Connection, one session, per test
  @pytest.fixture
  def machine():
      with EmulatorController(host).connect() as conn:
          session_id = conn.run_project(project)
          try:
              with conn.attach(session_id) as m:
                  yield m
          finally:
              conn.stop_project(session_id)
  ```

- **Move leaked-session cleanup out of the per-test fixture.** The naive
  fixture in the gotcha below (`for session_id, _ in
  conn.get_instance_list(): conn.stop_project(session_id)` before starting
  a new one) is correct for a single sequential run, but wrong once tests
  run concurrently: one worker's setup would stop every *other* worker's
  in-flight session too. Do that cleanup exactly once, before the
  (parallel) run starts — e.g. as a separate step in whatever script
  invokes `pytest`, not inside the fixture itself.

- **Bound worker count to CPUs actually allocated to the container**, not
  `os.cpu_count()` — that reports the *host's* core count regardless of any
  `docker run --cpus=N` limit, since Python doesn't consult the cgroup
  quota. Read it directly instead:

  ```python
  def allocated_cpus():
      try:
          with open("/sys/fs/cgroup/cpu.max") as f:      # cgroup v2
              quota, period = f.read().split()
          if quota != "max":
              return max(1, int(quota) // int(period))
      except FileNotFoundError:
          pass
      try:
          with open("/sys/fs/cgroup/cpu/cpu.cfs_quota_us") as f:  # cgroup v1
              quota = int(f.read())
          with open("/sys/fs/cgroup/cpu/cpu.cfs_period_us") as f:
              period = int(f.read())
          if quota > 0:
              return max(1, quota // period)
      except FileNotFoundError:
          pass
      return os.cpu_count() or 1  # unrestricted -- fall back to host count
  ```

See this project's `tests/emerson/conftest.py` and
`scripts/run_emerson_tests.sh` for a complete working example: `pytest-xdist`
installed on demand, leaked sessions cleared once up front, worker count
detected from the container's cgroup, then `pytest -n <N>`.

## Gotchas

- The package is only inside the container's image — don't try to `pip install
  emerson` or import it on the host.
- `Connection.attach(session_id)` needs a session that already exists (same
  precondition as `emerson ctl` needing `emerson load` + `emerson start`
  first); find its id via `conn.get_instance_list()` rather than guessing.
- `Machine.step(n)` steps **ticks**, not instructions — that's a different
  unit than `emerson ctl step [n]` (instructions). Don't assume parity between the
  two.
- **`Machine.step(n)` returns before the step has finished.** It dispatches the
  command and returns while the emulator is still executing, and the overrun
  scales with `n`. Call `machine.wait()` after every `step()` before reading
  anything:

  ```python
  machine.step(8000)
  machine.wait()          # without this the reads below are racy
  value = int(dev.get_register("CNT").value)
  ```

  Small steps hide it — they finish faster than the next HTTP round trip, so a
  script written against `step(500)` behaves perfectly and the same script at
  `step(8000)` is quietly wrong. There is no error and nothing in the return
  value to indicate it. Measured on `stm32f030r8`, reading the *same*
  register three times immediately after `step()`:

  ```
  step=500 :  32434  32434  32434   spread=0
  step=8000:  40934  44434  44934   spread=4000
  ```

  The damage is worst when a single sample reads **two or more** locations and
  compares them: each read is taken at a different point in emulated time, so
  reversing the read order flips the sign of the difference between two
  counters. That is enough to make two perfectly synchronised counters look
  independent — it has already produced one spurious emulator bug report.
  Tracked as [emerson-issues#11](https://github.com/tuliptreetech/emerson-issues/issues/11).

  `emerson ctl step` has the same behaviour, with no `wait` subcommand — poll
  `emerson ctl state` until `paused` instead (see the [emerson skill](../emerson/SKILL.md)).
- `step_back` requires checkpointing enabled (`enable_checkpointing()`) —
  same requirement as `emerson ctl checkpoint enable`.
- **A session leaked by a crashed script (no `stop_project`) stalls a new one
  on the same project.** The new session still reports `state: Running` and a
  normally-climbing `tick_count`, but the machine makes no real progress
  (e.g. `pc` stays pinned) — no error is raised anywhere. Always clear
  existing sessions before starting, and stop your own in `finally`. **Caveat
  when running multiple sessions concurrently (see "Running tests in
  parallel" above): "clear existing sessions" must happen once, before any
  of them start** — doing it inside a per-test fixture would stop sibling
  sessions that are legitimately still in flight, not just leaked ones.

  ```python
  with EmulatorController(host).connect() as conn:
      for session_id, _ in conn.get_instance_list():
          conn.stop_project(session_id)
      session_id = conn.run_project(project)
      try:
          with conn.attach(session_id) as machine:
              ...
      finally:
          conn.stop_project(session_id)
  ```
- **Prefer `get_broker_history` over `read_from_broker`.** The latter has
  been observed returning `b""` for data that was in fact already written and
  that `get_broker_history` returned correctly moments later. Use
  `get_broker_history` for anything that needs to reliably see broker output.
- **`await_serial_event()`/`await_debugger_event()` need a running asyncio
  event loop** despite their plain synchronous signature — calling them from
  an ordinary script raises `RuntimeError: no running event loop` (often
  reported against an unrelated later line). For a bounded wait on broker
  output, poll instead:

  ```python
  import time
  deadline = time.monotonic() + timeout_s
  data = b""
  while time.monotonic() < deadline and not data:
      data = bytes(machine.get_broker_history(broker_name))
      time.sleep(0.5)
  ```
