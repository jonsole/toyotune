#!/usr/bin/env python3

from lexer import MathEvaluator, MathEvaluatorError
import instruction, directive, macro, define
import collections
import re

# Exception used for errors
class AssemblyError(Exception): pass

# A single reported problem. 'line' is None for whole-assembly issues that
# aren't tied to one source line (e.g. failure to converge on a stable set
# of label addresses). 'level' is 'error' for a normal AssemblyError or
# 'internal' for an unexpected exception.
Diagnostic = collections.namedtuple('Diagnostic', ['line', 'level', 'message'])

# Upper bound on assembly passes. Two passes resolve every ROM seen so far;
# more are only needed if a forward-referenced label's address feeds back
# into the size of the instruction that references it (e.g. an 8-bit vs.
# 16-bit absolute addressing choice), shifting later label addresses enough
# to require re-resolving. This just bounds how long a genuinely
# non-converging source is retried before giving up.
MAX_PASSES = 10

instruction_re = re.compile(r'^(?P<opcode>[a-zA-Z]+)(\s*)(?P<operands>.+)?$')
directive_re = re.compile(r'^\.(?P<directive>[a-zA-Z]+)(\s*)(?P<arguments>.+)?$')
line_re = re.compile(r'^((?P<label>[.a-zA-Z0-9_$]+):?)?(\s*)((?P<directive>[.].*?)|(?P<instruction>[^.;].*?))?(\s*)(;\s*(?P<comment>.*)\s*)?$')
equ_directive_re = re.compile(r'^\.\s*equ\b', re.IGNORECASE)

# Splits an 'instruction'-position string into a leading name and the rest,
# used only to recognize macro invocations (macro *definitions* are the
# dot-prefixed '.macro'/'.endm'/'.local', handled via directive_re below).
# Unlike instruction_re (opcodes are always plain letters), this accepts the
# same character set as a label, since macro names follow this codebase's
# label naming convention (e.g. 'save_regs') rather than a fixed opcode
# mnemonic.
macro_name_re = re.compile(r'^(?P<name>[.a-zA-Z0-9_$]+)(\s*)(?P<args>.+)?$')


class AssemblerPass(object):

    def __init__(self, previous_pass=None, listing=None):
        if previous_pass:
            self._pass = previous_pass._pass + 1
            self._labels = previous_pass._labels
        else:
            self._pass = 1
            self._labels = {}

        self._bytes_pc = self._pc = 0
        self._bytes = bytearray()
        self._listing = listing
        self._blocks = []
        self._is_end = False
        self._labels_changed = False

        # initialise expression parser
        self.parser = MathEvaluator()

    def ParseExpression(self, expression):
        self._labels['pc'] = self._pc
        try:
            return self.parser.Evaluate(expression, variables=self._labels)
        except MathEvaluatorError as e:
            # If it is first pass, ignore error as it may be a forward reference
            # to a label not yet defined
            if self._pass == 1:
                return self._pc
            else:
                raise AssemblyError(e.args)

    def SetLabel(self, label, value):
        # A label resolving to a different address than it did on the
        # previous pass just means another pass is needed to re-resolve
        # everything against the corrected address (see Assembler.Assemble);
        # it isn't reported as an error here. Assembler.Assemble gives up
        # and reports a real error only if this keeps happening after
        # MAX_PASSES attempts.
        v = self._labels.get(label)
        if v is not None and v != value:
            self._labels_changed = True
        self._labels[label] = value

    @property
    def LabelsChanged(self):
        """True if any label resolved to a different address than it did on
        the previous pass (this pass's labels dict starts as a copy of the
        previous pass's, so unchanged labels never hit this)."""
        return self._labels_changed

    @property
    def Pc(self):
        return self._pc

    def SetPc(self, pc):
        if self._bytes:
            self._blocks.append({'address':self._bytes_pc, 'bytes':self._bytes})
        self._bytes_pc = self._pc = pc
        self._bytes = bytearray()

    def AddBytes(self, bytes):
        self._bytes.extend(bytes)
        self._pc += len(bytes)

    def LineBegin(self, line_num, line_str):
        if self._listing:
            self._line_num = line_num
            self._line_str = line_str
            self._line_pc = self._pc
            self._line_bytes_pos = len(self._bytes)

    def LineEnd(self, line_str):
        if self._listing:
            line_bytes = self._bytes[self._line_bytes_pos:]

            while True:
                line_hex = line_bytes[:4].hex(sep=' ').upper()
                print("{:0>5d}  {:0>4X}  {:<12} {}".format(self._line_num, self._line_pc, line_hex, self._line_str), file=self._listing)
                line_bytes = line_bytes[4:]
                if not line_bytes:
                    break
                self._line_str = ''

    def End(self):
        self.SetPc(0)
        self._is_end = True

    @property
    def IsEnd(self):
        return self._is_end


def _ReconstructLine(parsed_line):
    """Turn a macro-expanded ParsedLine back into a source-line string, so
    the normal line_re-based per-line parsing can run over it exactly as it
    would over real source. Only used for macro-expanded lines - source
    lines untouched by macro expansion are passed through as their exact
    original text instead of going through this.
    """
    parts = []
    if parsed_line.label:
        parts.append(parsed_line.label + ':')
    if parsed_line.directive:
        parts.append(parsed_line.directive)
    if parsed_line.instruction:
        parts.append(parsed_line.instruction)
    text = '\t'.join(parts)
    # Without a label, a leading tab is required: line_re's label group has
    # no other way to tell "bare opcode at column 0" apart from "label with
    # no colon" other than a preceding whitespace character forcing it to
    # match empty. Every hand-written line in this codebase already starts
    # instructions with a tab for this reason; a label followed by ':' has
    # no such ambiguity, so it needs no extra separator.
    if not parsed_line.label and text:
        text = '\t' + text
    if parsed_line.comment is not None:
        text += ('\t' if text else '') + '; ' + parsed_line.comment
    return text


class Assembler(object):

    def __init__(self):
        self.labels = {}
        self.pc = 0
        self.diagnostics = []

    @property
    def errors(self):
        """Number of problems reported by the most recent Assemble() call.
        Zero means assembly succeeded and output was written."""
        return len(self.diagnostics)

    def HandleInstruction(self, context, instruction_str):
        try:
            m = instruction_re.match(instruction_str)
            if m:
                opcode_str = m.group('opcode')
                operands_str = m.group('operands')
                instruction.Assemble(context, opcode_str, operands_str)
        except instruction.InstructionError as e:
            raise AssemblyError(e)

    def HandleLabel(self, context, label_str):
        context.SetLabel(label_str, context.Pc)

    def HandleDirective(self, context, directive_str, label_str=None):
        try:
            m = directive_re.match(directive_str)
            if m:
                directive_str = m.group('directive')
                arguments_str = m.group('arguments')
                directive.Assemble(context, directive_str, arguments_str, label_str)
        except directive.DirectiveError as e:
            raise AssemblyError(e)

    def _ReportError(self, line_num, message):
        self.diagnostics.append(Diagnostic(line_num, 'error', str(message)))
        if line_num is not None:
            print("{0:4d} : Error: {1}".format(line_num, message))
        else:
            print("Error: {0}".format(message))

    def _ReportInternalError(self, line_num, exc):
        self.diagnostics.append(Diagnostic(line_num, 'internal', repr(exc)))
        print("{0:4d} : Internal error: {1}".format(line_num, repr(exc)))

    def _ExpandDefines(self, lines):
        """Preprocess raw source lines: extract '.define NAME ...' /
        '.define NAME(a,b) ...' declarations and substitute their uses
        everywhere else in the file, C-preprocessor style. Runs before
        _ExpandMacros, so a '.define' can be used inside a macro body or in
        a macro invocation's arguments - by the time _ExpandMacros sees the
        source, all '.define' uses are already plain text.

        Returns (lines, errors). Unlike _ExpandMacros this never changes
        the line count or the meaning of any other line's position: each
        '.define' declaration line is blanked out in place (not removed),
        so no line-number remapping is needed here.
        """
        processor = define.DefineProcessor()
        result = []
        errors = []

        for line_num, line in enumerate(lines, 1):
            m = line_re.match(line)
            directive_str = m.group('directive') if m else None
            dm = directive_re.match(directive_str) if directive_str else None
            directive_name = dm.group('directive').lower() if dm else None

            try:
                if directive_name == 'define':
                    processor.Define(dm.group('arguments'), line_num)
                    result.append('')
                    continue

                if not m or not (m.group('directive') or m.group('instruction')):
                    result.append(line)
                    continue

                orig_directive = m.group('directive')
                orig_instruction = m.group('instruction')
                new_directive = processor.ExpandText(orig_directive)
                new_instruction = processor.ExpandText(orig_instruction)

                if new_directive == orig_directive and new_instruction == orig_instruction:
                    result.append(line)
                else:
                    result.append(_ReconstructLine(macro.ParsedLine(
                        line_num, m.group('label'), new_directive,
                        new_instruction, m.group('comment'))))
            except define.DefineError as e:
                errors.append((line_num, str(e)))
                result.append(line)

        return result, errors

    def _ExpandMacros(self, lines):
        """Preprocess raw source lines: extract '.macro' / ... / '.endm'
        definitions (with optional '.local sym,...' lines) and expand
        invocations, entirely before the real two-pass assembler ever sees
        them - macros are a pure source-to-source transform.

        Returns (expanded_lines, line_nums, errors). 'line_nums' gives the
        original 1-based source line each entry of 'expanded_lines' came
        from (needed for diagnostics/listing once expansion changes the
        line count). 'errors' is a list of (line_num, message) pairs, non-
        empty only on failure, in which case the other two return values
        are unusable (callers should report the errors and stop).

        Source lines untouched by macros are passed through as their exact
        original text; only macro-expanded lines are reconstructed from
        substituted parts (see _ReconstructLine).
        """
        processor = macro.MacroProcessor()
        Raw = collections.namedtuple('Raw', ['line_num', 'text'])
        Invocation = collections.namedtuple('Invocation', ['line_num', 'label', 'name', 'args'])

        pending = []
        errors = []

        for line_num, line in enumerate(lines, 1):
            m = line_re.match(line)
            label = m.group('label') if m else None
            directive_str = m.group('directive') if m else None
            instr = m.group('instruction') if m else None
            comment = m.group('comment') if m else None

            # '.macro'/'.endm'/'.local' are directives like every other
            # dot-prefixed pseudo-op in this dialect (.org, .equ, .db, ...);
            # only macro *invocations* are written bare, like a real
            # instruction mnemonic.
            dm = directive_re.match(directive_str) if directive_str else None
            directive_name = dm.group('directive').lower() if dm else None
            directive_args = dm.group('arguments') if dm else None

            try:
                if processor.InDefinition:
                    if directive_name == 'endm':
                        processor.EndDefinition()
                    elif directive_name == 'local':
                        processor.AddLocal(directive_args)
                    else:
                        processor.AddBodyLine(macro.ParsedLine(
                            line_num, label, directive_str, instr, comment))
                    continue

                if directive_name == 'macro':
                    processor.BeginDefinition(label, line_num)
                    continue

                nm = macro_name_re.match(instr) if instr else None
                if nm and processor.IsMacro(nm.group('name')):
                    pending.append(Invocation(line_num, label, nm.group('name'), nm.group('args')))
                    continue

                pending.append(Raw(line_num, line))
            except macro.MacroError as e:
                errors.append((line_num, str(e)))

        if processor.InDefinition:
            errors.append((processor.CurrentDefinitionLine, "'.macro' without matching '.endm'"))

        if errors:
            return lines, list(range(1, len(lines) + 1)), errors

        # A macro body may itself invoke another macro, so keep re-scanning
        # the expanded output until a pass produces no further expansions
        # (or MAX_EXPANSION_DEPTH gives up on what must be a macro that
        # invokes itself, directly or indirectly).
        for _ in range(macro.MAX_EXPANSION_DEPTH):
            changed = False
            next_pending = []
            for item in pending:
                if isinstance(item, Invocation):
                    changed = True
                    try:
                        body = processor.Expand(item.name, item.args, item.label, item.line_num)
                    except macro.MacroError as e:
                        errors.append((item.line_num, str(e)))
                        continue
                    for pl in body:
                        nm = macro_name_re.match(pl.instruction) if pl.instruction else None
                        if nm and processor.IsMacro(nm.group('name')):
                            next_pending.append(Invocation(pl.line_num, pl.label, nm.group('name'), nm.group('args')))
                        else:
                            next_pending.append(Raw(pl.line_num, _ReconstructLine(pl)))
                else:
                    next_pending.append(item)
            pending = next_pending
            if errors or not changed:
                break
        else:
            errors.append((None, "Macro expansion nested more than {} levels deep "
                                  "(a macro invoking itself, directly or indirectly?)".format(
                                      macro.MAX_EXPANSION_DEPTH)))

        if errors:
            return lines, list(range(1, len(lines) + 1)), errors

        return [item.text for item in pending], [item.line_num for item in pending], []

    def _RunPass(self, context, lines, line_nums=None):
        """Run one assembly pass over the already-split source lines against
        the given AssemblerPass. Returns True if an unexpected (non-
        AssemblyError) exception aborted the pass early."""
        if line_nums is None:
            line_nums = range(1, len(lines) + 1)
        for line_num, line in zip(line_nums, lines):

            context.LineBegin(line_num, line)
            try:
                m = line_re.match(line)
                if m:
                    label_str = m.group('label')
                    directive_str = m.group('directive')
                    instruction_str = m.group('instruction')
                    # 'equ' binds its label to an expression's value rather
                    # than the current PC, so withhold the normal PC-based
                    # label assignment and let HandleDirective/HandleEqu set
                    # it instead.
                    is_equ = bool(directive_str) and equ_directive_re.match(directive_str)
                    if label_str and not is_equ:
                        self.HandleLabel(context, label_str)
                    if directive_str:
                        self.HandleDirective(context, directive_str, label_str)
                    if instruction_str:
                        self.HandleInstruction(context, instruction_str)
            except AssemblyError as e:
                self._ReportError(line_num, e)
            except Exception as e:
                self._ReportInternalError(line_num, e)
                context.LineEnd(line)
                return True
            context.LineEnd(line)

            if context.IsEnd:
                break

        return False

    # Parse lines into intermediate object code
    def Assemble(self, input, output, listing, format="bin", fill_byte=0x00):
        self.diagnostics = []
        try:
            return self._Assemble(input, output, listing, format, fill_byte)
        finally:
            output.close()

    def _Assemble(self, input, output, listing, format, fill_byte=0x00):

        lines = [line.rstrip('\n') for line in input]

        # '.define' and macro expansion are one-time source-to-source
        # transforms (neither depends on label addresses), so both run once
        # here rather than being repeated on every pass below. Defines are
        # resolved first so their uses inside a macro body or macro
        # invocation arguments are already plain text by the time macro
        # expansion runs.
        lines, define_errors = self._ExpandDefines(lines)
        if define_errors:
            for line_num, message in define_errors:
                self._ReportError(line_num, message)
            return len(lines)

        lines, line_nums, macro_errors = self._ExpandMacros(lines)
        if macro_errors:
            for line_num, message in macro_errors:
                self._ReportError(line_num, message)
            return len(lines)

        # Pass 1 establishes an initial guess at every label's address.
        # Forward references to labels not yet seen fall back to the
        # current pc as a placeholder (AssemblerPass.ParseExpression),
        # which is corrected on the next pass once every label is known.
        print("Assembling Pass 1...")
        current_pass = AssemblerPass()
        aborted = self._RunPass(current_pass, lines, line_nums)
        current_pass.End()

        if aborted or self.diagnostics:
            return len(lines)

        # Re-resolve against the previous pass's labels, retrying with a
        # fresh pass each time a label's resolved address changes (which
        # can happen if an addressing mode with a size that depends on the
        # label's value, e.g. 8-bit vs. 16-bit absolute, was guessed wrong
        # using the pass-1 placeholder). The listing/output are only ever
        # generated from the pass that turns out to be stable.
        for attempt in range(2, MAX_PASSES + 1):
            print("Assembling Pass {}...".format(attempt))

            if listing:
                listing.seek(0)
                listing.truncate()

            next_pass = AssemblerPass(current_pass, listing)
            aborted = self._RunPass(next_pass, lines, line_nums)
            next_pass.End()

            if aborted or self.diagnostics:
                return len(lines)

            current_pass = next_pass
            if not current_pass.LabelsChanged:
                break
        else:
            self._ReportError(
                None,
                "Assembly did not converge after {} passes; label "
                "addresses kept changing between passes".format(MAX_PASSES))
            return len(lines)

        # TODO: handle various output formats
        if format == "bin":

            if current_pass._blocks:

                # get address of first block
                output_addr = current_pass._blocks[0]['address']

                # iterate through all the blocks
                for b in current_pass._blocks:
                    block_addr = b['address']

                    # fill space between last block and this one - matches
                    # tasm32's '-f<xx>' flag, which fills unused memory
                    # space with a chosen byte (this Makefile pipeline uses
                    # -f5F) rather than leaving it zeroed. Only the *gap
                    # between* blocks is filled; nothing is emitted before
                    # the first block's own address.
                    output.write(bytes([fill_byte]) * (block_addr - output_addr))

                    # output block
                    output.write(b['bytes'])

                    # calculate address after block
                    output_addr = block_addr + len(b['bytes'])
        else:
            raise AssemblyError("Unsupported output format \'{}\'".format(format))

        return len(lines)

def _HexByte(text):
    try:
        value = int(text, 16)
    except ValueError:
        raise argparse.ArgumentTypeError("'{}' is not a valid hex byte".format(text))
    if not 0 <= value <= 0xFF:
        raise argparse.ArgumentTypeError("'{}' is out of range for a byte (00-FF)".format(text))
    return value


if __name__ == '__main__':
    import sys
    import argparse
    import time

    parser = argparse.ArgumentParser(description='Denso 8x Assembler v0.1')

    parser.add_argument('input', type=argparse.FileType(mode='r', encoding="ascii", errors="surrogateescape"), default=sys.stdin, help="Input file")
    parser.add_argument('output', type=argparse.FileType(mode='wb'), help="Output file")
    parser.add_argument('listing', nargs='?', type=argparse.FileType('w', encoding="ascii", errors="surrogateescape"), help="Listing file")
    parser.add_argument('-f', '--format', choices=["bin", "obj"], default="bin")
    parser.add_argument('-p', '--fill', type=_HexByte, default=0x00, metavar='XX',
                         help="Hex byte (e.g. 5F) to fill unused memory space between .org "
                              "blocks with, matching tasm32's '-f<xx>' flag. Default: 00")
    args = parser.parse_args()

    time_start = time.perf_counter()
    asm = Assembler()
    num_lines = asm.Assemble(args.input, args.output, args.listing, args.format, args.fill)
    time_stop = time.perf_counter()
    time_elapsed = time_stop - time_start
    print("Processed {0} lines in {1:.2f} seconds, {2:.2f} lines/sec".format(num_lines, time_elapsed, num_lines / time_elapsed))

    if asm.diagnostics:
        print("Assembly failed with {0} error(s).".format(len(asm.diagnostics)))
        sys.exit(1)