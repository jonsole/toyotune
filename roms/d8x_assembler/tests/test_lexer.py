import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))

from lexer import MathEvaluator, MathEvaluatorError, MathTokenizer, Tokenizer


class TestMathEvaluator(unittest.TestCase):

    def setUp(self):
        self.ev = MathEvaluator()

    def test_decimal(self):
        self.assertEqual(self.ev.Evaluate('42'), 42)

    def test_hex_suffix(self):
        self.assertEqual(self.ev.Evaluate('1Fh'), 0x1F)
        self.assertEqual(self.ev.Evaluate('0FFh'), 0xFF)

    def test_hex_prefix(self):
        self.assertEqual(self.ev.Evaluate('$FF'), 0xFF)

    def test_binary(self):
        self.assertEqual(self.ev.Evaluate('101b'), 0b101)

    def test_identifier_lookup(self):
        self.assertEqual(self.ev.Evaluate('foo', variables={'foo': 7}), 7)

    def test_unknown_identifier_raises(self):
        with self.assertRaises(MathEvaluatorError):
            self.ev.Evaluate('bar', variables={})

    def test_operator_precedence(self):
        self.assertEqual(self.ev.Evaluate('2+3*4'), 14)
        self.assertEqual(self.ev.Evaluate('2*3+4'), 10)

    def test_parentheses_override_precedence(self):
        self.assertEqual(self.ev.Evaluate('(2+3)*4'), 20)

    def test_shift_and_bitwise(self):
        self.assertEqual(self.ev.Evaluate('1<<4'), 16)
        self.assertEqual(self.ev.Evaluate('$80>>4'), 8)
        self.assertEqual(self.ev.Evaluate('$F0&$3C'), 0x30)
        self.assertEqual(self.ev.Evaluate('$F0|$0F'), 0xFF)

    def test_division_matches_previous_eval_semantics(self):
        # The original implementation used Python's eval() for '/', which is
        # true division (float), not integer division. Callers (instruction
        # encoders) truncate with int() afterwards. This test locks in that
        # behaviour so the eval()->operator-table change doesn't silently
        # switch to floor division.
        self.assertEqual(self.ev.Evaluate('7/2'), 3.5)
        self.assertEqual(self.ev.Evaluate('8/2'), 4)

    def test_repeated_evaluate_calls_do_not_leak_state(self):
        # MathEvaluator now reuses a single Tokenizer instance across calls
        # for performance; make sure input/position state is fully reset
        # each time rather than carrying over from the previous expression.
        self.assertEqual(self.ev.Evaluate('1+1'), 2)
        self.assertEqual(self.ev.Evaluate('$10'), 0x10)
        self.assertEqual(self.ev.Evaluate('(3+4)*2'), 14)

    def test_two_evaluators_do_not_share_variables(self):
        ev2 = MathEvaluator()
        self.ev.Evaluate('x', variables={'x': 1})
        with self.assertRaises(MathEvaluatorError):
            ev2.Evaluate('x', variables={})


class TestChainedSamePrecedenceOperators(unittest.TestCase):
    # Regression tests: the original algorithm only popped the operator
    # stack for a strictly-higher-precedence operator, so chains of equal
    # precedence operators (e.g. '-' and '+') were applied right-to-left by
    # the final drain loop instead of left-to-right, silently producing the
    # wrong answer for any expression combining more than one +/- (or
    # more than one */ etc).

    def setUp(self):
        self.ev = MathEvaluator()

    def test_subtract_then_add(self):
        self.assertEqual(self.ev.Evaluate('5-2+3'), 6)

    def test_chained_subtraction(self):
        self.assertEqual(self.ev.Evaluate('10-3-2'), 5)

    def test_chained_division(self):
        self.assertEqual(self.ev.Evaluate('100/5/2'), 10)


class TestUnaryMinus(unittest.TestCase):

    def setUp(self):
        self.ev = MathEvaluator()

    def test_leading_unary_minus(self):
        self.assertEqual(self.ev.Evaluate('-5'), -5)

    def test_unary_minus_before_addition(self):
        self.assertEqual(self.ev.Evaluate('-5+3'), -2)

    def test_binary_minus_followed_by_unary_minus(self):
        self.assertEqual(self.ev.Evaluate('2- -3'), 5)

    def test_double_unary_minus(self):
        self.assertEqual(self.ev.Evaluate('--5'), 5)

    def test_unary_minus_binds_tighter_than_multiplication(self):
        self.assertEqual(self.ev.Evaluate('-2*3'), -6)

    def test_unary_minus_with_parentheses(self):
        self.assertEqual(self.ev.Evaluate('-(2+3)*4'), -20)

    def test_leading_unary_plus_is_noop(self):
        self.assertEqual(self.ev.Evaluate('+5'), 5)


class TestMalformedExpressions(unittest.TestCase):
    # Regression tests: these used to raise raw IndexError/KeyError instead
    # of MathEvaluatorError. Since asm_d8x.py only catches MathEvaluatorError
    # (via AssemblyError) around expression evaluation, an uncaught
    # IndexError/KeyError would fall through to the top-level
    # "except Exception: break" and abort assembling the rest of the file.

    def setUp(self):
        self.ev = MathEvaluator()

    def test_unbalanced_open_paren(self):
        with self.assertRaises(MathEvaluatorError):
            self.ev.Evaluate('(1+2')

    def test_unbalanced_close_paren(self):
        with self.assertRaises(MathEvaluatorError):
            self.ev.Evaluate('1+2)')

    def test_empty_expression(self):
        with self.assertRaises(MathEvaluatorError):
            self.ev.Evaluate('')

    def test_two_operands_no_operator(self):
        with self.assertRaises(MathEvaluatorError):
            self.ev.Evaluate('1 2')

    def test_invalid_leading_operator(self):
        with self.assertRaises(MathEvaluatorError):
            self.ev.Evaluate('*5')

    def test_trailing_operator(self):
        with self.assertRaises(MathEvaluatorError):
            self.ev.Evaluate('1++')


class TestTokenizerRuleCaching(unittest.TestCase):

    def test_multiple_instances_share_cached_regex_and_rules(self):
        t1 = MathTokenizer()
        t2 = MathTokenizer()
        self.assertIs(t1.regex, t2.regex)
        self.assertIs(t1.token_rules, t2.token_rules)

    def test_token_rules_only_contains_this_class_rules(self):
        rules = MathTokenizer().token_rules
        self.assertIn('Decimal', rules)
        self.assertIn('Identifier', rules)
        self.assertNotIn('_rules_cache', rules)


if __name__ == '__main__':
    unittest.main()
