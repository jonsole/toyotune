import collections
import re


class MacroError(Exception):
    pass


# A single already-classified source line: the (label, directive,
# instruction, comment) groups produced by asm_d8x.py's line_re, plus the
# original 1-based source line number (kept so diagnostics/listings can
# still point at the real file line once macro expansion has changed the
# line count).
ParsedLine = collections.namedtuple(
    'ParsedLine', ['line_num', 'label', 'directive', 'instruction', 'comment'])

# Upper bound on how many rounds of expansion an invocation chain (a macro
# invoking another macro) can go through before giving up - mirrors
# asm_d8x.MAX_PASSES, and exists only to guard against a macro that
# (directly or indirectly) invokes itself.
MAX_EXPANSION_DEPTH = 10

# TASM-style positional macro parameters are written ']1' .. ']9' rather
# than '#1' .. '#9'. This ISA's immediate-addressing operands are already
# written '#nn' (e.g. 'ld a,#5'), so a '#'-based placeholder would be
# ambiguous with a literal immediate value used inside a macro body.
_param_re = re.compile(r'\](?P<index>[1-9])')


class MacroDefinition(object):
    def __init__(self, name, line_num):
        self.name = name
        self.line_num = line_num
        self.locals = []    # symbol names declared via LOCAL, made unique per expansion
        self.body = []      # list of ParsedLine, in definition order


class MacroProcessor(object):
    """Extracts MACRO/ENDM/LOCAL definitions from a source file and expands
    invocations of them, as a text/structure substitution pass that runs
    entirely ahead of the real two-pass assembler - macros are never seen
    by AssemblerPass itself.
    """

    def __init__(self):
        self._macros = {}       # name.lower() -> MacroDefinition
        self._current = None    # MacroDefinition being accumulated, or None
        self._expansion_count = 0

    @property
    def InDefinition(self):
        return self._current is not None

    @property
    def CurrentDefinitionLine(self):
        return self._current.line_num if self._current else None

    def IsMacro(self, name):
        return name is not None and name.lower() in self._macros

    def BeginDefinition(self, name, line_num):
        if not name:
            raise MacroError("'.macro' requires a name (label the .macro line)")
        if name.lower() in self._macros:
            raise MacroError("Macro '{}' already defined".format(name))
        self._current = MacroDefinition(name, line_num)

    def EndDefinition(self):
        self._macros[self._current.name.lower()] = self._current
        self._current = None

    def AddLocal(self, args_str):
        if not args_str:
            raise MacroError("'.local' requires at least one symbol name")
        for sym in (a.strip() for a in args_str.split(',')):
            if sym:
                self._current.locals.append(sym)

    def AddBodyLine(self, parsed_line):
        self._current.body.append(parsed_line)

    def Expand(self, name, args_str, invocation_label, line_num):
        """Expand one invocation of the named macro into a list of
        ParsedLine (already parameter/local substituted). 'args_str' is the
        raw comma-separated argument text from the invocation; if
        'invocation_label' is given, it becomes the label on the first line
        of the expansion (matching how a label on a macro-invocation line
        behaves in TASM: it labels the macro's expansion site).
        """
        macro_def = self._macros[name.lower()]
        args = [a.strip() for a in args_str.split(',')] if args_str else []

        self._expansion_count += 1
        # Suffix that makes this expansion's LOCAL symbols unique: distinct
        # per invocation, so the same macro called more than once doesn't
        # produce two definitions of the same label.
        suffix = '__{}_{}'.format(macro_def.name, self._expansion_count)

        def substitute(text):
            if text is None:
                return None

            def repl_param(m):
                index = int(m.group('index'))
                if index > len(args):
                    raise MacroError(
                        "Macro '{}' invoked with too few arguments (body "
                        "references ]{}, only {} given)".format(
                            macro_def.name, index, len(args)))
                return args[index - 1]

            text = _param_re.sub(repl_param, text)
            for sym in macro_def.locals:
                text = re.sub(r'\b{}\b'.format(re.escape(sym)), sym + suffix, text)
            return text

        expanded = []
        for i, pl in enumerate(macro_def.body):
            label = substitute(pl.label)
            if i == 0 and invocation_label:
                if label:
                    raise MacroError(
                        "Macro '{}' invocation has a label ('{}') but its "
                        "body's first line already has one ('{}')".format(
                            macro_def.name, invocation_label, label))
                label = invocation_label
            expanded.append(ParsedLine(
                line_num=line_num,
                label=label,
                directive=substitute(pl.directive),
                instruction=substitute(pl.instruction),
                comment=pl.comment,
            ))
        return expanded
