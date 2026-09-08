"""Turning matcher votes into a rename plan that is safe to apply.

A wrong name in a disassembly is worse than no name -- it reads as a fact and
gets built on. So every rule here refuses rather than guesses, and each one is
the residue of an actual mistake. See README.md for the incident behind each.
"""

import re
from collections import defaultdict

from d8x_source import (ram_symbols, defined_labels, AUTO, plausible_address)

#: An address embedded in a name, e.g. the 187 in `inc_cnt_187`. Uppercase
#: only: `adc` is three valid hex digits but is not an address.
FRAGMENT = re.compile(r'(?<=_)([0-9A-F]{2,4})(?=_|$)')
IDENTIFIER = re.compile(r'^[A-Za-z_]\w*$')


def _self_address(name):
    """The address an IDA auto-name encodes: unk_9E -> 0x9E."""
    m = re.match(r'^(?:unk|byte|word|off|dword|sub|loc|locret)_([0-9A-Fa-f]+)$', name)
    return int(m.group(1), 16) if m else None


def _tail_address(name):
    """The trailing address of a hand-name: table_ect_C185 -> 0xC185."""
    m = re.search(r'_([0-9A-F]{4})$', name)
    value = int(m.group(1), 16) if m else None
    return value if plausible_address(value) else None


def address_of(name, ram):
    a = ram.get(name, (None,))[0]
    if a is not None:
        return a
    return _self_address(name) if AUTO.match(name) else _tail_address(name)


def build_address_map(votes, src_ram, dst_ram):
    """Map source addresses to destination addresses, from confident pairs only.

    Also seeds identity entries for symbols already named the same in both,
    which is what lets a name like `var_flags_4E_saved` port unchanged.
    """
    mapping = {}
    for target, counter in votes.items():
        if len(counter) != 1:
            continue
        src_addr = address_of(next(iter(counter)), src_ram)
        dst_addr = address_of(target, dst_ram)
        if src_addr is not None and dst_addr is not None:
            mapping[src_addr] = dst_addr
    for name in set(src_ram) & set(dst_ram):
        if not AUTO.match(name):
            mapping.setdefault(src_ram[name][0], dst_ram[name][0])
    return mapping


def adapt_name(source_name, target_name, address_map, src_ram, dst_ram,
               justified_addresses=frozenset()):
    """Rewrite the addresses embedded in a source name for the target ROM.

    Returns (new name, [unresolved fragments]). A fragment is rewritten when:

      * it is the symbol's OWN address -- always, and this takes precedence
        over everything else. An unrelated identity entry once left
        `var_cnt_CE` unchanged when it belonged at `var_cnt_C8`, and only a
        name collision revealed it.
      * it names something the matched source function references, and the
        caller vouched for it via `justified_addresses`. This is what makes
        `inc_cnt_187` (which names the counter it increments) portable.
      * it maps to itself, i.e. the same address in both ROMs: a no-op.

    Everything else is refused. In particular the CPU2 half of a name like
    `dmarx_max_retard_23B_161` must never be rewritten with a CPU1 map -- use
    `matchers.dma_offset` for that, and note the offset differs per ECU pair.
    """
    own_src = address_of(source_name, src_ram)
    own_dst = address_of(target_name, dst_ram)
    new, unresolved = source_name, []
    for fragment in FRAGMENT.findall(source_name):
        value = int(fragment, 16)
        if not plausible_address(value):
            continue                      # a threshold or a constant, not an address
        if own_src is not None and value == own_src:
            if own_dst is None:
                unresolved.append(f'{fragment} (no target address)')
                continue
            new = re.sub(r'(?<=_)' + fragment + r'(?=_|$)', f'{own_dst:X}', new, count=1)
        elif value in justified_addresses and value in address_map:
            new = re.sub(r'(?<=_)' + fragment + r'(?=_|$)',
                         f'{address_map[value]:X}', new, count=1)
        elif address_map.get(value) == value:
            continue                      # same address in both ROMs
        elif value in address_map or len(fragment) >= 3:
            # Either a known address that nothing vouches for, or a 3-4 digit
            # run which in these names is essentially always an address -- the
            # 166 in `dmarx_ign_timing_unk_166` is the CPU2 side of a DMA
            # pair. Both are indistinguishable from a referenced CPU1 variable
            # at this strength of evidence, so refuse rather than guess.
            unresolved.append(f'{fragment} (unjustified)')
        else:
            # A two-digit run that is not an address anywhere in this ROM pair
            # is a scale factor, as in `divide_rD_32`. Leave it alone. (Real
            # two-digit addresses in these names are the symbol's own, and
            # were handled above.)
            continue
    return new, unresolved


def plan_renames(votes, src_path, dst_path, referenced=None, min_votes=1,
                 trust_referenced=False):
    """Decide which votes become renames.

    `trust_referenced` allows the second adaptation rule above; only turn it on
    for exact-function evidence, where `referenced` says what the source twin
    actually touched. Windowed evidence is not strong enough to distinguish a
    referenced CPU1 variable from a CPU2 address, so it runs without it.

    Returns (new, updated, unchanged, rejected).
    """
    src_ram, dst_ram = ram_symbols(src_path), ram_symbols(dst_path)
    address_map = build_address_map(votes, src_ram, dst_ram)
    existing = defined_labels(dst_path) | set(dst_ram)
    referenced = referenced or {}

    winners = {}
    for target, counter in votes.items():
        if len(counter) != 1:
            continue
        source = next(iter(counter))
        if not AUTO.match(source):
            winners[target] = (source, counter[source])

    # a source name claimed by two different targets is a contradiction
    claimants = defaultdict(list)
    for target, (source, _) in winners.items():
        claimants[source].append(target)

    new, updated, unchanged, rejected = [], [], [], []
    for target, (source, count) in winners.items():
        if count < min_votes:
            rejected.append((target, source, count, 'too few votes'))
            continue
        if len(claimants[source]) > 1:
            rejected.append((target, source, count,
                             'source also claimed by ' + ', '.join(
                                 t for t in claimants[source] if t != target)))
            continue
        if source == target:
            unchanged.append(target)
            continue

        justified = set()
        if trust_referenced:
            for name in referenced.get(target, ()):
                addr = address_of(name, src_ram)
                if addr is not None:
                    justified.add(addr)

        candidate, unresolved = adapt_name(source, target, address_map,
                                           src_ram, dst_ram, justified)
        if unresolved:
            rejected.append((target, source, count, '; '.join(unresolved)))
            continue
        if not IDENTIFIER.match(candidate):
            # IDA writes some operands as expressions, e.g.
            # `#(table_knock_retard_step-1)`. One of those became a label
            # definition once and broke the assembly.
            rejected.append((target, candidate, count, 'not a valid identifier'))
            continue
        if candidate == target:
            unchanged.append(target)      # already correctly adapted
            continue
        if candidate in existing:
            rejected.append((target, candidate, count, 'name already used in target'))
            continue
        (new if AUTO.match(target) else updated).append(
            [target, candidate, count,
             'verbatim' if candidate == source else f'adapted from {source}'])

    # two targets resolving to one name would collapse two symbols into one
    by_name = defaultdict(list)
    for row in new + updated:
        by_name[row[1]].append(row[0])
    collisions = {name: targets for name, targets in by_name.items() if len(targets) > 1}
    new = [r for r in new if r[1] not in collisions]
    updated = [r for r in updated if r[1] not in collisions]
    for name, targets in collisions.items():
        rejected.append((', '.join(targets), name, 0, 'two symbols map to one name'))

    return new, updated, unchanged, rejected


def apply_renames(path, renames):
    """Rewrite a source file, applying every rename in one simultaneous pass.

    Simultaneous matters: a batch can legitimately contain A->B while B->C,
    and applying those in sequence would merge them.
    """
    for target, name in renames.items():
        if not IDENTIFIER.match(name):
            raise ValueError(f'refusing to write invalid identifier {name!r}')
    if len(set(renames.values())) != len(renames):
        raise ValueError('two symbols would be renamed to the same name')

    from d8x_source import read
    text = read(path)
    existing = set(re.findall(r'^([A-Za-z_]\w*):', text, re.M))
    clashes = {t: n for t, n in renames.items() if n in existing and n != t}
    if clashes:
        raise ValueError(f'target names already defined in {path}: {clashes}')

    pattern = re.compile(
        r'\b(' + '|'.join(sorted(map(re.escape, renames), key=len, reverse=True)) + r')\b')
    counts = defaultdict(int)

    def substitute(match):
        counts[match.group(1)] += 1
        return renames[match.group(1)]

    out = pattern.sub(substitute, text)
    with open(path, 'w', encoding='utf-8', newline='') as handle:
        handle.write(out)
    return dict(counts)
