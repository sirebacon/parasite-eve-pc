#!/usr/bin/env python3
"""Replay a Bug recorder report (mods/pe_bugrec) headless, from launch to the moment it was saved.

  replay.py <report folder> --exe build/parasite-eve-pc --disc "Disc 1.cue" [--disc2 "Disc 2.cue"]

It rebuilds the launch's settings, mods' settings, quick saves and memory cards in <report>/replay,
starts the game headless with them, and the recorder plays the controller tape back while it checks
its state hashes against the recorded ones. It prints where the replay first differs, if it does, and
writes a recording of the replay at the same moment (<report>/replay/config/bug-reports/...), which
decode.py reads like the original. Exit 0: every state check matched; 1: the replay diverged.
"""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path


def bug_vblank(report):
    note = report / 'note.txt'
    if note.exists():
        words = note.read_text(errors='replace').split()
        if 'VBlank' in words:
            return int(words[words.index('VBlank') + 1])
    last = 0
    with (report / 'session' / 'tape.txt').open() as f:
        for line in f:
            if line[:1].isdigit():
                last = int(line.split()[0])
    return last + 600


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('report', type=Path, help='a bug-reports/<date-time> folder, or bug-recorder/session-previous')
    p.add_argument('--exe', required=True, type=Path)
    p.add_argument('--disc', required=True, type=Path)
    p.add_argument('--disc2', type=Path)
    p.add_argument('--work', type=Path)
    p.add_argument('--extra', type=int, default=120, help='VBlanks to run past the report (default 120)')
    a = p.parse_args()

    report = a.report.resolve()
    session = report / 'session' if (report / 'session' / 'tape.txt').exists() else report
    if not (session / 'tape.txt').exists():
        sys.exit(f'no tape.txt in {report}')
    stop = bug_vblank(report) if session != report else bug_vblank_from_session(session)
    work = (a.work or report / 'replay').resolve()
    if work.exists():
        shutil.rmtree(work)
    config = work / 'config'
    shutil.copytree(session / 'start', config)
    # Cards live under the settings folder; headless runs also look beside the working folder.
    cards = config / 'memory-cards'
    if cards.exists():
        for port in ('bu00', 'bu10'):
            if (cards / port).exists():
                shutil.copytree(cards / port, work / port)

    env = dict(os.environ, SDL_AUDIO_DRIVER='dummy', PSYZ_AUDIO_CLOCK='video', PE_CONFIG_HOME=str(config),
               PE_BUGREC_REPLAY=str(session), PE_BUGREC_DUMP_AT=str(stop))
    env.pop('PE_HD_BACKGROUNDS', None)
    args = [str(a.exe.resolve()), '--disc', str(a.disc.resolve())]
    if a.disc2:
        args += ['--disc2', str(a.disc2.resolve())]
    args += ['--headless', '--frame-limit', str(stop + a.extra), '--dump-frame', str(work / 'frame.ppm')]
    print('replaying to VBlank', stop, 'in', work, flush=True)
    diverged = matched = False
    with (work / 'game.log').open('w') as log:
        child = subprocess.Popen(args, cwd=work, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 text=True, errors='replace')
        for line in child.stdout:
            log.write(line)
            if line.startswith('bugrec:'):
                print(line.rstrip(), flush=True)
                diverged |= 'DIVERGED' in line
                matched |= 'all of them' in line
        status = child.wait()
    print(f'game exit {status}; log {work / "game.log"}')
    if diverged:
        print('RESULT: the replay differs from the recorded play (see the first DIVERGED line)')
        return 1
    print('RESULT: the replay matched' + (' every state check' if matched else ' (no state check reached)'))
    return 0 if status == 0 else 2


def bug_vblank_from_session(session):
    last = 0
    with (session / 'hashes.txt').open() as f:
        for line in f:
            if line[:1].isdigit():
                last = int(line.split()[0])
    return last + 60


if __name__ == '__main__':
    sys.exit(main())
