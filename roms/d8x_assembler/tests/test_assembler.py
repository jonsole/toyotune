import contextlib
import io
import os
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))

import asm_d8x
from asm_d8x import Assembler, AssemblerPass


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


class TestTwoPassForwardReferences(unittest.TestCase):

    def test_forward_relative_branch_resolves_correctly(self):
        source = (
            "\t.org 0\n"
            "\tbra\tloop\n"
            "loop:\n"
            "\tnop\n"
        )
        num_lines, data, log, asm = run_assembler(source)
        self.assertEqual(asm.diagnostics, [])
        self.assertEqual(num_lines, 4)
        # bra (0x40) with rel8 offset 0 (branch lands exactly on itself+2),
        # followed by the nop it jumps to.
        self.assertEqual(data, b'\x40\x00\x00')
        # Converges in the common/fast case: pass 1 + pass 2, no retries.
        self.assertEqual(log.count("Assembling Pass"), 2)

    def test_forward_absolute_jmp_resolves_correctly(self):
        source = (
            "\t.org 0\n"
            "\tjmp\ttarget\n"
            "target:\n"
            "\tnop\n"
        )
        num_lines, data, log, asm = run_assembler(source)
        self.assertEqual(asm.diagnostics, [])
        self.assertEqual(data, b'\x03\x00\x03\x00')

    def test_db_dw_with_label_reference_and_org_offset(self):
        source = (
            "\t.org 10h\n"
            "val:\n"
            "\t.db\t1,2,3\n"
            "\t.dw\tval\n"
        )
        num_lines, data, log, asm = run_assembler(source)
        self.assertEqual(asm.diagnostics, [])
        # val = 0x10 (the org address); dw emits it big-endian.
        self.assertEqual(data, b'\x01\x02\x03\x00\x10')


class TestErrorRecoveryContinuesPastBadLine(unittest.TestCase):

    def test_pass1_keeps_scanning_after_a_directive_error(self):
        # Regression test for the HandleDb/HandleDw bug: an out-of-range
        # .db value used to raise a bare Exception, which asm_d8x.py's
        # generic "except Exception: break" caught as an "Internal error"
        # and aborted pass 1 entirely, silently skipping every line after
        # it. It must now be reported as a normal per-line error so
        # subsequent lines (including further errors) still get processed.
        source = (
            "\t.db 999\n"
            "\tzzz\n"
        )
        num_lines, data, log, asm = run_assembler(source)
        self.assertEqual(num_lines, 2)
        self.assertNotIn("Internal error", log)
        self.assertEqual(len(asm.diagnostics), 2)
        self.assertEqual([d.level for d in asm.diagnostics], ['error', 'error'])
        self.assertEqual([d.line for d in asm.diagnostics], [1, 2])
        # errors > 0, so pass 2 (and thus output) is skipped entirely.
        self.assertEqual(data, b'')


class TestLabelConvergenceAcrossPasses(unittest.TestCase):
    # 'ld a,target' is a forward reference: pass 1 has no address for
    # 'target' yet, so it falls back to a placeholder (the current pc,
    # which is small) and picks the 8-bit absolute encoding. Once 'target'
    # is actually known (303, from the 300-byte .block) the operand no
    # longer fits 8 bits, so the encoding grows by a byte, which shifts
    # 'target' itself by one more byte than pass 1 assumed - requiring a
    # 3rd pass to re-resolve against the corrected address.
    SOURCE = (
        "\t.org 0\n"
        "\tld\ta,target\n"
        "\t.block 300\n"
        "target:\n"
        "\tnop\n"
    )

    def test_converges_after_three_passes(self):
        num_lines, data, log, asm = run_assembler(self.SOURCE)
        self.assertEqual(asm.diagnostics, [])
        self.assertEqual(log.count("Assembling Pass"), 3)
        # ld a,nnnn (0xFA) with target = 3 (instruction length) + 300 = 303 (0x012F)
        expected = b'\xFA\x01\x2F' + b'\x00' * 300 + b'\x00'
        self.assertEqual(data, expected)

    def test_reports_a_clean_error_if_it_never_converges(self):
        # Force the convergence loop to give up after fewer attempts than
        # this source actually needs, to exercise the "did not converge"
        # path without needing a source that oscillates forever.
        with unittest.mock.patch.object(asm_d8x, 'MAX_PASSES', 2):
            num_lines, data, log, asm = run_assembler(self.SOURCE)
        self.assertEqual(len(asm.diagnostics), 1)
        self.assertIn("did not converge", asm.diagnostics[0].message)
        self.assertIsNone(asm.diagnostics[0].line)
        self.assertEqual(data, b'')


class TestEquAlias(unittest.TestCase):

    def test_equ_alias_resolves_to_aliased_address_not_its_own_line_pc(self):
        # Regression test for the bug where an unrecognized '.equ' still got
        # its label bound to the current PC (the generic label-handling
        # path), instead of erroring cleanly or resolving to the aliased
        # expression's value. var_flags_4E_alias's own source line sits one
        # byte after var_flags_4E (which consumes a byte via .block), so a
        # PC-based fallback would incorrectly resolve to var_flags_4E + 1
        # instead of var_flags_4E itself.
        source = (
            "\t.org 100h\n"
            "\t.block 5\n"
            "var_flags_4E:\t.block 1\n"
            "var_flags_4E_alias:\t.equ var_flags_4E\n"
            "\tld\ta,var_flags_4E_alias\n"
        )
        num_lines, data, log, asm = run_assembler(source)
        self.assertEqual(asm.diagnostics, [])
        # var_flags_4E is at 0x100 + 5 = 0x105.
        self.assertEqual(data, b'\xFA\x01\x05')


class TestLabelsChangedTracking(unittest.TestCase):

    def test_label_value_changing_between_passes_is_tracked_not_raised(self):
        pass1 = AssemblerPass()
        pass1.SetLabel('foo', 10)
        pass2 = AssemblerPass(pass1)
        pass2.SetLabel('foo', 20)  # no longer raises
        self.assertTrue(pass2.LabelsChanged)

    def test_label_value_unchanged_between_passes(self):
        pass1 = AssemblerPass()
        pass1.SetLabel('foo', 10)
        pass2 = AssemblerPass(pass1)
        pass2.SetLabel('foo', 10)
        self.assertFalse(pass2.LabelsChanged)


class TestExitCode(unittest.TestCase):

    def _run_cli(self, source):
        script = os.path.join(os.path.dirname(__file__), '..', 'asm_d8x.py')
        src_path = os.path.join(os.path.dirname(__file__), '_cli_input.asm')
        out_path = os.path.join(os.path.dirname(__file__), '_cli_output.bin')
        with open(src_path, 'w') as f:
            f.write(source)
        try:
            import subprocess
            result = subprocess.run(
                [sys.executable, script, src_path, out_path],
                capture_output=True, text=True)
            return result.returncode
        finally:
            for p in (src_path, out_path):
                if os.path.exists(p):
                    os.remove(p)

    def test_exit_code_zero_on_success(self):
        self.assertEqual(self._run_cli("\t.org 0\n\tnop\n"), 0)

    def test_exit_code_nonzero_on_error(self):
        self.assertEqual(self._run_cli("\tzzz\n"), 1)


if __name__ == '__main__':
    unittest.main()
