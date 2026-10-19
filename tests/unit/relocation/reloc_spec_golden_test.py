#!/usr/bin/env python3
"""reloc_spec_golden_test — golden structural invariants of the committed
synthetic-reloc spec table (generated/src/<ver>/ssb64_reloc_rebuild.<ver>.cpp).

The spec table is the generator<->runtime contract of the deblob engine
(docs/deblob.md): the runtime trusts file_id ordering, slice containment
inside vanilla_data_size, and intern/extern slot bounds without
re-validating per build. A bad regen silently corrupts the relocated view;
these invariants are exactly what the runtime assumes, so check them here
deterministically and ROM-free.

Checks per spec entry:
  * gSyntheticRelocSpecs sorted by file_id, unique
  * slice/intern/extern arrays exist, counts match the spec fields
  * slice vanilla_offset ascending, vanilla_offset+size <= vanilla_data_size
  * intern slot: slot_slice/target_slice < slice_count; offsets inside slice
  * extern slot: slot_slice < slice_count; slot offset inside slice;
    dep_file_id present in RelocFileTable id space (< RELOC_FILE_COUNT)

Update path: regenerate with tools/generate_fighter_slices.py — invariants
must still hold; this test fails on contract violations, not on new data.
"""
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
FAILS = []


def fail(msg):
    FAILS.append(msg)


def parse_int(s):
    s = s.strip().rstrip('uU')
    return int(s, 16) if s.lower().startswith('0x') else int(s)


def parse_file(path):
    text = path.read_text(encoding='utf-8')

    slices = {}
    for m in re.finditer(
            r'const SSB64SyntheticSlice (\w+)\[\] = \{(.*?)\};', text, re.DOTALL):
        name, body = m.group(1), m.group(2)
        entries = re.findall(
            r'\{\s*"([^"]+)",\s*(0x[0-9A-Fa-f]+|\d+),\s*(0x[0-9A-Fa-f]+|\d+),'
            r'\s*SSB64SyntheticSliceKind::(\w+)\s*\}', body)
        slices[name] = [
            (p, parse_int(off), parse_int(sz), kind)
            for p, off, sz, kind in entries]

    interns = {}
    for m in re.finditer(
            r'const SSB64RelocInternSlot (\w+)\[\] = \{(.*?)\};', text, re.DOTALL):
        name, body = m.group(1), m.group(2)
        entries = re.findall(
            r'\{\s*(\d+),\s*(\d+),\s*(0x[0-9A-Fa-f]+|\d+),\s*(0x[0-9A-Fa-f]+|\d+)\s*\}',
            body)
        interns[name] = [
            (int(a), int(b), parse_int(c), parse_int(d))
            for a, b, c, d in entries]

    externs = {}
    for m in re.finditer(
            r'const SSB64RelocExternSlot (\w+)\[\] = \{(.*?)\};', text, re.DOTALL):
        name, body = m.group(1), m.group(2)
        entries = re.findall(
            r'\{\s*(\d+),\s*(\d+),\s*(0x[0-9A-Fa-f]+|\d+),\s*(0x[0-9A-Fa-f]+|\d+)\s*\}',
            body)
        externs[name] = [
            (int(a), int(b), parse_int(c), parse_int(d))
            for a, b, c, d in entries]

    specs = []
    table = re.search(r'gSyntheticRelocSpecs\[\] = \{(.*?)\};', text, re.DOTALL)
    if not table:
        fail(f'{path.name}: gSyntheticRelocSpecs table not found')
        return specs, slices, interns, externs
    for m in re.finditer(
            r'\{\s*(\d+),\s*"([^"]+)",\s*(0x[0-9A-Fa-f]+|\d+),\s*0x[0-9A-Fa-f]+u?,'
            r'\s*"([0-9a-f]+)",\s*(\w+|nullptr),\s*(\d+),'
            r'\s*(\w+|nullptr),\s*(\d+),\s*(\w+|nullptr),\s*(\d+)\s*\}',
            table.group(1)):
        (fid, parent, vsize, mhash, sarr, scnt,
         iarr, icnt, earr, ecnt) = m.groups()
        specs.append({
            'file_id': int(fid), 'parent': parent, 'vsize': parse_int(vsize),
            'mhash': mhash, 'sarr': sarr, 'scnt': int(scnt),
            'iarr': iarr, 'icnt': int(icnt), 'earr': earr, 'ecnt': int(ecnt)})
    return specs, slices, interns, externs


def check_version(ver):
    path = REPO / 'generated' / 'src' / ver / f'ssb64_reloc_rebuild.{ver}.cpp'
    if not path.exists():
        print(f'reloc_spec_golden: SKIP (no {path.name})')
        return
    specs, slices, interns, externs = parse_file(path)
    tag = f'{path.name}'
    if not specs:
        fail(f'{tag}: no spec entries parsed')
    prev_id = -1
    for s in specs:
        ctx = f'{tag}:{s["parent"]}(fid {s["file_id"]})'
        if s['file_id'] <= prev_id:
            fail(f'{ctx}: file_id not strictly ascending (prev {prev_id})')
        prev_id = s['file_id']

        sl = slices.get(s['sarr'])
        if s['sarr'] == 'nullptr':
            if s['scnt'] != 0:
                fail(f'{ctx}: slices nullptr but slice_count={s["scnt"]}')
            sl = []
        elif sl is None:
            fail(f'{ctx}: missing slice array {s["sarr"]}')
            sl = []
        if sl is not None and len(sl) != s['scnt']:
            fail(f'{ctx}: slice_count {s["scnt"]} != len({s["sarr"]})={len(sl)}')

        prev_off = -1
        for i, (p, off, sz, _kind) in enumerate(sl):
            if off <= prev_off:
                fail(f'{ctx}: slice {i} offset 0x{off:X} not ascending')
            prev_off = off
            if off + sz > s['vsize']:
                fail(f'{ctx}: slice {i} [{p}] 0x{off:X}+0x{sz:X} exceeds '
                     f'vanilla_data_size 0x{s["vsize"]:X}')
            exp_pfx = f'd{s["parent"].rsplit("/", 1)[-1]}_'
            if not p.rsplit('/', 1)[-1].startswith(exp_pfx):
                fail(f'{ctx}: slice {i} basename {p} lacks {exp_pfx} prefix')

        il = interns.get(s['iarr'], [] if s['iarr'] == 'nullptr' else None)
        if il is None:
            fail(f'{ctx}: missing intern array {s["iarr"]}')
            il = []
        if len(il) != s['icnt']:
            fail(f'{ctx}: intern_slot_count {s["icnt"]} != len={len(il)}')
        for i, (ss, ts, soff, toff) in enumerate(il):
            if ss >= len(sl) or ts >= len(sl):
                fail(f'{ctx}: intern {i} slice idx {ss}->{ts} out of range')
                continue
            if soff + 4 > sl[ss][2]:
                fail(f'{ctx}: intern {i} slot_off 0x{soff:X} exceeds slice {ss}')
            if toff > sl[ts][2]:
                fail(f'{ctx}: intern {i} target_off 0x{toff:X} exceeds slice {ts}')

        el = externs.get(s['earr'], [] if s['earr'] == 'nullptr' else None)
        if el is None:
            fail(f'{ctx}: missing extern array {s["earr"]}')
            el = []
        if len(el) != s['ecnt']:
            fail(f'{ctx}: extern_slot_count {s["ecnt"]} != len={len(el)}')
        for i, (ss, dep, soff, _doff) in enumerate(el):
            if ss >= len(sl):
                fail(f'{ctx}: extern {i} slot_slice {ss} out of range')
                continue
            if soff + 4 > sl[ss][2]:
                fail(f'{ctx}: extern {i} slot_off 0x{soff:X} exceeds slice {ss}')
            if dep == 0:
                fail(f'{ctx}: extern {i} dep_file_id 0')
    return specs


def main():
    total = 0
    for ver in ('us', 'jp'):
        specs = check_version(ver)
        if specs:
            total += len(specs)
    if FAILS:
        for f in FAILS[:40]:
            print('FAIL', f)
        if len(FAILS) > 40:
            print(f'... and {len(FAILS) - 40} more')
        print(f'reloc_spec_golden: FAIL ({len(FAILS)} violations)')
        return 1
    print(f'reloc_spec_golden: PASS ({total} specs, all invariants hold)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
