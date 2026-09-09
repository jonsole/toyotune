"""Matching two sibling ROMs by behaviour rather than by address.

Sibling ECUs share most of their code but not their addresses, and not their
RAM layout either -- between D151803-9651 and D151804-0461 the RAM map shifts
piecewise by 0, -4, -6 and -8 bytes in different regions. So nothing can be
carried across by address. Everything here works from what the code *does*.

Two matchers, of different strength:

  `exact_function_pairs`  strongest. Two functions whose entire normalised
                          instruction sequence is identical, and which are
                          each unique in their own ROM.
  `window_votes`          weaker but reaches further. Any run of N
                          instructions that is unique in both ROMs and
                          identical between them.

Both end up voting on "0461 symbol X is 9651 symbol Y". The planner decides
which votes are safe to act on.
"""

import difflib
from collections import defaultdict, Counter

from d8x_source import functions, instruction_stream, AUTO, LOCAL

DEFAULT_WINDOWS = (10, 8, 6)


def exact_function_pairs(src_path, dst_path):
    """Functions with an identical fingerprint, unique on both sides."""
    src, dst = functions(src_path), functions(dst_path)
    by_sig_src, by_sig_dst = defaultdict(list), defaultdict(list)
    for f in src:
        by_sig_src[f['sig']].append(f)
    for f in dst:
        by_sig_dst[f['sig']].append(f)
    return [(by_sig_src[sig][0], group[0])
            for sig, group in by_sig_dst.items()
            if sig in by_sig_src and len(group) == 1 and len(by_sig_src[sig]) == 1]


def fuzzy_function_pairs(src_path, dst_path, threshold=0.75, exclude=()):
    """Near-identical functions, for reporting only.

    Deliberately not fed into renaming: a 0.93 match once paired 9651's
    `adc_handler_pim` with a 0461 function that was not it, while 0461's real
    `adc_handler_pim` sat elsewhere.
    """
    src, dst = functions(src_path), functions(dst_path)
    used_src = {id(a) for a, _ in exclude}
    used_dst = {id(b) for _, b in exclude}
    remaining_src = [f for f in src if id(f) not in used_src]
    out = []
    for b in dst:
        if id(b) in used_dst:
            continue
        best, ratio = None, 0.0
        for a in remaining_src:
            if abs(len(a['sig']) - len(b['sig'])) > max(6, 0.4 * len(b['sig'])):
                continue
            r = difflib.SequenceMatcher(None, a['sig'], b['sig']).ratio()
            if r > ratio:
                best, ratio = a, r
        if best and ratio >= threshold:
            out.append((best, b, ratio))
    return out


def lockstep_votes(pairs):
    """Walk each matched pair instruction by instruction.

    Equal fingerprints mean equal instruction positions, so the symbols at a
    given position are the same symbol. Also returns, per destination
    function, the set of symbols its source twin references -- the planner
    uses that to justify an address embedded in a name.
    """
    votes = defaultdict(Counter)
    referenced = defaultdict(set)
    for a, b in pairs:
        for syms_a, syms_b in zip(a['syms'], b['syms']):
            if len(syms_a) != len(syms_b):
                continue
            for name_a, name_b in zip(syms_a, syms_b):
                if LOCAL.match(name_a) or LOCAL.match(name_b):
                    continue
                votes[name_b][name_a] += 1
        votes[b['name']][a['name']] += 1
        referenced[b['name']].update(s for group in a['syms'] for s in group)
    return votes, referenced


def window_votes(src_path, dst_path, sizes=DEFAULT_WINDOWS):
    """Vote from runs of instructions that are unique in both ROMs and equal.

    Needs no function boundaries, so it reaches chunked code. This is how
    `unk_23C` in 0461 was identified as `dmarx_iscv_duty`: that comparison
    sits inside a block IDA folded into `divide_d_by_x`, so there was no
    function pair to walk.
    """
    src, dst = instruction_stream(src_path), instruction_stream(dst_path)

    def index(stream, width):
        table = defaultdict(list)
        for i in range(len(stream) - width + 1):
            key = tuple((m, o) for m, o, _, _ in stream[i:i + width])
            table[key].append(i)
        return table

    votes = defaultdict(Counter)
    matched = 0
    for width in sizes:
        src_index, dst_index = index(src, width), index(dst, width)
        for key, src_at in src_index.items():
            dst_at = dst_index.get(key)
            if not dst_at or len(src_at) != 1 or len(dst_at) != 1:
                continue
            matched += 1
            for a, b in zip(src[src_at[0]:src_at[0] + width],
                            dst[dst_at[0]:dst_at[0] + width]):
                if len(a[2]) != len(b[2]):
                    continue
                for name_a, name_b in zip(a[2], b[2]):
                    votes[name_b][name_a] += 1
    return votes, matched


def clear_winners(votes, dominance=4):
    """Collapse each destination symbol's votes to a single source name.

    Accepts a lone candidate, or a leader with `dominance` times the votes of
    the runner-up. Anything closer than that is dropped rather than guessed.
    """
    out = {}
    for target, counter in votes.items():
        ranked = counter.most_common(2)
        if not ranked:
            continue
        if len(ranked) == 1 or ranked[0][1] >= dominance * ranked[1][1]:
            source, count = ranked[0]
            if not AUTO.match(source):
                out[target] = (source, count)
    return out


def dma_offset(cpu1_ram, cpu2_ram, direction):
    """Derive an ECU pair's inter-CPU DMA offset from its own name pairs.

    `direction` is 'cpu2_to_cpu1' (CPU2 `dmatx_X` = CPU1 `dmarx_X`) or
    'cpu1_to_cpu2' (CPU1 `dmatx_X` = CPU2 `dmarx_X`).

    These offsets are a property of the ECU pair, NOT of the family: the MR2
    pair uses +0xDA/+0x13B and the ST205 pair +0xD1/+0x133. Reusing one on the
    other pair lands inside a neighbouring variable and returns something that
    looks plausible. Always re-derive.

    Returns (modal offset, vote counter). Take the mode: a 16-bit variable
    whose name sits on a different byte of the pair will disagree by one.
    """
    if direction == 'cpu2_to_cpu1':
        sender, receiver = cpu2_ram, cpu1_ram
    elif direction == 'cpu1_to_cpu2':
        sender, receiver = cpu1_ram, cpu2_ram
    else:
        raise ValueError(f'unknown direction {direction!r}')

    stems_tx = {n[len('dmatx_'):] for n in sender if n.startswith('dmatx_')}
    stems_rx = {n[len('dmarx_'):] for n in receiver if n.startswith('dmarx_')}
    votes = Counter()
    for stem in stems_tx & stems_rx:
        tx = sender['dmatx_' + stem][0]
        rx = receiver['dmarx_' + stem][0]
        # both offsets are quoted in CLAUDE.md as CPU1_addr = CPU2_addr + N,
        # so normalise to "CPU1 minus CPU2" whichever way the data travels
        votes[(rx - tx) if direction == 'cpu2_to_cpu1' else (tx - rx)] += 1
    if not votes:
        return None, votes
    return votes.most_common(1)[0][0], votes
