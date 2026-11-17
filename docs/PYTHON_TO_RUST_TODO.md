# Python-to-Rust Migration TODO

Date: November 16, 2026
Status: In progress; core session/battle helpers, screenshots and validated save envelopes/export implemented; broader live acceptance pending
Goal: Replace the project's first-party Python development and testing tools with maintainable Rust equivalents while preserving their behavior.

## Scope and inventory

| Source checkout | Snapshot commit | Python files | Lines, including comments and blank lines | Priority |
|---|---|---:|---:|---|
| `pe-headless-harness` | `f8095f7` | 54 | 7,164 | First migration |
| `pe-headless-debug` | `6e78ef7d6` | 178 | 36,900 | Later migrations by tool family |
| Total | November 16 inventory | 232 | 44,064 | Phased conversion |

These counts cover tracked `.py` files in these two checkouts. They exclude third-party submodule contents, generated builds, saved games, disc images, and other project directories such as the Combat Simulation Toolkit. Recount on the target branch before implementation because upstream and PR changes may alter this snapshot.

The conversion target is the Python tooling. The C game runtime and modular in-game debug implementation remain the native integration boundary. The `pe-protocol`, `pe-save`, `pe-session` and `pe-tools` crates now exist under `pe-headless-debug/devtools/`. Other components and suite commands below remain planned.

## Current progress: command/process control, battle helpers and screenshots

Owner: Codex. Started November 16, 2026. Rust implementation lives in the game
repository's `devtools/` workspace. Keep the companion Python harness as the
comparison baseline; no Python files have been removed or fully retired.
Local branch: `feat/rust-debug-tools`, based on the existing headless-debug branch
at `80dbf1093c3a26e947c948320528a41735f02496`. No migration PR has been published.

| Item | Status | Replacement / evidence |
|---|---|---|
| Refresh local inventory | Done | Harness `f8095f7b85c5b0f5a8536d256f5a43698dd8c32d`: 54 Python files; game `80dbf1093c3a26e947c948320528a41735f02496`: 178; unchanged total 232 |
| Choose initial Rust location | Done | `devtools/` in the game repository; baseline harness is at `D:/Recomp Games/_old/pe-headless-harness` on this machine |
| Create initial Cargo workspace | Done | `pe-protocol`, `pe-session`, `pe-tools`; locked dependencies, offline build after dependency fetch |
| Port `Session.command` transport | Implemented; live-game acceptance pending | `pe-session::DebugControl::{ask, ask_with_frames}`; atomic file publish, exact replies, errors, 10-frame callback schedule, timeouts |
| Parse command/reply and full RNG snapshot | Implemented; live RNG restore pending | `pe-protocol::{Command, Reply, RngSnapshot}`; all 521 bytes, u32 state/frame, index -1..520 |
| Provide attached client CLI | Done for this slice | `pe-tools command --control-dir DIR -- COMMAND`; `pe-tools rng-validate SNAPSHOT`; help/version and exit codes |
| Add automated source checks | Implemented; remote CI pending | `.github/workflows/rust-devtools.yml`: format, Clippy, tests on Windows/Linux/macOS |
| Source-only validation | Passed locally | 71 Rust tests, earlier Python checks and binary/helper parity; one ignored entry is the subprocess test fixture invoked by the process tests |
| Port process launch/control JSON/cleanup | Implemented; broader live acceptance pending | `pe-session::process::{LaunchOptions, GameSession}`, `pe-protocol::control`; real boot + 30-frame Python/Rust comparison passed |
| Port typed battle state/events and helpers | Implemented; live battle/caller cutover pending | All 10 `pedebug/battle.py` helpers map to `pe-session::battle::BattleClient`; typed models in `pe-protocol::battle` |
| Port screenshot capture | Implemented; pixel parity passed | `pe-session::image::ppm_to_png`, `GameSession::shot`; synthetic and live 320x240 output matches Python RGB pixels |
| Port save export, card listing and logs | Implemented; live save acceptance/caller cutover pending | `GameSession::{mods_tail,save_block,save_files,save_to,card_files,mods_log,play_log}` plus extra native arguments |
| Port generic save envelopes | Implemented; field/provider decoding remains | `pe-save`: game CRC, PEMC records/compatibility window, PEEX identity and checksum layers; 11 Python binary/helper comparison cases passed |
| Port reusable flows and scenarios | Next | `pedebug/flows.py`, `places.py` and scenario runners |

Use `devtools/README.md` for real commands and the channel concurrency/timeout
contract. This batch does not close any complete Python-file migration item.

Progress estimate after the first three batches was approximately 2–3% of overall
effort. This is a rough estimate, not a checklist-derived percentage. Formal
Python-file retirement is still 0/232 because live acceptance/caller cutover is
pending. Track implemented capabilities and validation separately from retirement.

## Completion rules

- [ ] Give each migration item an owner, status, replacement path/command, and validation evidence.
- [ ] Preserve the capability of every inventoried file. Several Python modules may become one Rust module or command; a one-to-one file translation is not required.
- [ ] Keep the Python implementation available as a temporary comparison baseline until the corresponding Rust replacement passes its acceptance checks.
- [ ] Remove a Python entry point only after its callers, documentation, build scripts and CI use the verified Rust replacement.
- [ ] If a historical script is obsolete, document its callers and replacement before explicitly retiring it. Do not count an unexplained deletion as a conversion.
- [ ] Keep user saves, `.pe-ext` companions, disc images, extracted copyrighted assets, credentials and generated run output out of source commits.
- [ ] Separate migration work from gameplay changes so a parity failure can be investigated without also changing battle rules.

## Phase 0: Establish the target and freeze the baseline

- [x] Count and list the current 54 harness and 178 game-repository Python files.
- [ ] Refresh this inventory against the target upstream branch and record the new commit IDs.
- [x] Confirm the initial repository location: the Rust harness lives in the game repository under `devtools/`; the Python harness remains a separate baseline checkout.
- [x] Integrate the Rust tooling into the game repository; initial migration owner is Codex. This does not change the contents of an already published PR.
- [ ] Identify active commands, imported helper modules, historical one-off scripts, generated files and GUI tools.
- [ ] Record each tool's arguments, environment variables, default paths, output files, exit codes and platform requirements.
- [ ] Capture baseline results for the five routine harness suites: `mod_saves`, `mods_on_off`, `new_game`, `places_check` and `debug_battle`.
- [ ] Capture the separate real-combat check and the existing harness unit tests.
- [ ] Record the game executable revision, mod configuration, fixture identities and input/frame schedule for each baseline.
- [ ] Choose supported platforms, a minimum Rust version, dependency policy and reproducible build/release approach. Start with the current Windows workflow.

Acceptance: Every Python capability has a disposition and baseline; the Rust workspace location and platform policy are recorded.

## Phase 1: Create a modular Rust workspace

Suggested layout to confirm during Phase 0:

| Proposed component | Responsibility |
|---|---|
| `pe-protocol` | Control JSON, debug command/reply parsing, battle event types |
| `pe-session` | Game process lifecycle, isolated run directories, input and timeouts |
| `pe-save` | Card blocks, CRCs, mod records and companion files |
| `pe-disc` | Read-only disc and room data parsing |
| `pe-text` | Script/text/font decoding and localization support |
| `pe-scenarios` | Reusable menu/field/battle flows and scenario implementations |
| `pe-assets` | Shared image/asset processing used by later tool migrations |
| `pe-tools` | User-facing CLI, suite selection and reports |

- [x] Create the initial Cargo workspace with separate protocol, transport and CLI crates.
- [ ] Complete the remaining components and stabilize their public interfaces after process/scenario integration.
- [ ] Keep transport, binary parsing, scenario logic and reporting separate; avoid recreating one large driver file.
- [ ] Select and pin required libraries after checking their supported platforms, licenses and functionality.
- [ ] Define typed errors that retain command, game state, frame, file and process-exit context.
- [ ] Provide portable configuration for executable paths, both discs, fixtures, mod options and output directories.
- [x] Preserve `--exe` and `PE_DEBUG_EXE` compatibility in `pe-tools run`.
- [ ] Provide help/version output, structured logging and deterministic report paths.
- [x] Add formatting, linting and unit-test checks to CI for the initial workspace; remote execution pending.

Acceptance: The Rust CLI builds and displays help on the supported platforms without needing game assets.

## Phase 2: Port process control and debug protocol

Primary sources: `pedebug/game.py`, `pedebug/build.py`, `pedebug/battle.py`.

- [x] Launch the actual game with equivalent headless, control, frame-dump and pad-event arguments (live launch/advance/quit verified).
- [x] Create an isolated per-session config/card/log directory and preserve the original user files (copy/isolation tested; companion validation still pending).
- [x] Read stdout continuously, recognize control/frame-limit JSON, and retain a bounded state history.
- [x] Separate game-frame deadlines from wall-clock deadlines; retain a default 60-second process-response deadline, including under chatty stdout.
- [x] Implement graceful quit, forced termination after a deadline, and process/pipe cleanup for ordinary failures.
- [ ] Add explicit Ctrl+C/signal interruption handling and validate child cleanup on supported platforms.
- [x] Match native button masks and hold/release frame scheduling exactly (mock-process checks).
- [x] Port advance, press, wait, wait-until-stable and readable state summaries.
- [x] Port PNG screenshot capture and timeout evidence; synthetic and live-frame pixel parity verified against Python.
- [x] Write `cmd.tmp`, rename atomically to `cmd`, and consume replies from `out` without matching a command-name prefix accidentally (source-only checks; live command readiness pending).
- [x] Reject empty or multi-line command requests and preserve actionable `err` responses (`pe-protocol`, `pe-session`; source-only tests).
- [ ] Support all currently implemented debug commands, including room/actor/script queries, positioning, variables, save export, setup, settings, RNG and tracing.
- [x] Preserve the distinction between native target-list indexes and field actor indexes with separate Rust types.
- [x] Parse battle snapshots and JSONL event streams with explicit types and forward-compatible handling of added fields (source-only parity; live battle suites pending).
- [ ] Preserve full native RNG snapshots: generator state, frame seed, cursor and all 521 state bytes. Test restoration without consuming a random roll.
- [x] Keep HP-loss admissions, committed enemy HP changes, observed player HP changes, reward offers and actual steal results distinct in typed trace events.
- [x] Respect the reported restart requirement for stored mod enable/disable settings; return it to callers without hot-starting mods.

Acceptance: Rust can boot the same executable, load the same fixture, send valid/invalid commands, reproduce the same frame/input schedule and produce equivalent structured observations.

## Phase 3: Port save, disc, script and text formats

Primary sources: `pedebug/card.py`, `disc.py`, `script.py`, `gametext.py`, `jatext.py`, `font.py` and root inspection/extraction scripts.

- [ ] Define explicit byte order, bounds checks, integer widths and wrapping behavior for binary formats.
- [x] Port the game CRC and generic card/PEMC/compatibility-window envelopes, including legacy-block discovery; opaque payloads preserved.
- [ ] Complete base-game field/title decoding, legacy/provider-specific payload decoding and supported field edits from `pedebug/card.py`.
- [x] Preserve unknown records/payloads byte-for-byte in supported save-bundle copies; a 600-byte unknown record crossing the compatibility window matches Python.
- [x] Port companion identity/path resolution, outer/internal/section checksum validation and generic section extraction (provider payloads remain opaque).
- [x] Export/copy cards with required companions after reference/checksum validation; source-only command/process parity verified, live save acceptance pending.
- [ ] Compare save round trips by persisted record/section payloads while allowing transport generation IDs to change.
- [ ] Preserve current native refusal expectations for damaged and unsupported containers.
- [ ] Port read-only CUE/raw-sector/ISO file access, PE.IMG tables, room data and resident data access.
- [ ] Port self-describing room script decoding, operand modes and incomplete-decode diagnostics.
- [ ] Port glyph/text decoding and encoding with exact binary round-trip checks, including control tags and supported Japanese text.
- [ ] Port font metrics and relevant image outputs; compare pixels or documented tolerances where visual output matters.
- [ ] Replace `pecard.py`, `extract_text.py`, `build_glyph_table.py` and `explore.py` entry points with documented Rust commands.
- [ ] Investigate Windows OCR and other OS-specific dependencies before claiming a fully Python-free toolchain.
- [ ] Add small synthetic binary fixtures and permitted local-fixture hooks; do not commit game disc content.

Acceptance: Golden binary round trips, CRC/companion corruption tests, script/text round trips and relevant image comparisons pass.

## Phase 4: Port reusable flows and all 24 harness scenarios

Primary sources: `pedebug/flows.py`, `pedebug/places.py`, `scenarios/`.

- [ ] Port boot/title, Continue, slot selection, successful load and expected load refusal.
- [ ] Port free-field checks, soft reset, New Game, phone discovery, dialogue interaction and real phone saving.
- [ ] Port calibrated movement, walking to an actor and talking through the real menus.
- [ ] Port room warp, actor/script lookup, placement, variable access, scenario capture and scenario reopening.
- [ ] Port attack targeting, native action-queue completion, range/state refusals and bounded auto-battle policies.
- [ ] Port mod save migration, mod on/off, New Game reset, edge cases and stack save scenarios.
- [ ] Port economy/payout/menu/provider/settlement and Wayne hand-in scenarios.
- [ ] Port phone, localization, recovery and workshop scenarios, retaining their fixture/pack/build requirements.
- [ ] Preserve expected failure paths and supporting logs/screenshots, not just success cases.
- [ ] Mark asset-dependent cases explicitly as skipped when prerequisites are unavailable; never report a missing prerequisite as a pass.
- [ ] Run Python and Rust scenarios against equivalent copied fixtures and compare semantic outcomes, command sequences and meaningful timings.
- [ ] Compare wall-clock timeout behavior and ensure failed suites do not leave orphaned game processes.

Acceptance: Every scenario in Appendix A has a verified Rust replacement. The five routine suites and separate real-combat check pass through Rust.

## Phase 5: Replace runner, helper commands and CI entry points

Primary sources: `run_all.py`, root scripts, `tools/`, `tests/test_debug_helpers.py`.

- [ ] Port suite listing/selection, independent result collection, Markdown/JSON reports, logs and aggregate exit status.
- [ ] Port the 13 helper tools, including scenario capture, save/room inspection, phone checks, recovery and API checks.
- [ ] Preserve or replace helper integrations that build C fixtures; the C fixture itself need not be rewritten merely to replace its Python launcher.
- [ ] Replace Python unit tests with Rust tests covering equivalent contracts, including exact command matching and companion corruption detection.
- [ ] Add configurable, relocatable executable/disc/fixture discovery and remove assumptions about one user's drive layout.
- [ ] Publish tested commands such as the proposed `pe-tools test all`, `test list`, `scenario capture` and `card show` command families.
- [ ] Update all harness documentation and callers to the actual Rust commands implemented.
- [ ] Audit and remove Python imports/invocations from the supported harness path after parity is proven.
- [ ] Record remaining optional external tools and platform requirements, including OCR or image encoders.
- [ ] Package a Windows executable and verify it from a directory that has no Python installation available to the process.
- [ ] Add supported-platform CI builds and appropriate source-only versus local-asset integration jobs.

Acceptance: The supported headless harness works end to end without a Python runtime, and all 54 Python source-file dispositions are closed.

## Phase 6: Port the game repository's 18 Python regression drivers

- [ ] Map each `tests/*.py` driver to its existing C/native test targets, assets, invocation and expected outputs.
- [ ] Port font/text, interpolation, mouse/input and display-selection driver logic.
- [ ] Port HD pack/background/live-apply/download regression drivers without weakening negative-path checks.
- [ ] Port Japanese-release, room replay/crash, sound-catalog, Windows-import and costume-style live checks.
- [ ] Compare existing outputs and exit codes, including intentionally unavailable asset cases.
- [ ] Replace Python invocations in CMake and CI only as each corresponding Rust driver passes.

Acceptance: All 18 current regression drivers have verified Rust equivalents and updated callers.

## Phase 7: Port the 78 localization Python files

- [ ] Inventory package generation, translation checking/import, text/glyph processing, artwork and language-specific workflows.
- [ ] Port shared localization data models and validators before individual entry points.
- [ ] Preserve pack formats, control sequences, binary output, Unicode behavior and language-specific layout rules.
- [ ] Port the existing localization tests; keep valid and invalid input coverage equivalent.
- [ ] Decide and implement the Rust replacement for translator GUI capabilities, including artwork, glyph/letter, menu, battle and in-game previews.
- [ ] Preserve user-facing project/package workflows; a CLI-only replacement does not close GUI features.
- [ ] Verify rendering/layout outputs with recorded comparison criteria.
- [ ] Replace localization build/packaging/CI Python dependencies and document command/UI changes.

Acceptance: All localization capabilities and tests in Appendix B are covered, with usable replacements for the existing GUI workflows.

## Phase 8: Port asset pipelines and lighting-editor Python tools

- [ ] Review the 75 `docs/assets/**/*.py` files for active pipelines versus historical experiments/corrections.
- [ ] Port shared image conversion, composition, registration, pack generation and audit primitives once.
- [ ] Port active static, animated, Day 1, widescreen, title and background production workflows.
- [ ] Preserve provenance, alignment/registration, temporal consistency, correction and asset-validation outputs.
- [ ] Record an explicit port or approved retirement decision for every historical proof-of-concept and one-off correction script.
- [ ] Port all seven `tools/lighting-editor/*.py` capabilities: assets, model capture, local serving, desktop packaging/archive and tests.
- [ ] Preserve integrations with the existing editor/frontend and any external image/encoding programs.
- [ ] Verify output filenames, schemas, pack compatibility and visual results using the same source assets.
- [ ] Update asset/editor documentation, packaging scripts and CI callers.

Acceptance: Every asset/editor source in Appendix B has a verified Rust replacement or a documented, agreed retirement with equivalent capability retained elsewhere.

## Phase 9: Remove Python from the agreed first-party scope

- [ ] Recount tracked `.py` files in both target repositories and reconcile each remaining file with this inventory.
- [ ] Search build, CI, launcher, packaging, README and developer instructions for Python executable names and `.py` entry points.
- [ ] Verify that removal does not discard fixtures, reference behavior, provenance or test coverage.
- [ ] Remove superseded first-party Python source only after its replacement has passed its acceptance gate.
- [ ] Confirm Rust unit/source-only checks can run without proprietary game assets.
- [ ] Confirm configured integration suites work with the user's local assets and copied save/companion fixtures.
- [ ] Run the full supported test matrix on the target merge base and retain a report tied to the final commits.
- [ ] Confirm a fresh supported-machine setup can build/install and run the tools without Python.
- [ ] Keep any permitted third-party Python dependencies explicitly documented; do not claim a completely Python-free toolchain while they remain required.

Final acceptance: No first-party Python runtime is required for the agreed supported workflows. Every file below is accounted for, all migrated capabilities pass, and the documentation/CI/release artifacts use Rust.

## Suggested PR sequence

1. Inventory, format fixtures, compatibility contract and Rust workspace.
2. Session/process control, typed debug protocol, RNG and battle observations.
3. Save/companion, disc/script/text libraries and inspection commands.
4. Shared flows, scenario parity, runner and harness cutover.
5. Native regression drivers.
6. Localization core/tests, then GUI/workflow replacement.
7. Asset pipelines and lighting-editor helpers, grouped by capability.
8. Python removal and final clean-machine validation.

Keep each migration PR reviewable and attach its parity evidence. Do not bundle the entire 232-file replacement into one unverified switch.

## Related work that Rust conversion does not automatically complete

- [ ] Implement a validated chosen-encounter loader for `battle N` as separate native/debug work; `Battle_StartEncounter` accepts a mode, not a group ID.
- [ ] Add cast, use-item and escape flows in the new harness after the existing behavior is ported and tested.
- [ ] Add story/movie/dialogue skip support with the necessary native integration.
- [ ] Add challenge-specific trace/state providers and distinguish their settlements from native reward offers.
- [ ] Track raw memory access, Junk setup and equipment-mod setup as explicit feature tasks if still required.

These are feature gaps in the current implementation, not features gained simply by translating the external harness to Rust.

## Appendix A: Headless harness file checklist (54)

Source paths below are relative to `pe-headless-harness/`. For each checkbox, record the replacement module/command and parity evidence before marking it complete.

### Root commands (5)

- [ ] `build_glyph_table.py`
- [ ] `explore.py`
- [ ] `extract_text.py`
- [ ] `pecard.py`
- [ ] `run_all.py`

### Reusable harness modules (11)

- [ ] `pedebug/battle.py` — all 10 helpers implemented in `pe-session::battle`; fixture/mock-process parity passed; live battle acceptance and caller cutover pending.
- [ ] `pedebug/build.py`
- [ ] `pedebug/card.py`: CRC/container/legacy-discovery/window/companion envelopes implemented in `pe-save`; field/provider decoding, full command coverage and cutover pending.
- [ ] `pedebug/disc.py`
- [ ] `pedebug/flows.py`
- [ ] `pedebug/font.py`
- [ ] `pedebug/game.py`: core process, frame/input, screenshot, export, card/log helpers implemented; full flow integration, live fixture acceptance and cutover pending.
- [ ] `pedebug/gametext.py`
- [ ] `pedebug/jatext.py`
- [ ] `pedebug/places.py`
- [ ] `pedebug/script.py`

### Scenario checks (24)

- [ ] `scenarios/combat_check.py`
- [ ] `scenarios/debug_battle.py`
- [ ] `scenarios/economy_held_payout.py`
- [ ] `scenarios/economy_menu.py`
- [ ] `scenarios/economy_menu_provider.py`
- [ ] `scenarios/economy_path.py`
- [ ] `scenarios/economy_paused.py`
- [ ] `scenarios/economy_settlement.py`
- [ ] `scenarios/economy_step5_remaining.py`
- [ ] `scenarios/edge_cases.py`
- [ ] `scenarios/lang_items.py`
- [ ] `scenarios/lang_menus.py`
- [ ] `scenarios/lang_steal.py`
- [ ] `scenarios/lang_suite.py`
- [ ] `scenarios/mod_saves.py`
- [ ] `scenarios/mods_on_off.py`
- [ ] `scenarios/new_game.py`
- [ ] `scenarios/phone_calls.py`
- [ ] `scenarios/places_check.py`
- [ ] `scenarios/recovery_native.py`
- [ ] `scenarios/stacks_save.py`
- [ ] `scenarios/wayne_handin_live.py`
- [ ] `scenarios/workshop_mockup.py`
- [ ] `scenarios/workshop_screens.py`

### Helper tools (13)

- [ ] `tools/economy-step5/build.py`
- [ ] `tools/economy-step5/targeted.py`
- [ ] `tools/economy-step6/build.py`
- [ ] `tools/phone_fit.py`
- [ ] `tools/phone_rooms.py`
- [ ] `tools/run_api_extensions.py`
- [ ] `tools/run_api_integrations.py`
- [ ] `tools/run_api_interactions.py`
- [ ] `tools/run_api_save_regressions.py`
- [ ] `tools/run_recovery.py`
- [ ] `tools/save_rooms.py`
- [ ] `tools/scenario.py`
- [ ] `tools/test_panel_limits.py`

### Harness unit tests (1)

- [ ] `tests/test_debug_helpers.py`

## Appendix B: Game repository Python file checklist (178)

Source paths below are relative to `pe-headless-debug/`, representing the `parasite-eve-pc` checkout used for this inventory. Each item needs a verified replacement; document any agreed obsolete-script retirement explicitly.

### Native regression drivers (18)

- [ ] `tests/font_game.py`
- [ ] `tests/frame_interpolation.py`
- [ ] `tests/hd_hot_apply.py`
- [ ] `tests/hd_mod_download.py`
- [ ] `tests/hd_pack_png.py`
- [ ] `tests/hd_pack_webp.py`
- [ ] `tests/hd_widescreen.py`
- [ ] `tests/hd_widescreen_catalog.py`
- [ ] `tests/japanese_release.py`
- [ ] `tests/m273_crash_replay.py`
- [ ] `tests/mouse_game.py`
- [ ] `tests/run_pe_channeler_live.py`
- [ ] `tests/run_pe_costume_styles_live.py`
- [ ] `tests/run_pe_striker_live.py`
- [ ] `tests/text_renderer_game.py`
- [ ] `tests/verify_akao_catalog.py`
- [ ] `tests/vsync_display_selection.py`
- [ ] `tests/windows_imports.py`

### Localization tooling and tests (78)

- [ ] `localization/build_glyph_table.py`
- [ ] `localization/check_translations.py`
- [ ] `localization/extract_text.py`
- [ ] `localization/import_ja_patch.py`
- [ ] `localization/name_ja_patch_letters.py`
- [ ] `localization/pelang.py`
- [ ] `localization/petext/__init__.py`
- [ ] `localization/petext/alphabets.py`
- [ ] `localization/petext/battle.py`
- [ ] `localization/petext/citymap.py`
- [ ] `localization/petext/cjk.py`
- [ ] `localization/petext/cyrillic.py`
- [ ] `localization/petext/disc.py`
- [ ] `localization/petext/font.py`
- [ ] `localization/petext/gametext.py`
- [ ] `localization/petext/hanzi.py`
- [ ] `localization/petext/interfaces.py`
- [ ] `localization/petext/japatch.py`
- [ ] `localization/petext/jatext.py`
- [ ] `localization/petext/latin.py`
- [ ] `localization/petext/layout.py`
- [ ] `localization/petext/letterpairs.py`
- [ ] `localization/petext/letters.py`
- [ ] `localization/petext/manifest.py`
- [ ] `localization/petext/menu.py`
- [ ] `localization/petext/modtext.py`
- [ ] `localization/petext/name_catalog.py`
- [ ] `localization/petext/names.py`
- [ ] `localization/petext/patchfile.py`
- [ ] `localization/petext/patchimport.py`
- [ ] `localization/petext/pictures.py`
- [ ] `localization/petext/rtl.py`
- [ ] `localization/petext/script.py`
- [ ] `localization/petext/stretch.py`
- [ ] `localization/petext/tim.py`
- [ ] `localization/petext/validation.py`
- [ ] `localization/petext/wrap.py`
- [ ] `localization/preview_latin.py`
- [ ] `localization/preview_letters.py`
- [ ] `localization/tests/many_letters_pack.py`
- [ ] `localization/tests/test_battle.py`
- [ ] `localization/tests/test_cjk_pictures.py`
- [ ] `localization/tests/test_de_box_fit.py`
- [ ] `localization/tests/test_de_matches_patch.py`
- [ ] `localization/tests/test_en_pack.py`
- [ ] `localization/tests/test_es_both_discs.py`
- [ ] `localization/tests/test_es_box_fit.py`
- [ ] `localization/tests/test_fr_box_fit.py`
- [ ] `localization/tests/test_fr_matches_patch.py`
- [ ] `localization/tests/test_hanzi.py`
- [ ] `localization/tests/test_it_box_fit.py`
- [ ] `localization/tests/test_it_matches_patch.py`
- [ ] `localization/tests/test_ja_patch_import.py`
- [ ] `localization/tests/test_languages.py`
- [ ] `localization/tests/test_letterpairs.py`
- [ ] `localization/tests/test_letters.py`
- [ ] `localization/tests/test_menu.py`
- [ ] `localization/tests/test_names.py`
- [ ] `localization/tests/test_patch_import.py`
- [ ] `localization/tests/test_pelang.py`
- [ ] `localization/tests/test_pictures.py`
- [ ] `localization/tests/test_rtl.py`
- [ ] `localization/tests/test_stretch.py`
- [ ] `localization/tests/test_translator.py`
- [ ] `localization/tests/test_zh_box_fit.py`
- [ ] `localization/tests/test_zh_pictures.py`
- [ ] `localization/translate.py`
- [ ] `localization/translator/__init__.py`
- [ ] `localization/translator/app.py`
- [ ] `localization/translator/artwork.py`
- [ ] `localization/translator/battlelook.py`
- [ ] `localization/translator/gamelook.py`
- [ ] `localization/translator/letterart.py`
- [ ] `localization/translator/letters_window.py`
- [ ] `localization/translator/menulook.py`
- [ ] `localization/translator/package.py`
- [ ] `localization/translator/picture_window.py`
- [ ] `localization/translator/project.py`

### Asset pipelines and historical scripts (75)

- [ ] `docs/assets/hd-animated/anchor_materials.py`
- [ ] `docs/assets/hd-animated/assemble_sources.py`
- [ ] `docs/assets/hd-animated/audit_pack.py`
- [ ] `docs/assets/hd-animated/audit_registration.py`
- [ ] `docs/assets/hd-animated/audit_temporal.py`
- [ ] `docs/assets/hd-animated/combine.py`
- [ ] `docs/assets/hd-animated/composite.py`
- [ ] `docs/assets/hd-animated/corrections/issue-56/build_candidate.py`
- [ ] `docs/assets/hd-animated/corrections/issue-56/build_underlays.py`
- [ ] `docs/assets/hd-animated/corrections/issue-56/encode_webp.py`
- [ ] `docs/assets/hd-animated/normalize.py`
- [ ] `docs/assets/hd-animated/plan_anchors.py`
- [ ] `docs/assets/hd-animated/prepare.py`
- [ ] `docs/assets/hd-animated/register_anchors.py`
- [ ] `docs/assets/hd-animated/retain_reviewed.py`
- [ ] `docs/assets/hd-animated/review.py`
- [ ] `docs/assets/hd-animated/source_motion.py`
- [ ] `docs/assets/hd-day1/apply_phone_fix.py`
- [ ] `docs/assets/hd-day1/audit_animation_fit.py`
- [ ] `docs/assets/hd-day1/audit_motion.py`
- [ ] `docs/assets/hd-day1/audit_pack.py`
- [ ] `docs/assets/hd-day1/audit_rendered.py`
- [ ] `docs/assets/hd-day1/combine_controller_sheets.py`
- [ ] `docs/assets/hd-day1/compose_local.py`
- [ ] `docs/assets/hd-day1/composite.py`
- [ ] `docs/assets/hd-day1/feedback-fixes/apply_gate_material.py`
- [ ] `docs/assets/hd-day1/feedback-fixes/apply_loop_tail.py`
- [ ] `docs/assets/hd-day1/feedback-fixes/apply_m027_motion.py`
- [ ] `docs/assets/hd-day1/feedback-fixes/apply_m037_source_motion.py`
- [ ] `docs/assets/hd-day1/feedback-fixes/apply_structural.py`
- [ ] `docs/assets/hd-day1/feedback-fixes/apply_temporal.py`
- [ ] `docs/assets/hd-day1/feedback-fixes/audit.py`
- [ ] `docs/assets/hd-day1/feedback-fixes/audit_m037_details.py`
- [ ] `docs/assets/hd-day1/feedback-fixes/audit_m037_pillars.py`
- [ ] `docs/assets/hd-day1/feedback-fixes/finalize_gate.py`
- [ ] `docs/assets/hd-day1/feedback-fixes/prepare_temporal.py`
- [ ] `docs/assets/hd-day1/feedback-fixes/rebuild.py`
- [ ] `docs/assets/hd-day1/finish_pipeline.py`
- [ ] `docs/assets/hd-day1/localize.py`
- [ ] `docs/assets/hd-day1/make_review_sheets.py`
- [ ] `docs/assets/hd-day1/next_jobs.py`
- [ ] `docs/assets/hd-day1/next_local.py`
- [ ] `docs/assets/hd-day1/normalize.py`
- [ ] `docs/assets/hd-day1/prepare.py`
- [ ] `docs/assets/hd-day1/reconcile_animation.py`
- [ ] `docs/assets/hd-day1/record_fixture.py`
- [ ] `docs/assets/hd-day1/replay.py`
- [ ] `docs/assets/hd-day1/review.py`
- [ ] `docs/assets/hd-day1/review_composites.py`
- [ ] `docs/assets/hd-day1/review_corrections.py`
- [ ] `docs/assets/hd-day1/runtime_gallery.py`
- [ ] `docs/assets/hd-poc/compress_pack.py`
- [ ] `docs/assets/hd-poc/compress_webp.py`
- [ ] `docs/assets/hd-poc/export.py`
- [ ] `docs/assets/hd-poc/infer.py`
- [ ] `docs/assets/hd-poc/pack.py`
- [ ] `docs/assets/hd-static/apply_corrections.py`
- [ ] `docs/assets/hd-static/audit_pack.py`
- [ ] `docs/assets/hd-static/audit_registration.py`
- [ ] `docs/assets/hd-static/audit_static.py`
- [ ] `docs/assets/hd-static/combine.py`
- [ ] `docs/assets/hd-static/composite.py`
- [ ] `docs/assets/hd-static/gallery.py`
- [ ] `docs/assets/hd-static/inventory.py`
- [ ] `docs/assets/hd-static/next.py`
- [ ] `docs/assets/hd-static/normalize.py`
- [ ] `docs/assets/hd-static/prepare.py`
- [ ] `docs/assets/hd-title/extract.py`
- [ ] `docs/assets/hd-widescreen/foreground/build_front.py`
- [ ] `docs/assets/hd-widescreen/foreground/cut.py`
- [ ] `docs/assets/hd-widescreen/foreground/hd_matte.py`
- [ ] `docs/assets/hd-widescreen/foreground/inspect_room.py`
- [ ] `docs/assets/hd-widescreen/prepare.py`
- [ ] `docs/assets/widescreen/backgrounds.py`
- [ ] `docs/assets/widescreen/survey.py`

### Lighting-editor tooling (7)

- [ ] `tools/lighting-editor/archive_desktop.py`
- [ ] `tools/lighting-editor/assets.py`
- [ ] `tools/lighting-editor/build_desktop.py`
- [ ] `tools/lighting-editor/capture_model.py`
- [ ] `tools/lighting-editor/create_test_assets.py`
- [ ] `tools/lighting-editor/serve.py`
- [ ] `tools/lighting-editor/test_assets.py`

## Validation and evidence record

### Batch 4: save envelopes, export and remaining session helpers (November 16, 2026)

Owner: Codex. Continued on `feat/rust-debug-tools` from `214d05869`.

- Added `pe-save` for bounded card/container/companion parsing and verified bundle
  export. The game CRC module is shared with `save-editor/src/crc.rs` without GUI
  dependencies; CRC32 uses pinned `crc32fast` 1.5.4. CI watches the shared source.
- PEMC parsing retains unknown IDs/version/payload bytes, validates container and
  record CRCs, discovers legacy blocks and applies the compatibility-window
  precedence used by Python. Generic PEEX sections retain opaque provider data
  while checking name/generation/size, all checksum layers, order and bounds.
- Added reusable binary builders for synthetic fixtures, plus `PortableSave`
  read/validate/copy APIs. Exports require new destinations, validate before
  creation, write companions before cards, and remove only their own new files
  on ordinary failures. They do not promise crash-atomic publication.
- Ported `mods_tail`, `save_block`, `save_files`, `save_to`, card listing, logs and
  extra native arguments. Battle/save clients now share `transport.rs`.
  Raw blocks remain inspectable; verified exports reject corrupt, unknown or
  unsupported envelopes and inconsistent/missing companion metadata.
- Added `pe-tools card-check CARD`, `card-copy CARD NEW_DIRECTORY`, and
  `save-export --control-dir DIR --out NEW_DIRECTORY [--slot N]`.
- Passed: 71 Rust tests (14 protocol, 12 save, 33 session, 12 CLI), warnings denied,
  locked offline build. One ignored entry is solely the subprocess fixture entry.
  New cases cover every truncated binary prefix, independent CRC failures,
  unknown records, compatibility-window precedence, bounded companion directories,
  path/metadata rejection, native refusals over stale output, no-companion cards,
  cleanup/overwrite refusal, attached/headless export, card/log helpers and arguments.
- Passed: 11 Python comparison cases. A Rust-built card/companion matched Python
  byte-for-byte, including a 600-byte unknown record spanning the container's
  discontiguous physical layout. Python read Rust sections; Rust checked/copied
  Python bundles without changing either file. Python session export helpers
  returned the same tail, block and portable file bytes. Empty/legacy/window-tail
  semantics matched. Corrupt game CRC, foreign format, corrupt companion and
  missing companion could not produce a reported portable copy.
- Local ignored evidence: `devtools/target/save-parity-report.json`; the temporary
  Rust oracle source/binary and synthetic comparison files stay outside commits.
- Limits: base-game state fields, titles, individual legacy/provider payload
  meanings and actual native save loading remain unvalidated/unported. Generic
  structural validity does not establish gameplay/provider correctness. No live
  user saves were used or modified. Native command readiness, signal cleanup,
  flows/scenarios, Clippy/MSRV/remote-platform checks and Python cutover remain pending.

### Batch 3: battle helpers and screenshots (November 16, 2026)

Owner: Codex. Continued on `feat/rust-debug-tools` from `abc698223`.

- Added typed battle/player/weapon/enemy and mod-row models in
  `pe-protocol/src/battle.rs`. Native numeric widths and signedness are checked;
  negative enemy HP, raw PE, signed weapon bonuses and positions are retained.
  Separate target-list and field-actor index types prevent accidental interchange.
  The complete original JSON remains available, including unknown fields/events.
- `pe-session/src/battle.rs` implements every Python battle helper: state, RNG
  get/set, tracing/events, item grants, equipment, all four stat names, mod rows
  and registered settings/restart results. It supports the headless process and
  attached file channel through the same interface. Native refusals are preserved.
- Events distinguish enemy HP commits, admitted incoming loss, observed player HP,
  reward offers, actual steal transfer results, resources, actions and spells.
  Missing trace files produce empty lists; malformed/incomplete lines are explicit
  errors with path/line context. Unknown events are preserved without reinterpretation.
- Added `GameSession::shot`, automatic wait-timeout evidence, and P6-to-PNG support
  in `pe-session/src/image.rs`. New CLI operations: `battle-validate FILE`,
  `events FILE`, `ppm-to-png INPUT OUTPUT`, and `run --shot LABEL`.
- Screenshot conversion uses pinned pure-Rust `png` 0.18.1 (MIT OR Apache-2.0),
  with dependencies locked. Unlike Python, missing frames fail explicitly and
  existing PNG files are refused. Native P6 pixel behavior is preserved; extra
  header/size checks and a 64-megapixel limit reject unsupported/corrupt input.
- Passed: 49 Rust tests (14 protocol, 27 session/image, 8 CLI), warnings denied,
  offline locked workspace build. The sole ignored entry is the subprocess fixture
  invoked by process tests, not a skipped asset-dependent scenario.
- Python comparison: all 15 helper command exchanges in the shared contract
  matched spelling, parameter formatting, refusal handling and restart behavior.
  Rust CLI snapshot JSON and 9 synthetic trace events matched Python's raw JSON;
  both rejected a corrupt final trace line. One existing prebuilt C-test resource
  event also matched. The Rust process test exercised all helper families through
  actual `cmd`/`out` files, including full RNG restore/readback in a mock process.
  These are not live battle/RNG restoration acceptance results.
- Pixel comparison: Rust and Python PNGs decoded to identical dimensions, RGB
  bytes and bit depth for synthetic input and a real 320x240 local-game frame.
  PNG chunk CRCs were validated. A fresh live Rust session launched the same
  local executable identified in Batch 2, advanced VBlank 1 to 31, saved
  `001-boot.png`, and quit with exit 0. No save fixture or gameplay was validated.
- Local ignored reports: `devtools/target/battle-parity-report.json` and
  `devtools/target/screenshot-parity-report.json`; live output is under
  `devtools/target/live-screenshot-smoke-01/`. No game images, native dumps or
  generated reports are included in source commits. The committed fixtures are synthetic.
- Remaining: live native command readiness/battle acceptance, save export/format
  validation, reusable flows, scenario suites, signal handling and caller cutover.
  Clippy is still unavailable locally; remote CI and minimum-version checks remain pending.

### Batch 2 local evidence (November 16, 2026)

- Added `pe-protocol/src/control.rs`, `pe-session/src/process.rs` and `pe-tools run`
  for the process-control portions of `pedebug/game.py` and executable override
  from `pedebug/build.py`. Rust now owns process launch, isolated card/config/log
  folders, control JSON, frame/input scheduling, waits, command stepping and shutdown.
- Passed: 31 Rust tests with warnings denied; one ignored test is solely a child
  fixture entry point and is invoked explicitly by the passing process tests.
  Tests cover native flags, typed JSON/extensions, 2000-state history bound,
  card/companion copies, directory/traversal refusal, hold/release, command polling,
  response deadlines even with chatty output, malformed/oversized output, startup
  exit, and child kill/reap after hung advance/quit.
- Live comparison passed against the same local native executable and USA discs:
  initial VBlank 1, advance 30 to VBlank 31, then graceful quit (Rust CLI exit 0).
  Compared `event`, `vblanks`, `scene`, `flags`, `game_flags`, `pad`, `menu`, `movie`,
  and `move_lock` with the Python harness. Default mod INI was
  `Manage mods/All mods=1`; both runs used empty isolated cards, no save fixture.
- Executable identity: SHA-256
  `ac61d6626d5eb410d3aed060a2941bc8d981ac225853c99dc6445537082bce5d`.
  This identifies the pre-existing local build, not a freshly rebuilt C revision.
  Local report: `devtools/target/process-parity-report.json`; raw logs remain in
  `devtools/target/live-process-smoke-04/`. These generated outputs are ignored.
- Fixed a Windows native-boundary issue: Rust canonicalization returns verbatim
  paths, but the C host appends forward slashes. The session now converts canonical
  drive/UNC paths to conventional absolute paths before passing them to the host;
  drive, Unicode and UNC conversion cases are covered by tests.
- Live `ping` did not complete in the local build's boot state, including after
  advancing 11000 frames. The original Python harness also timed out on boot-time
  `ping`. Do not count the live file-command path, field/save loading or battle
  suites as validated. Mock-process file-command coverage passes.
- Dependency change: pinned `serde_json` 1.0.154 (MIT OR Apache-2.0) plus locked
  dependencies, for cross-platform JSON parsing. CI fetches the lockfile before
  its offline checks. No C runtime/gameplay changes, Python deletions or caller
  cutovers were made. Clippy remains unavailable locally; remote CI not yet run.

### Batch 1 local evidence (November 16, 2026)

- Sources: `pedebug/game.py::Session.command`, the protocol/RNG portions of
  `pedebug/battle.py`, and native `mods/host/debug/pe_debug_rng.c`. Baseline revisions
  are recorded in the current-batch table. Rust changes are working-tree changes,
  not yet committed or pushed.
- Platform: Windows x86_64, Rust/Cargo 1.99.1, installed
  `stable-x86_64-pc-windows-gnullvm` toolchain. `RUSTFLAGS=-D warnings`.
- Passed: `cargo +stable-x86_64-pc-windows-gnullvm test --workspace --locked --offline`
  (18 tests: 5 protocol, 8 transport, 5 CLI); offline workspace build; formatting check.
- Passed: Python 3.13 `-B -m unittest discover -s tests -p test_debug_helpers.py -v`
  from the original harness (3 tests).
- Passed: four direct Python/Rust comparisons using temporary mock channel folders:
  prefix rejection (`battle` vs `battle-state`), `equip` native error propagation,
  `mod-state` JSON with spaces, and `ini-set` with a spaced key/restart marker.
  Returned lines/error messages and CLI exit classification matched; stale replies
  were excluded. These comparisons used no game assets or live process.
- Rust-specific checks include command injection rejection, partial replies,
  frame/wall deadlines, pending-command preservation, lock cleanup/concurrency,
  CLI environment fallback, all RNG bytes and malformed/boundary snapshots.
- Clippy could not run locally: its component is not installed. CI includes the
  check. The default GNU toolchain also lacks usable GCC runtime link libraries;
  the installed gnullvm toolchain built and ran all tests successfully.
- Not validated: declared minimum Rust 1.74, Linux/macOS execution, real game
  launch/fixtures, RNG restoration through the native channel, or scenario suites.
  No Python cutover/removal has occurred. Do not count this as end-to-end parity.

For every completed batch, record:

| Field | Required evidence |
|---|---|
| Source and Rust revisions | Exact commit IDs and relevant source files |
| Replacement | Rust crate/module and real CLI/UI entry point |
| Inputs | Fixture identity, game revision, mod settings, local asset prerequisites |
| Comparison | Binary, JSON, semantic, image or interaction parity criteria |
| Results | Passed/failed/skipped checks, logs/report paths and platform |
| Cutover | Updated callers/docs, Python removal status and remaining dependencies |
