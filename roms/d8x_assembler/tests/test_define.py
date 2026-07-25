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


class TestObjectLikeDefine(unittest.TestCase):

    def test_simple_constant_substituted_in_instruction(self):
        source = (
            "\t.define FIVE 5\n"
            "\t.org 0\n"
            "\tld\ta,#FIVE\n"
        )
        num_lines, data, log, asm = run_assembler(source)
        self.assertEqual(asm.diagnostics, [])
        self.assertEqual(data, b'\xCA\x05')

    def test_expression_fragment_substituted_inside_larger_expression(self):
        # A .define can't be expressed with .equ, since it isn't a whole
        # value on its own - it's spliced into a larger expression at the
        # use site.
        source = (
            "\t.define BIT0 1\n"
            "\t.define BIT1 2\n"
            "\t.org 0\n"
            "\tld\ta,#(BIT0|BIT1)\n"
        )
        num_lines, data, log, asm = run_assembler(source)
        self.assertEqual(asm.diagnostics, [])
        self.assertEqual(data, b'\xCA\x03')

    def test_define_referencing_another_define(self):
        source = (
            "\t.define BASE 5\n"
            "\t.define DOUBLE_BASE (BASE*2)\n"
            "\t.org 0\n"
            "\tld\ta,#DOUBLE_BASE\n"
        )
        num_lines, data, log, asm = run_assembler(source)
        self.assertEqual(asm.diagnostics, [])
        self.assertEqual(data, b'\xCA\x0A')

    def test_empty_body_expands_to_nothing(self):
        source = (
            "\t.define FLAG\n"
            "\t.org 0\n"
            "\t.db FLAG 5\n"
        )
        num_lines, data, log, asm = run_assembler(source)
        self.assertEqual(asm.diagnostics, [])
        self.assertEqual(data, b'\x05')

    def test_define_line_itself_produces_no_bytes(self):
        source = (
            "\t.org 0\n"
            "\t.define FIVE 5\n"
            "\tnop\n"
        )
        num_lines, data, log, asm = run_assembler(source)
        self.assertEqual(asm.diagnostics, [])
        self.assertEqual(data, b'\x00')


class TestFunctionLikeDefine(unittest.TestCase):

    def test_single_argument_substitution(self):
        source = (
            "\t.define DOUBLE(x) (x*2)\n"
            "\t.org 0\n"
            "\tld\ta,#DOUBLE(3)\n"
        )
        num_lines, data, log, asm = run_assembler(source)
        self.assertEqual(asm.diagnostics, [])
        self.assertEqual(data, b'\xCA\x06')

    def test_two_argument_substitution(self):
        source = (
            "\t.define ADD(x,y) (x+y)\n"
            "\t.org 0\n"
            "\tld\ta,#ADD(2,3)\n"
        )
        num_lines, data, log, asm = run_assembler(source)
        self.assertEqual(asm.diagnostics, [])
        self.assertEqual(data, b'\xCA\x05')

    def test_nested_call_as_argument(self):
        source = (
            "\t.define DOUBLE(x) (x*2)\n"
            "\t.define ADD(x,y) (x+y)\n"
            "\t.org 0\n"
            "\tld\ta,#ADD(DOUBLE(2),1)\n"
        )
        num_lines, data, log, asm = run_assembler(source)
        self.assertEqual(asm.diagnostics, [])
        self.assertEqual(data, b'\xCA\x05')

    def test_used_without_parens_left_literal(self):
        # Matches C: a function-like macro name with no '(...)' following
        # it is not an invocation, so it's left untouched - which then
        # fails as an unknown identifier, same as it would in C if nothing
        # else defines that bare name.
        source = (
            "\t.define DOUBLE(x) (x*2)\n"
            "\t.org 0\n"
            "\tld\ta,#DOUBLE\n"
        )
        num_lines, data, log, asm = run_assembler(source)
        self.assertEqual(len(asm.diagnostics), 1)
        self.assertIn("DOUBLE", asm.diagnostics[0].message)

    def test_wrong_argument_count_reports_error(self):
        source = (
            "\t.define ADD(x,y) (x+y)\n"
            "\t.org 0\n"
            "\tld\ta,#ADD(1)\n"
        )
        num_lines, data, log, asm = run_assembler(source)
        self.assertEqual(len(asm.diagnostics), 1)
        self.assertIn("expected 2", asm.diagnostics[0].message)


class TestInteractionWithMacros(unittest.TestCase):

    def test_define_usable_inside_macro_body(self):
        source = (
            "\t.define DELAY 10\n"
            "wait_a_bit\t.macro\n"
            "\tld\ta,#DELAY\n"
            "\t.endm\n"
            "\t.org 0\n"
            "\twait_a_bit\n"
        )
        num_lines, data, log, asm = run_assembler(source)
        self.assertEqual(asm.diagnostics, [])
        self.assertEqual(data, b'\xCA\x0A')

    def test_define_usable_as_macro_argument(self):
        source = (
            "\t.define DELAY 10\n"
            "load_a\t.macro\n"
            "\tld\ta,#]1\n"
            "\t.endm\n"
            "\t.org 0\n"
            "\tload_a\tDELAY\n"
        )
        num_lines, data, log, asm = run_assembler(source)
        self.assertEqual(asm.diagnostics, [])
        self.assertEqual(data, b'\xCA\x0A')


class TestErrors(unittest.TestCase):

    def test_redefinition_reports_error(self):
        source = (
            "\t.define FOO 1\n"
            "\t.define FOO 2\n"
        )
        num_lines, data, log, asm = run_assembler(source)
        self.assertEqual(len(asm.diagnostics), 1)
        self.assertIn("already defined", asm.diagnostics[0].message)

    def test_define_without_name_reports_error(self):
        source = "\t.define\n"
        num_lines, data, log, asm = run_assembler(source)
        self.assertEqual(len(asm.diagnostics), 1)
        self.assertIn("requires a name", asm.diagnostics[0].message)

    def test_self_referential_define_does_not_infinite_loop(self):
        # 'FOO' inside its own body is left unexpanded (matches C's rule
        # for a macro name reappearing during its own expansion), so this
        # must terminate rather than recurse forever, even though it's
        # nonsensical as an expression once assembled.
        source = (
            "\t.define FOO (FOO+1)\n"
            "\t.org 0\n"
            "\t.db FOO\n"
        )
        num_lines, data, log, asm = run_assembler(source)
        # 'FOO' inside the body doesn't re-expand, so it becomes '(FOO+1)'
        # literally - which then fails as an unknown identifier. The
        # important thing is that this reports a normal error instead of
        # hanging or crashing.
        self.assertEqual(len(asm.diagnostics), 1)

    def test_non_define_source_is_unaffected(self):
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
