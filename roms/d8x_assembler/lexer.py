import operator
import re

class Token(object):
    """ A simple Token structure.
        Contains the token rule that token was created from and
        the token value.
    """
    def __init__(self, rule, value):
        self._rule = rule
        self._value = value

    @property
    def value(self):
        return self._value

    @property
    def rule(self):
        return self._rule


class TokenizerError(Exception):
    """ Lexer error exception.
        pos:
            Position in the input line where the error occurred.
    """
    def __init__(self, pos):
        self.pos = pos


class Tokenizer(object):
    """ A simple regex-based lexer/tokenizer.
        See below for an example of usage.
    """

    # Per-subclass cache of (token_rules dict, compiled regex). The rule set
    # is fixed by the class definition, so it only needs to be derived once
    # per subclass rather than on every instantiation/token.
    _rules_cache = {}

    @classmethod
    def _get_rules_and_regex(cls):
        cached = Tokenizer._rules_cache.get(cls)
        if cached is None:
            rules = {k: v for (k, v) in cls.__dict__.items() if isinstance(v, TokenRule)}
            regex_list = ['(?P<{0}>{1})'.format(k, v.regex) for k, v in rules.items()]
            regex = re.compile('|'.join(regex_list))
            cached = (rules, regex)
            Tokenizer._rules_cache[cls] = cached
        return cached

    @property
    def token_rules(self):
        """ Dictionary of token rule objects.
        """
        return self._token_rules

    def __init__(self, ignore=''):

        self._tokens = []
        self._token_pos = 0

        # Characters to ignore before tokens
        self.ignore = ignore

        self._token_rules, self.regex = self._get_rules_and_regex()

    def tokens(self):
        """ Returns an iterator to the tokens found in the buffer.
        """
        while self.input_pos < len(self.input_buf):

            # This code provides some short-circuit code for whitespace, tabs, and other ignored characters
            if self.input_buf[self.input_pos] in self.ignore:
                self.input_pos += 1
                continue

            # Attempt to match regular expression to buffer
            m = self.regex.match(self.input_buf, self.input_pos)
            if m:
                # Get token rule from class dictionary using name of the group that matched
                rule = self._token_rules[m.lastgroup]

                # Create token from gnerator and regex match
                token = rule.token(m.group(m.lastgroup))
                self.input_pos = m.end()
                yield token
            else:
                # if we're here, no rule matched
                raise TokenizerError(self.input_pos)

    def input(self, buf):
        """ Initialize the lexer with a buffer as input.
        """
        self.input_buf = buf
        self.input_pos = 0


class TokenRule(object):

    def __init__(self, regex):
        self.regex = regex

    def token(self, value):
        return Token(self, value)


class MathTokenizer(Tokenizer):

    HexSuffix       = TokenRule(r'[0-9][0-9a-fA-F]*h') 
    HexPrefix       = TokenRule(r'\$[0-9a-fA-F]+')
    Binary          = TokenRule(r'[0-1]+b')
    Identifier      = TokenRule(r'[.a-zA-Z_][.a-zA-Z0-9_]*')
    Decimal         = TokenRule(r'\d+')
    Bracket         = TokenRule(r'[()]')
    Operator        = TokenRule(r'([\+/\*\-|&])|(<<)|(>>)')
        

class MathEvaluatorError(Exception):
    pass

# Maps operator token text to the equivalent Python operator function.
# '/' maps to true division (not floordiv) to match the previous eval()-based
# behaviour, which relied on Python's '/' operator semantics.
# 'u-' is the internal token used for unary minus (see Evaluate()); it is
# rewritten as a binary '0 - x', so it maps to the same function as '-'.
_BINARY_OPS = {
    '+'  : operator.add,
    '-'  : operator.sub,
    '*'  : operator.mul,
    '/'  : operator.truediv,
    '<<' : operator.lshift,
    '>>' : operator.rshift,
    '&'  : operator.and_,
    '|'  : operator.or_,
    'u-' : operator.sub,
}

# Operators that associate right-to-left. Only unary minus currently; all
# binary operators here are left-associative.
_RIGHT_ASSOCIATIVE = {'u-'}

class MathEvaluator(object):

    # List of tokens that are operands
    operands = [
        MathTokenizer.Decimal,
        MathTokenizer.HexSuffix,
        MathTokenizer.HexPrefix,
        MathTokenizer.Binary,
        MathTokenizer.Identifier
    ]

    def __init__(self):
        # Reused across calls to Evaluate() to avoid rebuilding the tokenizer
        # (and its regex) for every expression.
        self._tokenizer = MathTokenizer(ignore=' \t')

    def Evaluate(self, expression, variables={}):
        self._variables = variables

        def peek(stack):
            return stack[-1] if stack else None

        def apply_operator(operators, values):
            op = operators.pop().value
            right = values.pop()
            left = values.pop()
            values.append(_BINARY_OPS[op](left, right))

        precedences = {
            '+'  : 0,
            '-'  : 0,
            '*'  : 1,
            '/'  : 1,
            '<<' : 2,
            '>>' : 2,
            '&'  : 3,
            '|'  : 4,
            'u-' : 5,
        }

        def should_reduce_before(top_op, new_op):
            # Left-associative operators (everything except unary minus)
            # reduce on >= so that operators of equal precedence are applied
            # left-to-right as they're encountered, e.g. '5-2+3' must reduce
            # the '-' before pushing '+', otherwise the final LIFO drain loop
            # would apply them right-to-left and compute 5-(2+3) instead of
            # (5-2)+3.
            #
            # Unary minus is right-associative, so it must use a strict >:
            # otherwise a preceding operator of equal-or-lower precedence
            # (including another pending unary minus, e.g. '--5') would
            # wrongly bind to the placeholder 0 before the unary operator
            # reaches its real operand.
            if new_op in _RIGHT_ASSOCIATIVE:
                return precedences[top_op] > precedences[new_op]
            return precedences[top_op] >= precedences[new_op]

        values = []
        operators = []
        ml = self._tokenizer
        ml.input(expression)

        # Tracks whether the next token is expected to be an operand (or a
        # leading unary +/-) rather than a binary operator. True at the
        # start of the expression, right after '(', and right after a
        # binary operator.
        expect_operand = True

        try:

            for token in ml.tokens():

                if token.rule in MathEvaluator.operands:

                    if token.rule == MathTokenizer.Decimal:
                        values.append(int(token.value, base=10))
                    elif token.rule == MathTokenizer.HexSuffix:
                        values.append(int(token.value[:-1], base=16))
                    elif token.rule == MathTokenizer.HexPrefix:
                        values.append(int(token.value[1:], base=16))
                    elif token.rule == MathTokenizer.Binary:
                        values.append(int(token.value[:-1], base=2))
                    elif token.rule == MathTokenizer.Identifier:
                        v = self._variables.get(token.value)
                        if v != None:
                            values.append(v)
                        else:
                            raise MathEvaluatorError('Unknown identifier \'{}\''.format(token.value))
                    expect_operand = False

                elif token.value == '(':
                    operators.append(token)
                    expect_operand = True

                elif token.value == ')':

                    top = peek(operators)
                    while top is not None and top.value != '(':
                        apply_operator(operators, values)
                        top = peek(operators)

                    # Discard the '('
                    operators.pop()
                    expect_operand = False

                else:
                    # Operator (binary, or unary +/- if an operand was expected here)
                    if expect_operand:
                        if token.value == '+':
                            # Unary plus is a no-op.
                            continue
                        elif token.value == '-':
                            # Rewrite unary '-' as a binary '0 - x' so the
                            # rest of the algorithm can treat it normally,
                            # using a distinct 'u-' operator (see
                            # should_reduce_before) so it binds tighter than,
                            # and associates correctly with, real operators.
                            values.append(0)
                            token = Token(token.rule, 'u-')
                        else:
                            raise MathEvaluatorError('Unexpected operator \'{}\''.format(token.value))

                    top = peek(operators)
                    while top is not None and top.value not in "()" and should_reduce_before(top.value, token.value):
                        apply_operator(operators, values)
                        top = peek(operators)
                    operators.append(token)
                    expect_operand = True

            while operators:
                apply_operator(operators, values)

            if len(values) != 1:
                raise MathEvaluatorError('Malformed expression \'{}\''.format(expression))

            return values[0]

        except TokenizerError:
            raise MathEvaluatorError('Unknown token')
        except (IndexError, KeyError):
            # Stack underflow (e.g. mismatched parentheses, trailing/missing
            # operand) or an unrecognised operator - both indicate a
            # malformed expression rather than an internal bug, so surface
            # them the same way as other evaluator errors instead of
            # letting a raw IndexError/KeyError escape and be treated as an
            # "Internal error" by the caller (which aborts assembling the
            # rest of the file).
            raise MathEvaluatorError('Malformed expression \'{}\''.format(expression))



