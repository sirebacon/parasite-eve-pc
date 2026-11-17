#!/usr/bin/env python3
"""Read a Bug recorder report (mods/pe_bugrec) as text, for people and for AI.

  decode.py <report folder or recording.jsonl>              timeline: every row, with the
                                                             changed state bytes named and shown old -> new
  decode.py <report> --at VBLANK                            every state block as it was at that VBlank
  decode.py <report> --from VBLANK --to VBLANK              only that part of the timeline
  decode.py <report> --summary                              one line a second: room, HP, position ...

VBlanks count from launch, 60 a second; times are shown as minutes:seconds from launch.
"""
import argparse
import json
import struct
import sys
from pathlib import Path

# The game's runtime save state (include/pe1/save_blob.h, SaveRuntimeState).
SAVE_FIELDS = [
    (0x0000, 8, 'saveName'), (0x0008, 4, 'playTimeMinutes'), (0x000C, 4, 'gameTimeMinutes'),
    (0x0010, 13, 'ayaName'), (0x001D, 1, 'menuLayout'), (0x001E, 1, 'reserved1E'), (0x001F, 1, 'drawBaseY'),
    (0x0020, 0x3C, 'ayaStats'), (0x005C, 2, 'playTimeClass'), (0x005E, 2, 'currentMapNumber'),
    (0x0060, 2, 'menuClampValue'), (0x0062, 2, 'reserved62'), (0x0064, 4, 'drawBlendColor'),
    (0x0068, 0x1064, 'inventory'), (0x10CC, 12, 'categoryCounts'), (0x10D8, 0x1EC, 'reserved10D8'),
    (0x12C4, 0x20, 'cursorMetadata'),
]
# The Misc block the recorder builds (mods/pe_bugrec/pe_bugrec.c).
MISC_FIELDS = [(0, 4, 'frameCounter'), (4, 4, 'sceneToken'), (8, 4, 'gameStateFlags'), (12, 4, 'fieldMoveLock')]
BLOCK_NOTES = {
    'save': 'runtime save state (include/pe1/save_blob.h SaveRuntimeState)',
    'vars': 'script variable bank, 0x200 u32 words (story flags and counters)',
    'game': 'game state block (include/pe1/game_state_types.h Pe1GameState; holds native pointers)',
    'aya': "Aya's combatant record words (include/pe1/battle.h Combatant up to its action pointer)",
    'enemies': 'battle enemy slots x7 (include/pe1/battle_runtime.h BattleEnemySlot)',
    'player': "Aya's actor (include/pe1/battle.h BattleEntity; posX/Y/Z at 0x28/0x2C/0x30, 16.16)",
    'rng': 'per-frame generator words g_DrawPrimGeneratorWords[33]',
    'misc': 'frame counter, scene token, game state flags, field move lock',
}


def clock(v):
    s = v // 60
    return f'{s // 60:3d}:{s % 60:02d}'


def field_name(block, offset):
    table = SAVE_FIELDS if block == 'save' else MISC_FIELDS if block == 'misc' else None
    if table:
        for start, size, name in table:
            if start <= offset < start + size:
                return name if offset == start else f'{name}+0x{offset - start:X}'
    if block == 'vars':
        return f'var[{offset // 4}]' + (f'+{offset % 4}' if offset % 4 else '')
    return f'+0x{offset:X}'


def load(path):
    path = Path(path)
    if path.is_dir():
        path = path / 'recording.jsonl'
    rows = []
    with path.open(encoding='utf-8') as f:
        for n, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError:
                print(f'# line {n}: not JSON (cut short?)', file=sys.stderr)
    return rows


class State:
    def __init__(self):
        self.blocks = {}

    def apply(self, row):
        """Applies a block or diff row; returns [(block, offset, old bytes, new bytes)]."""
        changes = []
        if row['t'] == 'block':
            data = bytearray.fromhex(row['x'])
            old = self.blocks.get(row['r'])
            if old is not None and len(old) == len(data):
                changes = runs(row['r'], old, data)
            self.blocks[row['r']] = data
        elif row['t'] == 'diff' and row['r'] in self.blocks:
            data = self.blocks[row['r']]
            for offset, hexed in row['d']:
                new = bytes.fromhex(hexed)
                old = bytes(data[offset:offset + len(new)])
                data[offset:offset + len(new)] = new
                changes.extend(runs(row['r'], old, new, offset))
        return changes


def runs(block, old, new, base=0):
    """Byte runs that differ, split at field edges so each names one field."""
    out, i = [], 0
    while i < len(new):
        if old[i] == new[i]:
            i += 1
            continue
        j = i + 1
        while j < len(new) and old[j] != new[j] and field_name(block, base + j).split('+')[0] == \
                field_name(block, base + i).split('+')[0]:
            j += 1
        out.append((block, base + i, bytes(old[i:j]), bytes(new[i:j])))
        i = j
    return out


def show_value(b):
    if len(b) == 1:
        return f'{b[0]}'
    if len(b) == 2:
        return f'{struct.unpack("<h", b)[0]}'
    if len(b) == 4:
        return f'{struct.unpack("<i", b)[0]} (0x{struct.unpack("<I", b)[0]:08X})'
    return b.hex()


def describe(row):
    skip = {'v', 't'}
    return ' '.join(f'{k}={json.dumps(v) if isinstance(v, (list, dict)) else v}'
                    for k, v in row.items() if k not in skip)


def timeline(rows, start, end, summary):
    state = State()
    for row in rows:
        v, tag = row['v'], row['t']
        changes = state.apply(row)
        if not (start <= v <= end):
            continue
        if summary and tag != 'state':
            continue
        head = f'{clock(v)} v{v:<8}'
        if tag == 'block':
            print(f'{head} block {row["r"]} ({row["size"]} bytes, {BLOCK_NOTES.get(row["r"], "")})'
                  + (f' rewritten: {row.get("why")}' if row.get('why') else ''))
            for block, offset, old, new in changes[:40]:
                print(f'{"":17} {block}.{field_name(block, offset)}: {show_value(old)} -> {show_value(new)}')
        elif tag == 'diff':
            for block, offset, old, new in changes:
                print(f'{head} {block}.{field_name(block, offset)}: {show_value(old)} -> {show_value(new)}'
                      f'  [{row.get("why", "")}]')
        elif tag in ('save_write', 'save_read'):
            print(f'{head} {tag} (8 KB card block; hex in the recording)')
        elif tag == 'keyframe':
            print(f'{head} keyframe ({row.get("why", "")})')
        else:
            print(f'{head} {tag} {describe(row)}')


def at(rows, when):
    state, last = State(), None
    for row in rows:
        if row['v'] > when:
            break
        state.apply(row)
        if row['t'] == 'state':
            last = row
    print(f'State at v{when} ({clock(when).strip()})')
    if last:
        print('last state row:', describe(last))
    for name, data in state.blocks.items():
        print(f'\n== {name}: {len(data)} bytes, {BLOCK_NOTES.get(name, "")}')
        if name in ('save', 'misc'):
            for start, size, field in (SAVE_FIELDS if name == 'save' else MISC_FIELDS):
                chunk = bytes(data[start:start + size])
                if size <= 4:
                    print(f'  {field:16} {show_value(chunk)}')
                elif field in ('saveName', 'ayaName'):
                    print(f'  {field:16} {chunk.split(b"\0")[0]!r}')
                else:
                    for o in range(0, size, 32):
                        part = chunk[o:o + 32]
                        if any(part):
                            print(f'  {field:16} +0x{o:04X} {part.hex(" ")}')
        elif name == 'vars':
            words = struct.unpack(f'<{len(data) // 4}I', data)
            print('  ' + ' '.join(f'var[{i}]=0x{w:X}' for i, w in enumerate(words) if w))
        else:
            for o in range(0, len(data), 32):
                part = bytes(data[o:o + 32])
                if any(part):
                    print(f'  +0x{o:04X} {part.hex(" ")}')


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('report')
    p.add_argument('--at', type=int)
    p.add_argument('--from', dest='start', type=int, default=0)
    p.add_argument('--to', dest='end', type=int, default=1 << 62)
    p.add_argument('--summary', action='store_true')
    a = p.parse_args()
    rows = load(a.report)
    if not rows:
        sys.exit('empty recording')
    if a.at is not None:
        at(rows, a.at)
    else:
        print(f'# {len(rows)} rows, v{rows[0]["v"]}..v{rows[-1]["v"]} '
              f'({clock(rows[0]["v"]).strip()}..{clock(rows[-1]["v"]).strip()} from launch)')
        timeline(rows, a.start, a.end, a.summary)


if __name__ == '__main__':
    main()
