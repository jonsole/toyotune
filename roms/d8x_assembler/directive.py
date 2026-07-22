class DirectiveError(Exception):
    pass

    
def HandleOrg(context, arguments):
    context.SetPc(context.ParseExpression(arguments[0]))

def HandleBlock(context, arguments):
    size = context.ParseExpression(arguments[0])
    context.SetPc(context.Pc + size)

def HandleDb(context, arguments):
    b = bytearray()
    for arg in arguments:
        val = context.ParseExpression(arg)
        if val < -128 or val > 255:
            raise DirectiveError('\'{}\' larger than 8 bits'.format(val))
        b.extend(val.to_bytes(1, byteorder='big', signed=(val < 0)))
    context.AddBytes(b)

def HandleDw(context, arguments):
    b = bytearray()
    for arg in arguments:
        val = context.ParseExpression(arg)
        if val < -32768 or val > 65535:
            raise DirectiveError('\'{}\' larger than 8 bits'.format(val))
        b.extend(val.to_bytes(2, byteorder='big', signed=(val < 0)))
    context.AddBytes(b)

def HandleEnd(context, arguments):
    context.End()

def HandleLocalLabelChar(const, arguments):
    pass

def HandleEqu(context, arguments, label):
    # Unlike every other directive, 'equ' binds its label to an expression's
    # value instead of the current PC - the caller (asm_d8x.py) knows to
    # withhold the normal PC-based label assignment for an 'equ' line and
    # pass the label through here instead.
    if not label:
        raise DirectiveError("'equ' requires a label")
    if len(arguments) != 1 or not arguments[0]:
        raise DirectiveError("'equ' requires exactly one argument")
    context.SetLabel(label, context.ParseExpression(arguments[0]))

directives = {
    'org'   : HandleOrg,
    'block' : HandleBlock,
    'db'    : HandleDb,
    'dw'    : HandleDw,
    'end'   : HandleEnd,
    'locallabelchar' : HandleLocalLabelChar
}

def Assemble(context, directive, arguments, label=None):
    argument_list = [x.replace(' ','') for x in arguments.split(',')] if arguments else []

    dir_lower = directive.lower()
    if dir_lower == 'equ':
        HandleEqu(context, argument_list, label)
        return

    if dir_lower not in directives:
        raise DirectiveError('Unknown directive \'{}\''.format(directive))

    directives[dir_lower](context, argument_list)


