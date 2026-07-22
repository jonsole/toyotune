#!/usr/bin/env python3

from lexer import MathEvaluator, MathEvaluatorError
import instruction, directive
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

    def _RunPass(self, context, lines):
        """Run one assembly pass over the already-split source lines against
        the given AssemblerPass. Returns True if an unexpected (non-
        AssemblyError) exception aborted the pass early."""
        for line_num, line in enumerate(lines, 1):

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
    def Assemble(self, input, output, listing, format="bin"):
        self.diagnostics = []
        try:
            return self._Assemble(input, output, listing, format)
        finally:
            output.close()

    def _Assemble(self, input, output, listing, format):

        lines = [line.rstrip('\n') for line in input]

        # Pass 1 establishes an initial guess at every label's address.
        # Forward references to labels not yet seen fall back to the
        # current pc as a placeholder (AssemblerPass.ParseExpression),
        # which is corrected on the next pass once every label is known.
        print("Assembling Pass 1...")
        current_pass = AssemblerPass()
        aborted = self._RunPass(current_pass, lines)
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
            aborted = self._RunPass(next_pass, lines)
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

                    # fill space between last block and this one
                    output.write(bytearray(block_addr - output_addr))

                    # output block
                    output.write(b['bytes'])

                    # calculate address after block
                    output_addr = block_addr + len(b['bytes'])
        else:
            raise AssemblyError("Unsupported output format \'{}\'".format(format))

        return len(lines)

if __name__ == '__main__':
    import sys
    import argparse
    import time

    parser = argparse.ArgumentParser(description='Denso 8x Assembler v0.1')

    parser.add_argument('input', type=argparse.FileType(mode='r', encoding="ascii", errors="surrogateescape"), default=sys.stdin, help="Input file")
    parser.add_argument('output', type=argparse.FileType(mode='wb'), help="Output file")
    parser.add_argument('listing', nargs='?', type=argparse.FileType('w', encoding="ascii", errors="surrogateescape"), help="Listing file")
    parser.add_argument('-f', '--format', choices=["bin", "obj"], default="bin") 
    args = parser.parse_args()

    time_start = time.perf_counter()
    asm = Assembler()
    num_lines = asm.Assemble(args.input, args.output, args.listing, args.format)
    time_stop = time.perf_counter()
    time_elapsed = time_stop - time_start
    print("Processed {0} lines in {1:.2f} seconds, {2:.2f} lines/sec".format(num_lines, time_elapsed, num_lines / time_elapsed))

    if asm.diagnostics:
        print("Assembly failed with {0} error(s).".format(len(asm.diagnostics)))
        sys.exit(1)