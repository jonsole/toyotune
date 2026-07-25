import re


class DefineError(Exception):
    pass


class Definition(object):
    def __init__(self, name, params, body, line_num):
        self.name = name
        self.params = params    # None if object-like; list of str if function-like
        self.body = body        # raw, unexpanded replacement text
        self.line_num = line_num


# 'NAME' optionally immediately followed by '(params)' with no whitespace in
# between - the same convention C uses to tell a function-like declaration
# apart from an object-like one whose replacement text happens to start with
# a parenthesized expression (e.g. '.define MASK (BIT0|BIT1)' is object-
# like: the space before '(' matters) - then whitespace, then the
# replacement text (may be empty; a name with no replacement is legal and
# expands to nothing, like C's '#define DEBUG').
_declaration_re = re.compile(
    r'^(?P<name>[.a-zA-Z_][.a-zA-Z0-9_]*)(\((?P<params>[^)]*)\))?\s*(?P<body>.*)$')

_identifier_re = re.compile(r'[.a-zA-Z_][.a-zA-Z0-9_]*')

# Recursion guard for chained/self-referential defines - defines are
# expanded via a recursive per-identifier scan (see ExpandText) rather than
# the repeat-until-stable loop macro expansion uses, so in the well-behaved
# case this never gets close to triggering; it only exists to turn a
# pathological definition chain into a clean error instead of a Python
# RecursionError.
MAX_DEFINE_DEPTH = 50


def _ExtractBalancedParens(text, open_pos):
    """text[open_pos] is '('. Returns (inner_text, index_after_close), or
    (None, None) if there is no matching close paren."""
    depth = 0
    for i in range(open_pos, len(text)):
        c = text[i]
        if c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0:
                return text[open_pos + 1:i], i + 1
    return None, None


def _SplitTopLevelArgs(text):
    """Comma-split 'text', ignoring commas nested inside parentheses (so an
    argument that is itself a call, e.g. 'MUL(2,3)', isn't split apart)."""
    if not text.strip():
        return []
    args = []
    depth = 0
    current = []
    for c in text:
        if c == ',' and depth == 0:
            args.append(''.join(current))
            current = []
            continue
        if c == '(':
            depth += 1
        elif c == ')':
            depth -= 1
        current.append(c)
    args.append(''.join(current))
    return args


class DefineProcessor(object):
    """Implements '.define' - C-preprocessor-style object-like and
    function-like text macros. Unlike '.macro' (whole-line expansion with
    ']1'..']9' positional parameters), '.define' substitutes at the token
    level, so it can stand in for part of an expression (e.g. a named bit
    mask used inside a larger '.db'/operand expression) rather than only a
    whole instruction.

    Note this is a simplified model of C's preprocessor: a definition's
    body is expanded recursively as it's spliced in (with a currently-
    expanding set to stop self-reference, matching C's rule that a macro
    name reappearing during its own expansion is left unexpanded), but the
    result is not re-scanned as a single combined string the way a real C
    preprocessor rescans after substitution - two macro invocations that
    only form a valid name once concatenated across a substitution boundary
    won't be recognised. That case doesn't arise in normal use.
    """

    def __init__(self):
        self._defines = {}   # name -> Definition

    def IsDefine(self, name):
        return name in self._defines

    def Define(self, declaration, line_num):
        if not declaration:
            raise DefineError("'.define' requires a name")
        m = _declaration_re.match(declaration)
        if not m:
            raise DefineError("'.define' has an invalid declaration '{}'".format(declaration))
        name = m.group('name')
        if name in self._defines:
            raise DefineError("'{}' already defined".format(name))
        params = None
        if m.group('params') is not None:
            params_text = m.group('params').strip()
            params = [p.strip() for p in m.group('params').split(',')] if params_text else []
        self._defines[name] = Definition(name, params, m.group('body'), line_num)

    def ExpandText(self, text, _active=frozenset(), _depth=0):
        if not text:
            return text
        if _depth > MAX_DEFINE_DEPTH:
            raise DefineError('.define expansion nested too deep (recursive definition?)')

        out = []
        pos = 0
        for m in _identifier_re.finditer(text):
            # finditer precomputed every match over the original text up
            # front; a function-like call consumes its whole '(...)' span
            # (which can contain further identifier matches, e.g. a nested
            # call used as an argument), so matches that fall inside a span
            # already consumed by an earlier substitution must be skipped
            # rather than reprocessed.
            if m.start() < pos:
                continue
            out.append(text[pos:m.start()])
            name = m.group(0)
            pos = m.end()

            definition = self._defines.get(name)
            if definition is None or name in _active:
                out.append(name)
                continue

            if definition.params is None:
                out.append(self.ExpandText(definition.body, _active | {name}, _depth + 1))
                continue

            # Function-like: only expands when actually called with '(...)'
            # (optional whitespace before the paren is allowed at the call
            # site, unlike at the declaration site) - used bare, the name
            # is left as a literal identifier, matching C.
            call_pos = pos
            while call_pos < len(text) and text[call_pos] in ' \t':
                call_pos += 1
            if call_pos >= len(text) or text[call_pos] != '(':
                out.append(name)
                continue

            args_text, end_pos = _ExtractBalancedParens(text, call_pos)
            if args_text is None:
                raise DefineError("'{}(' is missing a closing ')'".format(name))
            args = _SplitTopLevelArgs(args_text)
            if len(args) != len(definition.params):
                raise DefineError(
                    "'{}' invoked with {} argument(s), expected {}".format(
                        name, len(args), len(definition.params)))

            body = definition.body
            for pname, avalue in zip(definition.params, args):
                body = re.sub(r'\b{}\b'.format(re.escape(pname)), avalue.strip(), body)

            out.append(self.ExpandText(body, _active | {name}, _depth + 1))
            pos = end_pos

        out.append(text[pos:])
        return ''.join(out)
