"""Port reverse-engineered symbol names from one ROM to a sibling ECU.

    python port_rom.py scope    SRC.asm DST.asm
    python port_rom.py offsets  CPU1.asm CPU2.asm
    python port_rom.py plan     SRC.asm DST.asm [-o plan.json] [--windows]
    python port_rom.py apply    DST.asm plan.json

`plan` never writes to the source files; `apply` is the only command that
does. ALWAYS reassemble afterwards and confirm the binary is unchanged --
renames should be inert, but that is a claim to check, not to assume:

    python ../d8x_assembler/asm_d8x.py -p 5F DST.asm out.bin out.lst
    python ../verify_assembly_match.py before.lst out.lst
"""

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from d8x_source import functions, ram_symbols, AUTO                  # noqa: E402
from matchers import (exact_function_pairs, fuzzy_function_pairs,     # noqa: E402
                      lockstep_votes, window_votes, clear_winners, dma_offset)
from planner import plan_renames, apply_renames                      # noqa: E402


def cmd_scope(args):
    src, dst = functions(args.source), functions(args.target)
    pairs = exact_function_pairs(args.source, args.target)
    fuzzy = fuzzy_function_pairs(args.source, args.target, exclude=pairs)
    print(f'{os.path.basename(args.source)}: {len(src)} functions, '
          f'{sum(1 for f in src if not AUTO.match(f["name"]))} hand-named')
    print(f'{os.path.basename(args.target)}: {len(dst)} functions, '
          f'{sum(1 for f in dst if not AUTO.match(f["name"]))} hand-named')
    print(f'\nexact signature matches: {len(pairs)}   fuzzy >=0.75: {len(fuzzy)}')

    gains = [(a, b) for a, b in pairs if AUTO.match(b['name']) and not AUTO.match(a['name'])]
    print(f'\n-- target unnamed, source named (exact, {len(gains)}) --')
    for a, b in sorted(gains, key=lambda p: p[1]['name']):
        print(f'   {b["name"]:16s} -> {a["name"]:38s} ({len(a["sig"])} insns)')

    clash = [(a, b) for a, b in pairs
             if not AUTO.match(a['name']) and not AUTO.match(b['name'])
             and a['name'] != b['name']]
    if clash:
        print('\n-- both named, names differ (review) --')
        for a, b in clash:
            print(f'   target {b["name"]:34s} vs source {a["name"]}')

    print(f'\n-- fuzzy, NOT used for renaming ({len(fuzzy)}) --')
    for a, b, ratio in sorted(fuzzy, key=lambda t: -t[2])[:20]:
        print(f'   {b["name"]:16s} ~ {a["name"]:38s} {ratio:.2f}')


def cmd_offsets(args):
    cpu1, cpu2 = ram_symbols(args.cpu1), ram_symbols(args.cpu2)
    print('CPU1_addr = CPU2_addr + N, derived from this pair\'s own name pairs.')
    print('These differ per ECU pair -- never reuse another pair\'s number.\n')
    for direction in ('cpu2_to_cpu1', 'cpu1_to_cpu2'):
        offset, votes = dma_offset(cpu1, cpu2, direction)
        if offset is None:
            print(f'  {direction:14s}  no dmatx_/dmarx_ name pairs found')
            continue
        total = sum(votes.values())
        print(f'  {direction:14s}  N = 0x{offset:X}   '
              f'({votes[offset]}/{total} pairs agree; '
              f'others {[hex(v) for v in votes if v != offset]})')


def _collect_votes(args):
    pairs = exact_function_pairs(args.source, args.target)
    votes, referenced = lockstep_votes(pairs)
    matched = None
    if args.windows:
        wvotes, matched = window_votes(args.source, args.target)
        for target, (source, count) in clear_winners(wvotes).items():
            if target not in votes:          # exact evidence always wins
                votes[target][source] += count
    return pairs, votes, referenced, matched


def cmd_plan(args):
    pairs, votes, referenced, matched = _collect_votes(args)
    print(f'exact function pairs: {len(pairs)}')
    if matched is not None:
        print(f'unique matching instruction windows: {matched}')

    new, updated, unchanged, rejected = plan_renames(
        votes, args.source, args.target, referenced=referenced,
        min_votes=args.min_votes, trust_referenced=not args.windows)

    print(f'\nNEW {len(new)}   UPDATED {len(updated)}   '
          f'already correct {len(unchanged)}   REJECTED {len(rejected)}')
    for title, rows in (('NEW', new), ('UPDATED', updated)):
        if not rows:
            continue
        print(f'\n-- {title} --')
        for target, name, count, why in sorted(rows):
            print(f'   {target:20s} -> {name:36s} {why}')
    if rejected:
        print('\n-- REJECTED (resolve by hand or leave alone) --')
        for target, name, count, why in sorted(rejected, key=lambda r: str(r[3])):
            print(f'   {target:20s} -> {name:36s} [{why}]')

    if args.out:
        with open(args.out, 'w', encoding='utf-8') as handle:
            json.dump({'source': args.source, 'target': args.target,
                       'new': new, 'updated': updated,
                       'rejected': [list(r) for r in rejected]}, handle, indent=1)
        print(f'\nplan written to {args.out}')


def cmd_apply(args):
    with open(args.plan, encoding='utf-8') as handle:
        plan = json.load(handle)
    renames = {row[0]: row[1] for row in plan['new']}
    if args.include_updates:
        renames.update({row[0]: row[1] for row in plan['updated']})
    elif plan['updated']:
        print(f'skipping {len(plan["updated"])} UPDATED renames '
              '(pass --include-updates to apply them).')
        print('Each one overwrites a name someone chose deliberately, so read '
              'them first: the source ROM having a better name is only one\n'
              'reason they can differ -- the other is that the two ROMs really '
              'do differ there, and the target\'s name is the correct one.')
        for row in plan['updated']:
            print(f'   {row[0]:24s} -> {row[1]}')
    if not renames:
        print('nothing to apply')
        return
    counts = apply_renames(args.target, renames)
    print(f'renamed {len(counts)}/{len(renames)} symbols, '
          f'{sum(counts.values())} substitutions')
    missing = [k for k in renames if k not in counts]
    if missing:
        print('NOT FOUND in target:', missing)
    print('\nNow reassemble and confirm the binary is unchanged:')
    print(f'  python ../d8x_assembler/asm_d8x.py -p 5F {args.target} out.bin out.lst')
    print( '  python ../verify_assembly_match.py before.lst out.lst')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)

    p = sub.add_parser('scope', help='report how well two ROMs match')
    p.add_argument('source'); p.add_argument('target')
    p.set_defaults(func=cmd_scope)

    p = sub.add_parser('offsets', help='derive an ECU pair\'s DMA offsets')
    p.add_argument('cpu1'); p.add_argument('cpu2')
    p.set_defaults(func=cmd_offsets)

    p = sub.add_parser('plan', help='produce a rename plan (writes nothing)')
    p.add_argument('source'); p.add_argument('target')
    p.add_argument('-o', '--out', help='write the plan as JSON')
    p.add_argument('--windows', action='store_true',
                   help='add windowed matching: more reach, weaker evidence, '
                        'and embedded addresses other than the symbol\'s own '
                        'are then refused')
    p.add_argument('--min-votes', type=int, default=1,
                   help='drop mappings with fewer votes (8 is a sane floor '
                        'with --windows)')
    p.set_defaults(func=cmd_plan)

    p = sub.add_parser('apply', help='apply a saved plan to the target file')
    p.add_argument('target'); p.add_argument('plan')
    p.add_argument('--include-updates', action='store_true',
                   help='also apply renames that overwrite an existing '
                        'hand-chosen name in the target (off by default)')
    p.set_defaults(func=cmd_apply)

    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == '__main__':
    sys.exit(main())
