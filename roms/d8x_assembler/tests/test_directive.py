import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))

import directive
from asm_d8x import AssemblerPass


class TestOrgAndBlock(unittest.TestCase):

    def test_org_sets_pc(self):
        ctx = AssemblerPass()
        directive.Assemble(ctx, 'org', '100h')
        self.assertEqual(ctx.Pc, 0x100)

    def test_block_advances_pc_without_emitting_bytes(self):
        ctx = AssemblerPass()
        directive.Assemble(ctx, 'org', '10h')
        directive.Assemble(ctx, 'block', '4')
        self.assertEqual(ctx.Pc, 0x14)
        self.assertEqual(bytes(ctx._bytes), b'')


class TestDbDw(unittest.TestCase):

    def test_db_emits_bytes(self):
        ctx = AssemblerPass()
        directive.Assemble(ctx, 'db', '1,2,$FF')
        self.assertEqual(bytes(ctx._bytes), b'\x01\x02\xFF')
        self.assertEqual(ctx.Pc, 3)

    def test_dw_emits_big_endian_words(self):
        ctx = AssemblerPass()
        directive.Assemble(ctx, 'dw', '1,$1234')
        self.assertEqual(bytes(ctx._bytes), b'\x00\x01\x12\x34')
        self.assertEqual(ctx.Pc, 4)

    def test_db_out_of_range_raises_directive_error(self):
        # Regression test: HandleDb used to raise a bare Exception for an
        # out-of-range value instead of DirectiveError. Since asm_d8x.py
        # only catches DirectiveError around directive handling, a bare
        # Exception fell through to the top-level "except Exception: break"
        # in Assembler.Assemble() and silently aborted assembly of the rest
        # of the file. It must raise DirectiveError so the caller reports a
        # normal per-line error and keeps assembling.
        ctx = AssemblerPass()
        with self.assertRaises(directive.DirectiveError):
            directive.Assemble(ctx, 'db', '999')

    def test_dw_out_of_range_raises_directive_error(self):
        ctx = AssemblerPass()
        with self.assertRaises(directive.DirectiveError):
            directive.Assemble(ctx, 'dw', '70000')


class TestEnd(unittest.TestCase):

    def test_end_sets_is_end(self):
        ctx = AssemblerPass()
        self.assertFalse(ctx.IsEnd)
        directive.Assemble(ctx, 'end', None)
        self.assertTrue(ctx.IsEnd)


class TestUnknownDirective(unittest.TestCase):

    def test_unknown_directive_raises(self):
        ctx = AssemblerPass()
        with self.assertRaises(directive.DirectiveError):
            directive.Assemble(ctx, 'bogus', None)


class TestEqu(unittest.TestCase):

    def test_equ_binds_label_to_expression_value_not_pc(self):
        ctx = AssemblerPass()
        directive.Assemble(ctx, 'org', '100h')
        directive.Assemble(ctx, 'equ', '1CFh', label='var_flags_4E_alias')
        self.assertEqual(ctx._labels['var_flags_4E_alias'], 0x1CF)
        # PC is untouched - 'equ' doesn't consume address space.
        self.assertEqual(ctx.Pc, 0x100)

    def test_equ_without_label_raises(self):
        ctx = AssemblerPass()
        with self.assertRaises(directive.DirectiveError):
            directive.Assemble(ctx, 'equ', '1CFh')

    def test_equ_without_argument_raises(self):
        ctx = AssemblerPass()
        with self.assertRaises(directive.DirectiveError):
            directive.Assemble(ctx, 'equ', None, label='foo')


if __name__ == '__main__':
    unittest.main()
