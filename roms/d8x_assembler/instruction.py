
class InstructionError(Exception):
    pass

class RangeError(Exception):
    pass

def OpNum8(context, output, operands):
    val = int(operands.pop(0))
    if val < -128 or val > 255:
        raise RangeError('\'{}\' larger than 8 bits'.format(val))
    output.extend(val.to_bytes(1, byteorder='big', signed=(val < 0)))

def OpNum16(context, output, operands):
    val = int(operands.pop(0))
    if val < -32768 or val > 65535:
        raise RangeError('\'{}\' larger than 16 bits'.format(val))
    output.extend(val.to_bytes(2, byteorder='big', signed=(val < 0)))

def OpIndX(context, output, operands):
    val = int(operands.pop(0))
    if val < 0 or val > 127:
        raise RangeError('\'{}\' larger than 7 bits'.format(val))
    output.extend(val.to_bytes(1, byteorder='big'))

def OpIndY(context, output, operands):
    val = int(operands.pop(0))
    if val < 0 or val > 127:
        raise RangeError('\'{}\' larger than 7 bits'.format(val))
    val |= 0x80
    output.extend(val.to_bytes(1, byteorder='big'))

def OpRel8(context, output, operands):
    val = int(operands.pop(0))
    offset = val - (context.Pc + len(output) + 1)           
    if offset < -128 or offset > 127:
        raise RangeError('\'{}\' larger than signed 8 bits'.format(val))
    output.extend(offset.to_bytes(1, signed=True, byteorder='big'))

def OpBitNumAddr(context, output, operands):
    bit_num = int(operands.pop(0))
    if bit_num < 0 or bit_num > 7:
        raise RangeError('\'{}\' out of the range 0..7'.format(bit_num))
    val = bit_num << 5
    address = int(operands.pop(0))
    if address >= 0x20 and address <= 0x2F:
        address -= 0x20
    elif address >= 0x40 and address <= 0x4F:
        address -= 0x30
    else:
        raise RangeError('Address \'{}h\' out of the range 0x20..0x2F and 0x40..0x4F'.format(hex(address)))
    val |= address
    output.extend(val.to_bytes(1, byteorder='big'))


opcodes = {
    'db' : {
        '#nn' : [OpNum8],
    },
    'add' : {
        'a' : {
            'b'     : [0x08],
            '#nn'   : [0xC0, OpNum8],
            'nn'    : [0xD0, OpNum8],
            'nnnn'  : [0xF0, OpNum16],
            'x+nn'  : [0xE0, OpIndX],
            'y+nn'  : [0xE0, OpIndY],
        },
        'b' : {
            '#nn'   : [0xC1, OpNum8],
            'nn'    : [0xD1, OpNum8],
            'nnnn'  : [0xF1, OpNum16],
            'x+nn'  : [0xE1, OpIndX],
            'y+nn'  : [0xE1, OpIndY],
        },
        'd' : {
            '#nnnn' : [0x87, OpNum16],
            'nn'    : [0x97, OpNum8],
            'nnnn'  : [0xB7, OpNum16],
            'x+nn'  : [0xA7, OpIndX],
            'y+nn'  : [0xA7, OpIndY],
        },

        'x' : {
            'a'     : [0x0C],
            'b'     : [0x0E],
        },
        'y' : {
            'a'     : [0x0D],
            'b'     : [0x0F],
        }
    },
    'addc' : {
        'a' : {
            '#nn'   : [0x80, OpNum8],
            'nn'    : [0x90, OpNum8],
            'nnnn'  : [0xB0, OpNum16],
            'x+nn'  : [0xA0, OpIndX],
            'y+nn'  : [0xA0, OpIndY],
        }
    },
    'adj' : {
        'a' : [0x5E],
    },
    'and' : {
        'a' : {
            '#nn'   : [0xC2, OpNum8],
            'nn'    : [0xD2, OpNum8],
            'nnnn'  : [0xF2, OpNum16],
            'x+nn'  : [0xE2, OpIndX],
            'y+nn'  : [0xE2, OpIndY],
        },
        'b' : {
            '#nn'   : [0xC3, OpNum8],
            'nn'    : [0xD3, OpNum8],
            'nnnn'  : [0xF3, OpNum16],
            'x+nn'  : [0xE3, OpIndX],
            'y+nn'  : [0xE3, OpIndY],
        }
    },

    'bcc' : {
        'nnnn'  : [0x44, OpRel8],
        '#nn'   : [0x44, OpNum8],
    },
    'bcs' : {
        'nnnn'  : [0x45, OpRel8],
        '#nn'   : [0x45, OpNum8],
    },
    'beq' : {
        'nnnn'  : [0x47, OpRel8],
        '#nn'   : [0x47, OpNum8],
    },
    'bgea' : {
        'nnnn'  : [0x4C, OpRel8],
        '#nn'   : [0x4C, OpNum8],
    },
    'bgt' : {
        'nnnn'  : [0x42, OpRel8],
        '#nn'   : [0x42, OpNum8],
    },
    'bgta' : {
        'nnnn'  : [0x4E, OpRel8],
        '#nn'   : [0x4E, OpNum8],
    },
    'ble' : {
        'nnnn'  : [0x43, OpRel8],
        '#nn'   : [0x43, OpNum8],
    },
    'blea' : {
        'nnnn'  : [0x4F, OpRel8],
        '#nn'   : [0x4F, OpNum8],
    },
    'blta' : {
        'nnnn'  : [0x4D, OpRel8],
        '#nn'   : [0x4D, OpNum8],
    },
    'bmi' : {
        'nnnn'  : [0x4B, OpRel8],
        '#nn'   : [0x4B, OpNum8],
    },
    'bne' : {
        'nnnn'  : [0x46, OpRel8],
        '#nn'   : [0x46, OpNum8],
    },
    'bpz' : {
        'nnnn'  : [0x4A, OpRel8],
        '#nn'   : [0x4A, OpNum8],
    },
    'bra' : {
        'nnnn'  : [0x40, OpRel8],
        '#nn'   : [0x40, OpNum8],
    },
    'brn' : {
        'nnnn'  : [0x41, OpRel8],
        '#nn'   : [0x41, OpNum8],
    },
    'bsr' : {
        'nnnn'  : [0x61, OpRel8],
        '#nn'   : [0x61, OpNum8],
    },
    'bvc' : {
        'nnnn'  : [0x48, OpRel8],
        '#nn'   : [0x48, OpNum8],
    },
    'bvs' : {
        'nnnn'  : [0x49, OpRel8],
        '#nn'   : [0x49, OpNum8],
    },

    'clr' : {
        'a'     : [0x52],
        'b'     : [0x53],
        'nn'    : [0x72, OpNum8],
        'x+nn'  : [0x62, OpIndX],
        'y+nn'  : [0x62, OpIndY],
    },

    'clrb' : {
        '#nn' : {
            'nn' : [0x75, OpBitNumAddr]
        }
    },

    'clrc' : [0x65],
    'clrv' : [0x25],

    'cmp' : {
        '#nn' :
        {
            'nn'    : [0x79, OpNum8, OpNum8],
        },
        'a' :
        {
            'b'     : [0x0B],
            '#nn'   : [0xCC, OpNum8],
            'nn'    : [0xDC, OpNum8],
            'nnnn'  : [0xFC, OpNum16],
            'x+nn'  : [0xEC, OpIndX],
            'y+nn'  : [0xEC, OpIndY], 
        },
        'b' :
        {
            '#nn'   : [0xCD, OpNum8],
            'nn'    : [0xDD, OpNum8],
            'nnnn'  : [0xFD, OpNum16],
            'x+nn'  : [0xED, OpIndX],
            'y+nn'  : [0xED, OpIndY], 
        },
        'd' :
        {
            '#nnnn' : [0x89, OpNum16],
            'nn'    : [0x99, OpNum8],
            'nnnn'  : [0xB9, OpNum16],
            'x+nn'  : [0xA9, OpIndX],
            'y+nn'  : [0xA9, OpIndY], 
        },
        'x' :
        {
            '#nnnn' : [0x8C, OpNum16],
            'nn'    : [0x9C, OpNum8],
            'nnnn'  : [0xBC, OpNum16],
            'x+nn'  : [0xAC, OpIndX],
            'y+nn'  : [0xAC, OpIndY], 
        },
        'y' :
        {
            '#nnnn' : [0x8D, OpNum16],
            'nn'    : [0x9D, OpNum8],
            'nnnn'  : [0xBD, OpNum16],
            'x+nn'  : [0xAD, OpIndX],
            'y+nn'  : [0xAD, OpIndY], 
        }
    },

    'cmpb' :
    {
        'a' :
        {
            '#nn'   : [0xCE, OpNum8],
            'nn'    : [0xDE, OpNum8],
            'nnnn'  : [0xFE, OpNum16],
            'x+nn'  : [0xEE, OpIndX],
            'y+nn'  : [0xEE, OpIndY], 

        },
        'b' :
        {
            '#nn'   : [0xCF, OpNum8],
            'nn'    : [0xDF, OpNum8],
            'nnnn'  : [0xFF, OpNum16],
            'x+nn'  : [0xEF, OpIndX],
            'y+nn'  : [0xEF, OpIndY], 
        }
    },

    'cmpz' :
    {
        'a' : [0x58],
        'b' : [0x59],
    },

    'dec' :
    {
        'x+nn'  : [0x60, OpIndX],
        'y+nn'  : [0x60, OpIndY],
        'a'     : [0x50],
        'b'     : [0x51],
        's'     : [0x2F],
        'x'     : [0x1E],
        'y'     : [0x1F],
        'nn'    : [0x70, OpNum8],
    },

    'di' : [0x05],

    'div' : 
    {
        'd' :
        {
            '#nn'   : [0x85, OpNum8],
            'nn'    : [0x95, OpNum8],
            'nnnn'  : [0xB5, OpNum16],
            'x+nn'  : [0xA5, OpIndX],
            'y+nn'  : [0xA5, OpIndY],
        }
    },

    'ei' : [0x07],

    'inc' :
    {
        'x+nn'  : [0x66, OpIndX],
        'y+nn'  : [0x66, OpIndY],
        'a'     : [0x56],
        'b'     : [0x57],
        's'     : [0x2D],
        'x'     : [0x1C],
        'y'     : [0x1D],
        'nn'    : [0x76, OpNum8],
    },

    'jmp' : 
    {
        'x+nn'  : [0x23, OpIndX],
        'y+nn'  : [0x23, OpIndY],
        'nnnn'  : [0x03, OpNum16],            
    },

    'jsr' : 
    {
        'x+nn'  : [0x21, OpIndX],
        'y+nn'  : [0x21, OpIndY],
        'nn'    : [0x31, OpNum8],
        'nnnn'  : [0x01, OpNum16],            
    },

    'ld' :
    {
        '#nn' :
        {
            'nn'    : [0x33, OpNum8, OpNum8],
        },
        'a' :
        {
            '[y]'   : [0x1A],
            '#nn'   : [0xCA, OpNum8],
            'nn'    : [0xDA, OpNum8],
            'nnnn'  : [0xFA, OpNum16],
            'x+nn'  : [0xEA, OpIndX],
            'y+nn'  : [0xEA, OpIndY],
        },
        'b' :
        {
            '#nn'   : [0xCB, OpNum8],
            'nn'    : [0xDB, OpNum8],
            'nnnn'  : [0xFB, OpNum16],
            'x+nn'  : [0xEB, OpIndX],
            'y+nn'  : [0xEB, OpIndY],
        },
        'd' :
        {
            '[y]'   : [0x1B],
            '#nnnn' : [0x86, OpNum16],
            'nn'    : [0x96, OpNum8],
            'nnnn'  : [0xB6, OpNum16],
            'x+nn'  : [0xA6, OpIndX],
            'y+nn'  : [0xA6, OpIndY],
        },
        's' :
        {
            '#nnnn' : [0x8B, OpNum16],
            'nn'    : [0x9B, OpNum8],
            'nnnn'  : [0xBB, OpNum16],
            'x+nn'  : [0xAB, OpIndX],
            'y+nn'  : [0xAB, OpIndY],
        },
        'x' :
        {
            '#nnnn' : [0x8E, OpNum16],
            'nn'    : [0x9E, OpNum8],
            'nnnn'  : [0xBE, OpNum16],
            'x+nn'  : [0xAE, OpIndX],
            'y+nn'  : [0xAE, OpIndY],
        },
        'y' :
        {
            '#nnnn' : [0x8F, OpNum16],
            'nn'    : [0x9F, OpNum8],
            'nnnn'  : [0xBF, OpNum16],
            'x+nn'  : [0xAF, OpIndX],
            'y+nn'  : [0xAF, OpIndY],
        }
    },

    'mov' :
    {
        'a' :
        {
            'b'     : [0x5B],
            'ocr'   : [0x5D],
        },
        'b' :
        {
            'a'     : [0x5A],
        },
        'd' :
        {
            'x'     : [0x3E],
            'y'     : [0x3F],
        },
        'ocr' :
        {
            'a'     : [0x5C],                
        },
        's' :
        {
            'x'     : [0x2E],
        },
        'x' :
        {
            's'     : [0x2C],
            'd'     : [0x3C],
        },
        'y' :
        {
            'd'     : [0x3D],
        },
    },
    'mul' :
    {
        'a' :
        {
            '#nn'   : [0x81, OpNum8],
            'nn'    : [0x91, OpNum8],
            'nnnn'  : [0xB1, OpNum16],
            'x+nn'  : [0xA1, OpIndX],
            'y+nn'  : [0xA1, OpIndY],
        }
    },

    'neg' : {
        'a'     : [0x54],
        'b'     : [0x55],
        'nn'    : [0x74, OpNum8],
        'x+nn'  : [0x64, OpIndX],
        'y+nn'  : [0x64, OpIndY],
    },

    'nmi' : [0x5F],
    'nop' : [0x00],

    'or' : {
        'a' : {
            '#nn'   : [0xC6, OpNum8],
            'nn'    : [0xD6, OpNum8],
            'nnnn'  : [0xF6, OpNum16],
            'x+nn'  : [0xE6, OpIndX],
            'y+nn'  : [0xE6, OpIndY],
        },
        'b' : {
            '#nn'   : [0xC7, OpNum8],
            'nn'    : [0xD7, OpNum8],
            'nnnn'  : [0xF7, OpNum16],
            'x+nn'  : [0xE7, OpIndX],
            'y+nn'  : [0xE7, OpIndY],
        }
    },

    'pull' :
    {
        'a'     : [0x7C],
        'b'     : [0x7D],
        'd'     : [0x78],
        'x'     : [0x7E],
        'y'     : [0x7F],
    },

    'push' :
    {
        'a'     : [0x6C],
        'b'     : [0x6D],
        'd'     : [0x68],
        'x'     : [0x6E],
        'y'     : [0x6F],
    },

    'ret'   : [0x63],
    'reti'  : [0x73],

    'rolc' :
    {
        'x+nn'  : [0x26, OpIndX],
        'y+nn'  : [0x26, OpIndY],
        'a'     : [0x16],
        'b'     : [0x17],
        'nn'    : [0x36, OpNum8],
    },

    'rorc' :
    {
        'x+nn'  : [0x24, OpIndX],
        'y+nn'  : [0x24, OpIndY],
        'a'     : [0x14],
        'b'     : [0x15],
        'nn'    : [0x34, OpNum8],
    },

    'setb' : {
        '#nn' : {
            'nn' : [0x77, OpBitNumAddr]
        }
    },

    'setc' : [0x67],
    'setv' : [0x27],

    'shl' :
    {
        'a'     : [0x12],
        'b'     : [0x13],
        'd'     : [0x06],
        'x'     : [0x22],
        'nn'    : [0x32, OpNum8],
    },

    'shr' :
    {
        'a'     : [0x10],
        'b'     : [0x11],
        'd'     : [0x04],
        'x'     : [0x20],
        'nn'    : [0x30, OpNum8],
    },

    'shra' :
    {
        'x+nn'  : [0x28, OpIndX],
        'y+nn'  : [0x28, OpIndY],
        'a'     : [0x18],
        'b'     : [0x19],
        'nn'    : [0x38, OpNum8],
    },

    'st' :
    {
        'a' :
        {
            '[y]'   : [0x82],
            'nn'    : [0x92, OpNum8],
            'nnnn'  : [0xB2, OpNum16],
            'x+nn'  : [0xA2, OpIndX],
            'y+nn'  : [0xA2, OpIndY],
        },
        'b' :
        {
            'nn'    : [0x93, OpNum8],
            'nnnn'  : [0xB3, OpNum16],
            'x+nn'  : [0xA3, OpIndX],
            'y+nn'  : [0xA3, OpIndY],
        },
        'd' :
        {
            '[y]'   : [0x8A],
            'nn'    : [0x9A, OpNum8],
            'nnnn'  : [0xBA, OpNum16],
            'x+nn'  : [0xAA, OpIndX],
            'y+nn'  : [0xAA, OpIndY]
        },
        's' :
        {
            'nn'    : [0x39, OpNum8],
            'x+nn'  : [0x29, OpIndX],
            'y+nn'  : [0x29, OpIndY],
        },
        'x' :
        {
            'nn'    : [0x3A, OpNum8],
            'nnnn'  : [0x0A, OpNum16],
            'x+nn'  : [0x2A, OpIndX],
            'y+nn'  : [0x2A, OpIndY],
        },
        'y' :
        {
            'nn'    : [0x3B, OpNum8],
            'nnnn'  : [0x0B, OpNum16],
            'x+nn'  : [0x2B, OpIndX],
            'y+nn'  : [0x2B, OpIndY],
        }
    },

    'sub' : {
        'a' : {
            'b'     : [0x09],
            '#nn'   : [0xC4, OpNum8],
            'nn'    : [0xD4, OpNum8],
            'nnnn'  : [0xF4, OpNum16],
            'x+nn'  : [0xE4, OpIndX],
            'y+nn'  : [0xE4, OpIndY],
        },
        'b' : {
            '#nn'   : [0xC5, OpNum8],
            'nn'    : [0xD5, OpNum8],
            'nnnn'  : [0xF5, OpNum16],
            'x+nn'  : [0xE5, OpIndX],
            'y+nn'  : [0xE5, OpIndY],
        },
        'd' : {
            '#nnnn' : [0x88, OpNum16],
            'nn'    : [0x98, OpNum8],
            'nnnn'  : [0xB8, OpNum16],
            'x+nn'  : [0xA8, OpIndX],
            'y+nn'  : [0xA8, OpIndY],
        },
    },

    'subc' : {
        'a' : {
            '#nn'   : [0x84, OpNum8],
            'nn'    : [0x94, OpNum8],
            'nnnn'  : [0xB4, OpNum16],
            'x+nn'  : [0xA4, OpIndX],
            'y+nn'  : [0xA4, OpIndY],
        }
    },

    'tbbc' :
    {
        '#nn' : {
            'nn' : {
                'nnnn' : [0x37, OpBitNumAddr, OpRel8],
            }
        }
    },

    'tbbs' : {
        '#nn' : {
            'nn' : {
                'nnnn' : [0x35, OpBitNumAddr, OpRel8],
            }
        }
    },

    'tbs' : {
        '#nn' : {
            'nn'    : [0x71, OpBitNumAddr]
        }
    },

    'wait'          : [0x83],

    'xch' :
    {
        'a' :
        {
            'b'     : [0x02],
            'x+nn'  : [0x6A, OpIndX],
            'y+nn'  : [0x6A, OpIndY],
            'nn'    : [0x7A, OpNum8],
        },
        'b' :
        {
            'x+nn'  : [0x6B, OpIndX],
            'y+nn'  : [0x6B, OpIndY],
            'nn'    : [0x7B, OpNum8],
        },
        'x' : 
        {
            'y'     : [0x69],
        }
    },

    'xor' : {
        'a' : {
            '#nn'   : [0xC8, OpNum8],
            'nn'    : [0xD8, OpNum8],
            'nnnn'  : [0xF8, OpNum16],
            'x+nn'  : [0xE8, OpIndX],
            'y+nn'  : [0xE8, OpIndY],
        },
        'b' : {
            '#nn'   : [0xC9, OpNum8],
            'nn'    : [0xD9, OpNum8],
            'nnnn'  : [0xF9, OpNum16],
            'x+nn'  : [0xE9, OpIndX],
            'y+nn'  : [0xE9, OpIndY],
        }
    },
}

registers = [
    'a', 'b', 'd', 'x', 'y', 's', 'ocr'
]

def Assemble(context, opcode, operands):

    opcode = opcode.lower()
    if opcode not in opcodes:        
        raise InstructionError('Unknown opcode \'{}\''.format(opcode))

    # dictionary of candidate instructions
    instructions = [opcodes[opcode]]

    # split string into individual operands, stripping all whitespace
    operand_list = [x.replace(' ','') for x in operands.split(',')] if operands else []
    operand_values = []

    # classify each operand into an addressing mode, filtering out instructions that don't match
    for operand in operand_list:

        op_lower = operand.lower()

        # check if register addressing
        if op_lower in registers:
            op_mode_list = [op_lower]
            op_val = None

        # check if immediate addressing
        elif op_lower.startswith('#'):
            op_val = operand[1:]
            op_mode_list = ['#nn','#nnnn']

        # check if indirect indexed x addressing
        elif op_lower.startswith('x+'):
            op_val = operand[2:]
            op_mode_list = ['x+nn']

        # check if indirect indexed y addressing
        elif op_lower.startswith('y+'):
            op_val = operand[2:]
            op_mode_list = ['y+nn']

        # check if post increment indirect y addressing
        elif op_lower == '[y]':
            op_val = None
            op_mode_list = ['[y]']

        # special case for bit numbers
        elif op_lower.startswith('bit'):
            op_val = operand[3:]
            op_mode_list = ['#nn']

        # must be absolute addressing
        else:
            op_val = operand
            op_mode_list = ['nn','nnnn']

        # If operand has a value than keep it for evaluation later
        if op_val is not None:
            operand_values.append(op_val)

        # filter list of instructions by operand
        instructions_matched = []
        for instruction in instructions:

            # instruction must be dictionary to lookup operands
            if isinstance(instruction, dict):                

                # check each candidate operand mode as there may be more then
                # one (i.e #nn and #nnnn)
                for op_mode in op_mode_list:

                    # add instruction to list if match for operand mode 
                    if i := instruction.get(op_mode):
                        instructions_matched.append(i)
            else:
                raise InstructionError('Too many operands')

        instructions = instructions_matched
        
        # if list is empty then there was no match for operand 
        if not instructions:
            raise InstructionError('Operand \'{}\' not supported for \'{} {}\''.format(operand, opcode, operands))

    # if opcode_info still a dictionary then there are not enough operands for this opcode
    for i in instructions:
        if isinstance(i, dict):
            raise InstructionError('Not enough operands for \'{} {}\''.format(opcode, operands))

    # evaluate each operand value            
    operands = []
    for operand in operand_values:
        operands.append(context.ParseExpression(operand))
        
    # try each instruction encoding until one can be assembled
    for instruction in instructions:
        try:
            # reset byte array and copy list of operands
            instruction_bytes = bytearray()
            instruction_operands = operands.copy()

            # iterate through op_byte
            for op_byte in instruction:

                # if byte is a raw value, just copy it to output array                    
                if isinstance(op_byte, int):
                    instruction_bytes.append(op_byte)
                else:
                    op_byte(context, instruction_bytes, instruction_operands)     

            # instruction assembled without exception, so break out of loop
            break

        except RangeError:
            # One of the operands is out of range for this instruction encoding, but
            # keep checking other instruction encodings
            pass

    else:
        raise InstructionError("No instruction encoding for \'{} {}\'".format(opcode, operands))

    context.AddBytes(instruction_bytes)


