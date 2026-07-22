import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))

import instruction
from asm_d8x import AssemblerPass


def assemble(opcode, operands, pc=0):
    """Assemble a single instruction against a fresh AssemblerPass and
    return the emitted bytes."""
    ctx = AssemblerPass()
    ctx.SetPc(pc)
    instruction.Assemble(ctx, opcode, operands)
    return bytes(ctx._bytes)


class TestNoOperandInstructions(unittest.TestCase):

    def test_nop(self):
        self.assertEqual(assemble('nop', None), b'\x00')

    def test_fixed_encoding_case_insensitive_opcode(self):
        self.assertEqual(assemble('CLRC', None), b'\x65')


class TestRegisterToRegister(unittest.TestCase):

    def test_mov_a_b(self):
        self.assertEqual(assemble('mov', 'a,b'), b'\x5B')

    def test_add_x_a(self):
        self.assertEqual(assemble('add', 'x,a'), b'\x0C')


class TestImmediateAddressing(unittest.TestCase):

    def test_8bit_immediate(self):
        self.assertEqual(assemble('ld', 'a,#10'), b'\xCA\x0A')

    def test_16bit_only_immediate(self):
        self.assertEqual(assemble('ld', 'd,#1000'), b'\x86\x03\xE8')

    def test_immediate_out_of_8bit_range_raises(self):
        with self.assertRaises(instruction.InstructionError):
            assemble('ld', 'a,#300')


class TestAbsoluteAddressing(unittest.TestCase):

    def test_prefers_8bit_form_when_value_fits(self):
        self.assertEqual(assemble('ld', 'a,5'), b'\xDA\x05')

    def test_falls_back_to_16bit_form_when_value_too_large(self):
        self.assertEqual(assemble('ld', 'a,300'), b'\xFA\x01\x2C')


class TestIndexedAddressing(unittest.TestCase):

    def test_indexed_x(self):
        self.assertEqual(assemble('ld', 'a,x+10'), b'\xEA\x0A')

    def test_indexed_y_sets_high_bit(self):
        self.assertEqual(assemble('ld', 'a,y+10'), b'\xEA\x8A')

    def test_indexed_offset_out_of_range_raises(self):
        with self.assertRaises(instruction.InstructionError):
            assemble('ld', 'a,x+200')


class TestRelativeBranch(unittest.TestCase):

    def test_forward_branch_offset(self):
        # bra at pc=0 (2 bytes) targeting pc=10 -> offset = 10 - (0+1+1) = 8
        self.assertEqual(assemble('bra', '10', pc=0), b'\x40\x08')

    def test_backward_branch_offset(self):
        # bra at pc=10 targeting pc=0 -> offset = 0 - (10+1+1) = -12
        self.assertEqual(assemble('bra', '0', pc=10), b'\x40\xF4')

    def test_branch_target_out_of_range_raises(self):
        with self.assertRaises(instruction.InstructionError):
            assemble('bra', '1000', pc=0)


class TestBitAddressing(unittest.TestCase):

    def test_setb_low_bank(self):
        # bit 3 of address 0x20 (low bank, address -= 0x20 -> 0)
        self.assertEqual(assemble('setb', '#3,20h'), b'\x77\x60')

    def test_setb_high_bank(self):
        # bit 1 of address 0x40 (high bank, address -= 0x30 -> 0x10)
        self.assertEqual(assemble('setb', '#1,40h'), b'\x77\x30')

    def test_bit_address_out_of_range_raises(self):
        with self.assertRaises(instruction.InstructionError):
            assemble('setb', '#1,10h')

    def test_bit_number_out_of_range_raises(self):
        with self.assertRaises(instruction.InstructionError):
            assemble('setb', '#8,20h')


class TestErrorCases(unittest.TestCase):

    def test_unknown_opcode_raises(self):
        with self.assertRaises(instruction.InstructionError):
            assemble('zzz', None)

    def test_too_many_operands_raises(self):
        with self.assertRaises(instruction.InstructionError):
            assemble('nop', 'a')

    def test_not_enough_operands_raises(self):
        with self.assertRaises(instruction.InstructionError):
            assemble('ld', 'a')


if __name__ == '__main__':
    unittest.main()
