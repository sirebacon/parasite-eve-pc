#!/usr/bin/env python3
"""Bug recorder (mods/pe_bugrec): record, decode and replay a report, headless.

1. A new game played by a fixed input route (title, Carnegie Hall, the first rooms, a walk) with the
   recorder on and PE_BUGREC_DUMP_AT: a report must hold a recording with room changes, dialogue
   pages and state rows, the controller tape, the state hashes and the launch's starting settings.
2. tools/bugrec/decode.py reads it.
3. tools/bugrec/replay.py replays it: every state check must match.
4. The same report with one controller press taken out of the tape must not match.
Set PE_TEST_DISC (Disc 1); otherwise CTest skips this asset test.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DUMP_AT = 40000


def route():
    events = []

    def press(tick, mask):
        events.extend([f'{tick}:{mask}', f'{tick + 10}:0'])

    press(10000, 8)          # title: Start
    press(11000, 0x10)       # Up to New Game
    press(11500, 0x4000)     # Cross
    for tick in range(17000, 22000, 600):
        press(tick, 0x4000)  # Carnegie Hall dialogue
    press(23000, 8)
    press(23600, 8)
    for tick in range(24200, 32200, 600):
        press(tick, 0x4000)
    events += ['33000:16', '39000:0']  # walk up through the next rooms
    args = []
    for event in events:
        args += ['--pad-at', event]
    return args


def main(game, evidence):
    disc = os.getenv('PE_TEST_DISC')
    if not disc:
        print('Set PE_TEST_DISC to run the Bug recorder replay test')
        return 77
    evidence.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='bugrec-', dir=evidence))
    config = work / 'config'
    config.mkdir()
    (config / 'pe_mods.ini').write_text('Manage mods/All mods=1\nManage mods/Bug recorder=1\n')
    env = dict(os.environ, SDL_AUDIO_DRIVER='dummy', PSYZ_AUDIO_CLOCK='video', PE_CONFIG_HOME=str(config),
               PE_BUGREC_DUMP_AT=str(DUMP_AT))
    env.pop('PE_HD_BACKGROUNDS', None)
    env.pop('PE_BUGREC_REPLAY', None)
    args = [str(game), '--disc', disc, '--headless', '--frame-limit', str(DUMP_AT + 100)] + route()
    with (work / 'record.log').open('w') as log:
        status = subprocess.run(args, cwd=work, env=env, stdout=log, stderr=subprocess.STDOUT).returncode
    assert status == 0, f'recording run exit {status}; see {work}'

    reports = sorted((config / 'bug-reports').glob('*'))
    assert len(reports) == 1, f'expected one report, found {reports}'
    report = reports[0]
    for name in ('recording.jsonl', 'README.txt', 'note.txt', 'session/tape.txt', 'session/hashes.txt',
                 'session/session.json', 'session/start/pe_mods.ini'):
        assert (report / name).is_file(), f'report lacks {name}'
    rows = [json.loads(line) for line in (report / 'recording.jsonl').open(encoding='utf-8')]
    tags = {row['t'] for row in rows}
    for tag in ('keyframe', 'block', 'diff', 'state', 'pad', 'scene', 'newgame', 'text_page', 'log', 'bug'):
        assert tag in tags, f'recording has no {tag} rows'
    maps = {row['map'] for row in rows if row['t'] == 'scene'}
    assert {10, 2, 3, 372} <= maps, f'expected the first rooms (10, 2, 3, 372), got {sorted(maps)}'
    walked = [row['pos'] for row in rows if row['t'] == 'state' and row['map'] == 3]
    assert len({tuple(p) for p in walked}) > 1, 'Aya did not move in M003'

    decoded = subprocess.run([sys.executable, '-I', str(ROOT / 'tools/bugrec/decode.py'), str(report)],
                             capture_output=True, text=True)
    assert decoded.returncode == 0, decoded.stderr
    for text in ('scene map=372', 'vars.var[74]', 'misc.sceneToken', 'player.'):
        assert text in decoded.stdout, f'decode output lacks {text}'
    at = subprocess.run([sys.executable, '-I', str(ROOT / 'tools/bugrec/decode.py'), str(report), '--at',
                         str(DUMP_AT)], capture_output=True, text=True)
    assert at.returncode == 0 and '== save:' in at.stdout and 'currentMapNumber' in at.stdout, at.stderr

    def replay(folder):
        out = subprocess.run([sys.executable, '-I', str(ROOT / 'tools/bugrec/replay.py'), str(folder), '--exe',
                              str(game), '--disc', disc], capture_output=True, text=True, env=env)
        (folder / 'replay.txt').write_text(out.stdout + out.stderr)
        return out

    good = replay(report)
    assert good.returncode == 0 and 'all of them' in good.stdout, f'replay did not match:\n{good.stdout}'

    tampered = work / 'tampered'
    shutil.copytree(report, tampered, ignore=shutil.ignore_patterns('replay'))
    tape = tampered / 'session/tape.txt'
    lines = tape.read_text().splitlines(keepends=True)
    walk = [i for i, line in enumerate(lines) if line[:1].isdigit() and 33000 <= int(line.split()[0]) <= 33010]
    assert walk, 'no walk press on the tape at 33000'
    del lines[walk[0]]
    tape.write_text(''.join(lines))
    bad = replay(tampered)
    assert bad.returncode == 1 and 'DIVERGED' in bad.stdout, f'a changed tape still matched:\n{bad.stdout}'

    print(f'PASS: {len(rows)} rows recorded, decoded, replayed ({good.stdout.count("matches")} progress lines), '
          f'and a changed tape diverged: {work}')
    return 0


if __name__ == '__main__':
    sys.exit(main(Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()))
