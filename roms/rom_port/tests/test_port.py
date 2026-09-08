"""Regression tests for the ROM name-porting tools.

Every test below corresponds to a mistake that actually reached a source file
during the 9651 -> 0461 and 9661 -> 0471 ports. Run from `roms/rom_port/`:

    python -m unittest discover -s tests -t .
"""

import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from d8x_source import (ram_symbols, functions, instruction_stream,   # noqa: E402
                        normalise_operand, numeric, plausible_address, AUTO)
from matchers import (exact_function_pairs, lockstep_votes,            # noqa: E402
                      window_votes, clear_winners, dma_offset)
from planner import adapt_name, plan_renames, apply_renames            # noqa: E402


def write(text, encoding='utf-8', suffix='.asm'):
    handle = tempfile.NamedTemporaryFile('wb', suffix=suffix, delete=False)
    handle.write(text.encode(encoding))
    handle.close()
    return handle.name


DATA_SEGMENT = """\
\t\t\t\t.org 40h
var_flags_40:\t\t\t.block 1
nv_block:\t\t\t.block 0Ch
unk_4D:\t\t\t\t.block 1
\t\t\t\t.block 2
unk_50:\t\t\t\t.block 1
\t\t\t\t.org 0C000h
"""


class TestSourceParsing(unittest.TestCase):
    def test_block_sizes_are_hex_when_suffixed(self):
        # `.block 0Ch` is 12 bytes, not 0. Reading it as decimal shifted every
        # later symbol by 12 and made the whole RAM map wrong.
        self.assertEqual(numeric('0Ch'), 12)
        self.assertEqual(numeric('12'), 12)

    def test_ram_addresses_match_the_auto_names(self):
        # IDA auto-names encode their own address, so the parser can be
        # checked against the file it just parsed.
        path = write(DATA_SEGMENT)
        try:
            symbols = ram_symbols(path)
            self.assertEqual(symbols['var_flags_40'][0], 0x40)
            self.assertEqual(symbols['unk_4D'][0], 0x4D)
            self.assertEqual(symbols['unk_50'][0], 0x50)
        finally:
            os.unlink(path)

    def test_cp437_sources_read_without_replacement_characters(self):
        # Every non-Claude .ASM in the repo is CP437; decoding as UTF-8 would
        # silently corrupt the banner characters.
        text = DATA_SEGMENT + '; \xc4\xc4\xc4 banner \xc4\xc4\xc4\n'
        path = write(text, encoding='cp437')
        try:
            symbols = ram_symbols(path)
            self.assertEqual(symbols['unk_4D'][0], 0x4D)
        finally:
            os.unlink(path)

    def test_expression_operands_decompose_into_symbols(self):
        # IDA writes some operands as `#(label-1)`. Treating the whole
        # expression as one symbol produced the label definition
        # `(table_knock_retard_step-1):` and broke assembly.
        _, symbols = normalise_operand('y, #(table_knock_retard_step-1)')
        self.assertEqual(symbols, ['table_knock_retard_step'])


class TestAddressPlausibility(unittest.TestCase):
    def test_rpm_thresholds_are_not_addresses(self):
        # `igf_count_rpm_lt_3000` names an RPM threshold. It was once
        # rewritten to `igf_count_rpm_lt_F6B3`.
        self.assertFalse(plausible_address(0x3000))
        self.assertTrue(plausible_address(0x1AB))     # RAM
        self.assertTrue(plausible_address(0xC185))    # ROM


class TestNameAdaptation(unittest.TestCase):
    def setUp(self):
        self.src_ram = {'var_cnt_CE': (0xCE, 1), 'var_ect_unk_194': (0x194, 1)}
        self.dst_ram = {'unk_C8': (0xC8, 1), 'unk_18C': (0x18C, 1)}

    def test_own_address_beats_an_identity_mapping(self):
        # 9651's var_cnt_CE belongs at 0xC8 in 0461. An unrelated identity
        # entry for 0xCE once left the name untouched, and only a collision
        # with the real var_cnt_CE revealed it.
        address_map = {0xCE: 0xCE, 0x194: 0x18C}
        name, unresolved = adapt_name('var_cnt_CE', 'unk_C8', address_map,
                                      self.src_ram, self.dst_ram)
        self.assertEqual(name, 'var_cnt_C8')
        self.assertEqual(unresolved, [])

    def test_cpu2_half_of_a_dma_name_is_refused(self):
        # dmarx_max_retard_23B_161 carries a CPU1 and a CPU2 address. The CPU2
        # half must not be rewritten with a CPU1 map -- the two ECU pairs use
        # different DMA offsets (0xDA vs 0xD1).
        src_ram = {'dmarx_max_retard_23B_161': (0x23B, 1)}
        dst_ram = {'unk_235': (0x235, 1)}
        address_map = {0x23B: 0x235, 0x161: 0x159}
        name, unresolved = adapt_name('dmarx_max_retard_23B_161', 'unk_235',
                                      address_map, src_ram, dst_ram)
        self.assertTrue(unresolved, 'the CPU2 fragment should be refused')
        self.assertIn('161', unresolved[0])

    def test_referenced_variable_is_adapted_when_vouched_for(self):
        # inc_cnt_187 names the counter it increments, not itself. With the
        # caller vouching for 0x187 it should follow the address map.
        src_ram = {'inc_cnt_187': (None, 0), 'var_cnt_187': (0x187, 1)}
        src_ram['inc_cnt_187'] = (None, 0)
        name, unresolved = adapt_name(
            'inc_cnt_187', 'sub_DDF3', {0x187: 0x17F}, {'var_cnt_187': (0x187, 1)},
            {}, justified_addresses={0x187})
        self.assertEqual(name, 'inc_cnt_17F')
        self.assertEqual(unresolved, [])

    def test_non_address_suffix_is_left_alone(self):
        # The 32 in `divide_rD_32` is a scale factor. No symbol lives at 0x32
        # in either ROM, so it is not in the address map and is left alone.
        name, unresolved = adapt_name('divide_rD_32', 'sub_C100', {0x194: 0x18C},
                                      {}, {})
        self.assertEqual(name, 'divide_rD_32')
        self.assertEqual(unresolved, [])

    def test_ambiguous_fragment_is_refused_not_mangled(self):
        # If a scale factor did collide with a mapped address we cannot tell
        # them apart -- the tool must refuse the name, never rewrite it. A
        # refused name costs a manual decision; a rewritten one is a lie.
        name, unresolved = adapt_name('divide_rD_32', 'sub_C100', {0x32: 0x99},
                                      {}, {})
        self.assertEqual(name, 'divide_rD_32')
        self.assertTrue(unresolved)


SRC_ROM = DATA_SEGMENT + """\
helper_routine:
\t\t\t\tld\ta, var_flags_40
\t\t\t\tcmp\ta, #08h
\t\t\t\tst\ta, unk_4D
\t\t\t\tret
; End of function helper_routine
"""

DST_ROM = DATA_SEGMENT + """\
sub_C200:
\t\t\t\tld\ta, var_flags_40
\t\t\t\tcmp\ta, #08h
\t\t\t\tst\ta, unk_4D
\t\t\t\tret
; End of function sub_C200
"""


class TestMatchingAndPlanning(unittest.TestCase):
    def setUp(self):
        self.src = write(SRC_ROM)
        self.dst = write(DST_ROM)

    def tearDown(self):
        os.unlink(self.src)
        os.unlink(self.dst)

    def test_identical_functions_pair_up(self):
        pairs = exact_function_pairs(self.src, self.dst)
        self.assertEqual(len(pairs), 1)
        self.assertEqual(pairs[0][0]['name'], 'helper_routine')
        self.assertEqual(pairs[0][1]['name'], 'sub_C200')

    def test_plan_renames_the_unnamed_function(self):
        votes, referenced = lockstep_votes(exact_function_pairs(self.src, self.dst))
        new, updated, unchanged, rejected = plan_renames(
            votes, self.src, self.dst, referenced=referenced, trust_referenced=True)
        self.assertIn(['sub_C200', 'helper_routine', 1, 'verbatim'], new)
        # symbols already named identically must not be touched
        self.assertIn('var_flags_40', unchanged)

    def test_existing_target_name_is_refused(self):
        # A fuzzy match once renamed a function to `adc_handler_pim`, a name
        # the target file already used, producing a duplicate label and an
        # assembly that would not converge. Here the target already defines
        # `helper_routine` elsewhere, so the rename must be refused.
        collide = DST_ROM.replace('unk_50:\t\t\t\t.block 1',
                                  'unk_50:\t\t\t\t.block 1\nhelper_routine:\t\t\t.block 1')
        path = write(collide)
        try:
            votes, referenced = lockstep_votes(exact_function_pairs(self.src, path))
            _, _, _, rejected = plan_renames(votes, self.src, path,
                                             referenced=referenced)
            self.assertTrue(any('already used' in str(r[3]) for r in rejected))
        finally:
            os.unlink(path)


class TestApply(unittest.TestCase):
    def test_swaps_are_applied_simultaneously(self):
        # A batch can legitimately contain A->B and B->C. Sequential
        # application would merge the two symbols.
        path = write('alpha:\n\t\t\t\tld a, beta\n')
        try:
            apply_renames(path, {'alpha': 'beta', 'beta': 'gamma'})
            text = open(path, encoding='utf-8').read()
            self.assertIn('beta:', text)
            self.assertIn('gamma', text)
            self.assertNotIn('alpha', text)
        finally:
            os.unlink(path)

    def test_invalid_identifier_is_refused(self):
        path = write('alpha:\n')
        try:
            with self.assertRaises(ValueError):
                apply_renames(path, {'alpha': '(label-1)'})
        finally:
            os.unlink(path)

    def test_two_symbols_to_one_name_is_refused(self):
        path = write('alpha:\nbeta:\n')
        try:
            with self.assertRaises(ValueError):
                apply_renames(path, {'alpha': 'same', 'beta': 'same'})
        finally:
            os.unlink(path)


class TestDmaOffset(unittest.TestCase):
    def test_modal_offset_wins_over_a_stray_pair(self):
        # A 16-bit variable whose name sits on the other byte disagrees by
        # one; the mode is the answer, not the first hit.
        cpu1 = {'dmarx_ign_timing': (0x23D, 1), 'dmarx_status1': (0x240, 1),
                'dmarx_rpm': (0x22E, 2)}
        cpu2 = {'dmatx_ign_timing': (0x163, 1), 'dmatx_status1': (0x166, 1),
                'dmatx_rpm': (0x155, 2)}
        offset, votes = dma_offset(cpu1, cpu2, 'cpu2_to_cpu1')
        self.assertEqual(offset, 0xDA)
        self.assertEqual(votes[0xDA], 2)
        self.assertEqual(votes[0xD9], 1)


class TestWindowMatching(unittest.TestCase):
    def test_window_reaches_code_outside_any_function(self):
        # The window matcher exists because `unk_23C` -> `dmarx_iscv_duty` sits
        # in a chunk IDA folded into another routine, so no function pair
        # covers it.
        body = ("\t\t\t\tld\ta, {sym}\n\t\t\t\tcmp\ta, #08h\n"
                "\t\t\t\tclr\tb\n\t\t\t\tinc\tx\n\t\t\t\tdec\ty\n\t\t\t\tret\n")
        src = write(DATA_SEGMENT + body.format(sym='dmarx_iscv_duty'))
        dst = write(DATA_SEGMENT + body.format(sym='unk_23C'))
        try:
            votes, matched = window_votes(src, dst, sizes=(6,))
            self.assertGreater(matched, 0)
            winners = clear_winners(votes)
            self.assertEqual(winners['unk_23C'][0], 'dmarx_iscv_duty')
        finally:
            os.unlink(src)
            os.unlink(dst)


if __name__ == '__main__':
    unittest.main()
