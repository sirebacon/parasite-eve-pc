# Rust development tools

Initial migration batches from the Python debug harness. This workspace lives in the
game repository so its protocol can be reviewed with the native implementation.
The Python companion harness remains the baseline during migration.

## Build and test

From this directory, with a working Rust toolchain and platform linker:

```powershell
cargo build --workspace --locked --offline
cargo test --workspace --locked --offline
cargo fmt --all -- --check
cargo clippy --workspace --all-targets --locked --offline -- -D warnings
```

JSON parsing uses pinned `serde_json` 1.0.154 (MIT OR Apache-2.0) with dependencies
recorded in `Cargo.lock`. PNG encoding uses pinned pure-Rust `png` 0.18.1
(MIT OR Apache-2.0). On a fresh machine first run `cargo fetch --locked`, then
the offline commands above. The declared
minimum Rust version is 1.74; current stable is the CI target. CI checks Windows,
Linux and macOS. Local validation initially uses Windows gnullvm; on this machine
select it with `cargo +stable-x86_64-pc-windows-gnullvm ...` because the default GNU
toolchain's linker cannot find its GCC runtime libraries. Add the Cargo bin folder
to PATH if needed. No game data or Python installation is needed for these tests.

## Attach to a running game

Start the game with `PE_DEBUG_CONTROL` set to an existing control folder. While
the game is advancing, run:

```powershell
cargo run --locked --offline -p pe-tools -- command --control-dir 'D:\pe-run\ctl' -- ping
cargo run --locked --offline -p pe-tools -- command --control-dir 'D:\pe-run\ctl' -- battle-state
cargo run --locked --offline -p pe-tools -- command --control-dir 'D:\pe-run\ctl' -- ini-set 'Manage mods/All mods' 0
```

The client also accepts `PE_DEBUG_CONTROL` as the directory default. The command
after `--` is passed to the native channel, so all native commands are accessible.
Success prints the original `ok` reply. Exit codes: 0 success, 1 transport/native
error, 2 invalid CLI arguments or RNG snapshot. Default wall-clock timeout: 30s;
override with `--timeout-ms N` before `--`.

The attached client cannot advance a game paused on stdin at `--control-at`.
For a Rust-owned process, use `run` below or `GameSession::command`, which advances
in the Python harness's 10-frame increments and applies a separate response deadline.

## Launch an isolated headless game

```powershell
cargo run --locked --offline -p pe-tools -- run --exe 'D:\build\parasite-eve-pc.exe' --disc 'D:\discs\disc1.cue' --disc2 'D:\discs\disc2.cue' --run-dir 'D:\runs\new-run' --advance 30
```

The parent of `--run-dir` must exist; the run directory itself must be new. Existing
directories are refused and never deleted. `--exe` also accepts the `PE_DEBUG_EXE`
environment default. Options include `--button cross` with `--advance`,
`--command 'room'`, `--frame-limit N`, `--shot 'boot'`, and
`--response-timeout-ms N` (default 60000).
The CLI prints initial/final control JSON, plus an optional native command reply,
then quits. Debug commands require the native polling hook to be running; do not
assume a boot-time `ping` proves readiness. The current local build also timed out
on early `ping` through the original Python harness.

`LaunchOptions` additionally supports copied cards in either port, associated
`.pe-ext` companions, custom mod INI content and relative extra files. Copies are
isolated; companion contents/checksums are not yet validated by Rust. The session
creates `bu00`, `bu10`, `ctl`, `pe_mods.ini`, `stdout.log`, `stderr.txt`, and native
frame/log output. It preserves unrecognized JSON fields, keeps the last 2000 states,
and bounds stdout lines (1 MiB), queued lines (64), and wall-clock response time.

Library operations include `advance`, `press`, `wait_until`, `wait_stable`,
`command`, and `quit`. `Drop` attempts graceful shutdown, then kills and reaps the
child after a deadline; read/write failures during advance terminate it immediately.
`shot(label)` saves numbered PNG evidence from `frame.ppm`; a failed `wait_until`
also attempts a screenshot without masking its primary timeout error. Portable
save export and Ctrl+C/signal-specific process cleanup are not implemented yet. Generic
waits cap the final step at the remaining frame budget, unlike Python's overshoot.

## Battle helpers and trace inspection

`pe-session::battle::BattleClient::headless(&mut session)` implements every helper
from Python's `pedebug/battle.py`. `BattleClient::attached(control_dir, timeout)`
uses the same interface for an already advancing game.

| Python helper | Rust method / result |
|---|---|
| `state` | `state()`: typed `BattleState`, with the original JSON in `raw` |
| `rng_get` / `rng_set` | Complete typed `RngSnapshot`; set validates before sending |
| `trace` / `events` | Enable/disable tracing; parse `battle.jsonl` into typed events |
| `give` / `equip` | Uppercase hex item IDs and decimal bag slots; native errors retained |
| `set_stat` | `Stat::{Level, Hp, PeRaw, Bp}` with native decimal values |
| `mod_state` | Typed registered mod rows, including inactive values |
| `ini_set` | Original reply plus `restart_required`; no automatic mod restart |

Snapshots distinguish `TargetListIndex` from `FieldActorIndex`; a field actor
number is not a battle target index. PE values retain their raw 16.16 integers.
Trace events separately represent admissions, commits, observations, reward offers
and steal transfers. Unknown event names and all extra JSON fields remain in `raw`.
Malformed known fields and incomplete/corrupt trace lines fail with field/line
context. A missing trace file returns an empty list, matching Python's helper.

The source-only fixtures under `pe-protocol/fixtures/` contain synthetic data.
For offline inspection:

```powershell
cargo run --locked --offline -p pe-tools -- battle-validate pe-protocol/fixtures/battle-state.json
cargo run --locked --offline -p pe-tools -- events pe-protocol/fixtures/battle-events.jsonl
cargo run --locked --offline -p pe-tools -- ppm-to-png 'D:\runs\new-run\frame.ppm' 'D:\runs\new-run\frame.png'
```

PNG conversion supports binary P6 RGB with max value 255, including comments and
common whitespace/CRLF headers. It checks dimensions, the exact raster length and
a 64-megapixel limit. Existing PNG output files are refused. Missing frames fail
explicitly rather than returning a path to a nonexistent screenshot. Pixel content
matches the Python converter; compressed PNG bytes need not match.

## Components and contracts

| Crate | Implemented |
|---|---|
| `pe-protocol` | Commands/replies, control/battle/mod JSON, distinct trace event types, native pad bits, complete RNG snapshots |
| `pe-session` | File/process transport, battle helpers, PNG screenshots, card/companion copies, input/frame waits and shutdown |
| `pe-tools` | Attached command CLI, isolated `run`, battle/trace validation, PNG conversion, RNG validation |

`rng-validate "STATE FRAME INDEX HEX1042"` prints a normalized snapshot without
changing a game. It retains unsigned 32-bit generator/frame values, index -1..520
and all 521 bytes. Restoring it still requires the same battle phase/input schedule.

Only one client may own a channel. The native format has no request IDs. Rust
clients cooperate through `client.lock` and refuse an existing `cmd`; Python clients
do not know this lock and must not run concurrently. A timeout leaves the pending
command intact because it may still execute. After a crashed client, inspect the
game/channel before manually removing a stale `client.lock`; retrying mutations
can apply them twice. Use a fresh control directory for a new game session.

Save-format/companion validation, save export, reusable menu/field/battle flows and
scenario runners remain on the migration checklist. Live battle/RNG acceptance and
Python caller cutover are still pending; no Python entry points have been retired.
