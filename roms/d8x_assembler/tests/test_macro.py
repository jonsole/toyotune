import contextlib
import io
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))

from asm_d8x import Assembler


class NonClosingBytesIO(io.BytesIO):
    """BytesIO whose close() is a no-op, so the test can still call
    getvalue() after Assembler.Assemble() closes its output handle."""
    def close(self):
        pass


def run_assembler(source):
    asm = Assembler()
    output = NonClosingBytesIO()
    stdout = io.StringIO()
    with contextlib.redirect_stdout(stdout):
        num_lines = asm.Assemble(io.StringIO(source), output, None, "bin")
    return num_lines, output.getvalue(), stdout.getvalue(), asm


class TestBasicExpansion(unittest.TestCase):

    def test_no_argument_macro_expands_inline(self):
        source = (
            "clear_a\t.macro\n"
            "\tclr\ta\n"
            "\t.endm\n"
            "\t.org 0\n"
            "\tclear_a\n"
            "\tnop\n"
        )
        num_lines, data, log, asm = run_assembler(source)
        self.assertEqual(asm.diagnostics, [])
        self.assertEqual(data, b'\x52\x00')

    def test_positional_parameter_substitution(self):
        # ']1'/']2' (not '#1'/'#2') because '#nn' already means an
        # immediate operand in this ISA - see macro.py.
        source = (
            "ld_both\t.macro\n"
            "\tld\ta,#]1\n"
            "\tld\tb,#]2\n"
            "\t.endm\n"
            "\t.org 0\n"
            "\tld_both\t5,10\n"
        )
        num_lines, data, log, asm = run_assembler(source)
        self.assertEqual(asm.diagnostics, [])
        # ld a,#nn = 0xCA nn ; ld b,#nn = 0xCB nn
        self.assertEqual(data, b'\xCA\x05\xCB\x0A')

    def test_macro_invoked_twice_expands_each_time(self):
        source = (
            "clear_a\t.macro\n"
            "\tclr\ta\n"
            "\t.endm\n"
            "\t.org 0\n"
            "\tclear_a\n"
            "\tclear_a\n"
        )
        num_lines, data, log, asm = run_assembler(source)
        self.assertEqual(asm.diagnostics, [])
        self.assertEqual(data, b'\x52\x52')

    def test_label_on_invocation_labels_first_expanded_line(self):
        source = (
            "clear_a\t.macro\n"
            "\tclr\ta\n"
            "\t.endm\n"
            "\t.org 0\n"
            "\tbra\tentry\n"
            "entry:\tclear_a\n"
        )
        num_lines, data, log, asm = run_assembler(source)
        self.assertEqual(asm.diagnostics, [])
        # bra (0x40) targeting 'entry' at offset 2 (rel8 = 2 - (0+2) = 0)
        self.assertEqual(data, b'\x40\x00\x52')


class TestLocalLabels(unittest.TestCase):

    def test_local_label_made_unique_across_two_invocations(self):
        # Without .local-renaming, the second expansion's 'loop:' would
        # collide with the first's and fail to assemble.
        source = (
            "countdown\t.macro\n"
            "\t.local loop\n"
            "\tld\ta,#]1\n"
            "loop:\tdec\ta\n"
            "\tbne\tloop\n"
            "\t.endm\n"
            "\t.org 0\n"
            "\tcountdown\t3\n"
            "\tcountdown\t5\n"
        )
        num_lines, data, log, asm = run_assembler(source)
        self.assertEqual(asm.diagnostics, [])
        # Each expansion: ld a,#nn (0xCA nn) ; dec a (0x50) ; bne loop (0x46, rel8=-3)
        expected = (b'\xCA\x03\x50\x46\xFD') + (b'\xCA\x05\x50\x46\xFD')
        self.assertEqual(data, expected)


class TestErrors(unittest.TestCase):

    def test_missing_endm_reports_error(self):
        source = (
            "clear_a\t.macro\n"
            "\tclr\ta\n"
        )
        num_lines, data, log, asm = run_assembler(source)
        self.assertEqual(len(asm.diagnostics), 1)
        self.assertIn("endm", asm.diagnostics[0].message)
        self.assertEqual(data, b'')

    def test_redefining_macro_reports_error(self):
        source = (
            "foo\t.macro\n"
            "\tnop\n"
            "\t.endm\n"
            "foo\t.macro\n"
            "\tnop\n"
            "\t.endm\n"
        )
        num_lines, data, log, asm = run_assembler(source)
        self.assertEqual(len(asm.diagnostics), 1)
        self.assertIn("already defined", asm.diagnostics[0].message)

    def test_too_few_arguments_reports_error(self):
        source = (
            "ld_both\t.macro\n"
            "\tld\ta,#]1\n"
            "\tld\tb,#]2\n"
            "\t.endm\n"
            "\t.org 0\n"
            "\tld_both\t5\n"
        )
        num_lines, data, log, asm = run_assembler(source)
        self.assertEqual(len(asm.diagnostics), 1)
        self.assertIn("too few arguments", asm.diagnostics[0].message)

    def test_non_macro_source_is_unaffected(self):
        # Regression guard: a source with no macros at all must assemble
        # identically to before macro support existed.
        source = (
            "\t.org 0\n"
            "\tbra\tloop\n"
            "loop:\n"
            "\tnop\n"
        )
        num_lines, data, log, asm = run_assembler(source)
        self.assertEqual(asm.diagnostics, [])
        self.assertEqual(num_lines, 4)
        self.assertEqual(data, b'\x40\x00\x00')


if __name__ == '__main__':
    unittest.main()
