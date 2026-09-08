"""Parsing for D8X `.ASM`/`.asm` disassembly sources.

Handles both encodings found in this repo: the `Claude/` working copies are
UTF-8, every other `.ASM` is raw CP437 (see CLAUDE.md). Reading always goes
through `read()`, which decodes CP437 as a fallback and maps IDA's 0x18/0x19
xref-direction glyphs to real arrows so the two encodings compare equal.
"""

import re

CODE_ORG = re.compile(r'^\s*\.org\s+0C000h', re.I)
END_FUNC = re.compile(r'^;\s*End of function\s+(\S+)')
LABEL    = re.compile(r'^([A-Za-z_]\w*):')
NUMERIC  = re.compile(r'^[0-9][0-9A-Fa-f]*h$|^[0-9]+$', re.I)
BITREF   = re.compile(r'^bit[0-7]$', re.I)
REGISTER = frozenset('abdxysp')

#: IDA's auto-generated names, i.e. "not yet understood".
AUTO = re.compile(r'^(?:sub|loc|locret|unk|byte|word|off|dword|nullsub)_[0-9A-Fa-f]+$')
LOCAL = re.compile(r'^(?:loc|locret)_[0-9A-Fa-f]+$')

#: RAM lives below 0x400, ROM at 0xC000-0xFFFF. Anything else in a symbol name
#: is not an address -- `igf_count_rpm_lt_3000` names an RPM threshold.
def plausible_address(value):
    return value is not None and (value <= 0x3FF or 0xC000 <= value <= 0xFFFF)


def read(path):
    """Decode a source file to text, whichever of the two encodings it uses."""
    with open(path, 'rb') as handle:
        raw = handle.read()
    try:
        return raw.decode('utf-8')
    except UnicodeDecodeError:
        return raw.decode('cp437').replace('\x18', '↑').replace('\x19', '↓')


def numeric(token):
    """`0Ch` is 12, `12` is 12. Getting this wrong silently shifts a RAM map."""
    token = token.strip()
    return int(token[:-1], 16) if token.lower().endswith('h') else int(token, 10)


def code_of(line):
    """The instruction part of a line, comment stripped."""
    return line.split(';')[0].rstrip()


def normalise_operand(operand):
    """Return (text with symbols replaced by SYM, [symbols in operand order]).

    Numeric immediates are kept, because they distinguish otherwise identical
    routines. Splitting on brackets and arithmetic matters: IDA writes some
    operands as expressions such as `#(table_knock_retard_step-1)`, and
    treating one of those as a single symbol will corrupt a rename.
    """
    out, symbols = [], []
    for token in re.split(r'([,\[\]#+\-()\s]+)', operand):
        if not token or re.match(r'^[,\[\]#+\-()\s]+$', token):
            out.append(token)
        elif NUMERIC.match(token):
            out.append(token.lower())
        elif len(token) == 1 and token.lower() in REGISTER:
            out.append(token.lower())
        elif BITREF.match(token):
            out.append(token.lower())
        else:
            out.append('SYM')
            symbols.append(token)
    return ''.join(out).replace(' ', ''), symbols


def ram_symbols(path):
    """Map every data-segment symbol to (address, size).

    Walks `.org` and `.block` from the top of the file to the start of ROM.
    Validated by the IDA auto-names, which encode their own address: for any
    `unk_9E` the computed address must be 0x9E (see tests).
    """
    symbols, address = {}, 0
    for line in read(path).splitlines():
        code = code_of(line)
        if not code.strip():
            continue
        org = re.match(r'^\s*\.org\s+([0-9A-Fa-f]+h?)\s*$', code, re.I)
        if org:
            address = numeric(org.group(1))
            if address >= 0xC000:
                break
            continue
        named = re.match(r'^([A-Za-z_]\w*):\s*\.block\s+(\S+)', code)
        if named:
            size = numeric(named.group(2))
            symbols[named.group(1)] = (address, size)
            address += size
            continue
        anon = re.match(r'^\s+\.block\s+(\S+)', code)
        if anon:
            address += numeric(anon.group(1))
            continue
        bare = re.match(r'^([A-Za-z_]\w*):\s*$', code)
        if bare:
            symbols[bare.group(1)] = (address, 0)
    return symbols


def functions(path):
    """Split the code segment into functions.

    Each is {name, sig, syms, start, end}. `sig` is the tuple of
    (mnemonic, normalised operand) pairs -- the fingerprint two ROMs are
    matched on. `syms[i]` holds the symbols of instruction i, so a pair of
    functions with equal `sig` can be walked in lockstep.

    Note this only sees code IDA recognised as a function. Chunks folded into
    a giant routine are invisible here, which is what `windows()` is for.
    """
    in_code, found = False, []
    current = {'insns': [], 'syms': [], 'start': None}
    for lineno, line in enumerate(read(path).splitlines(), 1):
        if not in_code:
            if CODE_ORG.match(code_of(line)):
                in_code = True
            continue
        end = END_FUNC.match(line)
        if end:
            if current['insns']:
                current.update(name=end.group(1), end=lineno,
                               sig=tuple(current['insns']))
                found.append(current)
            current = {'insns': [], 'syms': [], 'start': None}
            continue
        code = code_of(line)
        if not code.strip():
            continue
        label = LABEL.match(code)
        if label:
            if current['start'] is None:
                current['start'] = lineno
            code = code[label.end():]
        if not code.strip():
            continue
        parts = code.split(None, 1)
        mnemonic = parts[0].lower()
        operand, symbols = normalise_operand(parts[1]) if len(parts) > 1 else ('', [])
        if mnemonic.startswith('.'):
            operand = 'D:' + operand
        if current['start'] is None:
            current['start'] = lineno
        current['insns'].append((mnemonic, operand))
        current['syms'].append(symbols)
    return found


def instruction_stream(path):
    """The whole code segment as one flat list of (mnemonic, operand, syms, line).

    Ignores function boundaries entirely, so it reaches code the function
    parser cannot -- for instance a block IDA folded into `divide_d_by_x`.
    """
    in_code, out = False, []
    for lineno, line in enumerate(read(path).splitlines(), 1):
        if not in_code:
            if CODE_ORG.match(code_of(line)):
                in_code = True
            continue
        code = code_of(line)
        if not code.strip():
            continue
        label = LABEL.match(code)
        if label:
            code = code[label.end():]
        if not code.strip():
            continue
        parts = code.split(None, 1)
        mnemonic = parts[0].lower()
        operand, symbols = normalise_operand(parts[1]) if len(parts) > 1 else ('', [])
        if mnemonic.startswith('.'):
            operand = 'D:' + operand
        out.append((mnemonic, operand, symbols, lineno))
    return out


def defined_labels(path):
    """Every label defined at the start of a line."""
    return set(re.findall(r'^([A-Za-z_]\w*):', read(path), re.M))
