'use strict';
// Tests for the D8X extension, run against the real ROM sources.
//
// There is no node install on the bench machine, so run it under VS Code's
// own Electron in node mode:
//
//   ELECTRON_RUN_AS_NODE=1 "<VS Code>/Code.exe" tools/vscode-d8x/test.js
//
// The grammar half needs vscode-textmate and vscode-oniguruma, which VS Code
// ships inside its app directory; it is found automatically, or set
// VSCODE_APP to '<VS Code>/<commit>/resources/app'. Without them only the
// symbol tests run.

const fs = require('fs');
const path = require('path');
const assert = require('assert');
const sym = require('./symbols');

const REPO = path.resolve(__dirname, '..', '..');
const ROMS = path.join(REPO, 'roms', '3S-GTE');

// The Claude/ working copies are UTF-8; everything else is raw CP437, which
// latin1 reads byte-for-byte so the 0x18/0x19 xref arrows survive.
const SOURCES = [
  { file: 'D151803-9651/Claude/D151803-9651.asm', encoding: 'utf8' },
  { file: 'D151803-9661/Claude/D151803-9661.asm', encoding: 'utf8' },
  { file: 'D151804-0461/Claude/D151804-0461.asm', encoding: 'utf8' },
  { file: 'D151803-9651/D151803-9651.ASM', encoding: 'latin1' },
  { file: 'D151803-9651/toyotune/D151803-9651_DIAG16_32K.ASM', encoding: 'latin1' },
];

function load(src) {
  return fs.readFileSync(path.join(ROMS, src.file), src.encoding).split(/\r?\n/);
}

let failures = 0;
function test(name, fn) {
  try {
    fn();
    console.log('  ok   ' + name);
  } catch (e) {
    failures++;
    console.log('  FAIL ' + name + '\n       ' + String(e.message).split('\n').join('\n       '));
  }
}

const REGISTERS = new Set(['a', 'b', 'd', 'x', 'y', 's', 'pc', 'ocr']);

// Every identifier in an operand position, by line.
function* operandSymbols(lines) {
  let inMacro = false;
  for (let i = 0; i < lines.length; i++) {
    const text = lines[i];
    const c = sym.commentStart(text);
    const code = (c < 0 ? text : text.slice(0, c)).replace(/"[^"]*"|'[^']*'/g, '');
    const m = /^(?:[.A-Za-z0-9_$]+:?)?[ \t]+(\.?[A-Za-z_][A-Za-z0-9_$]*)(.*)$/.exec(code);
    if (!m) continue;
    const op = m[1].toLowerCase();
    if (op === '.macro') { inMacro = true; continue; }
    if (op === '.endm') { inMacro = false; continue; }
    if (inMacro || op === '.define') continue;
    const re = /(?<![.A-Za-z0-9_$])\.?[A-Za-z_][A-Za-z0-9_$]*/g;
    let w;
    while ((w = re.exec(m[2])) !== null) {
      const word = w[0];
      if (REGISTERS.has(word.toLowerCase()) || /^bit[0-7]$/i.test(word)) continue;
      yield { line: i, word };
    }
  }
}

// ---------------------------------------------------------------- symbols

console.log('symbols');

for (const src of SOURCES) {
  const lines = load(src);
  const index = sym.buildIndex(lines);

  test(`${src.file}: every operand symbol resolves (${index.defs.size} definitions)`, () => {
    const missing = [];
    for (const { line, word } of operandSymbols(lines)) {
      if (!index.defs.has(word)) missing.push(`${line + 1}: ${word}`);
    }
    assert.deepStrictEqual(missing.slice(0, 10), [], `${missing.length} unresolved`);
  });

  test(`${src.file}: no label is defined twice`, () => {
    assert.deepStrictEqual(index.duplicates.map(d => `${d.line + 1}: ${d.name}`), []);
  });
}

{
  const lines = load(SOURCES[0]);
  const index = sym.buildIndex(lines);
  const find = (needle, from = 0) => {
    for (let i = from; i < lines.length; i++) if (lines[i].includes(needle)) return i;
    throw new Error('not found: ' + needle);
  };
  const at = (line, needle) => sym.wordAt(lines[line], lines[line].indexOf(needle) + 1);

  test('IDA xref suffix in a comment resolves to the bare name', () => {
    // IDA writes 'CODE XREF: callerp' - the type letter glued to the name.
    let checked = 0;
    lines.forEach((t, i) => {
      const m = /XREF: ([A-Za-z_][A-Za-z0-9_]*)[↑↓]?[rwopj]\s*(?:\.\.\.)?$/.exec(t);
      if (!m || !index.defs.has(m[1])) return;
      const r = sym.resolve(index, sym.wordAt(t, t.lastIndexOf(m[1]) + 1));
      assert.strictEqual(r && r.name, m[1], `line ${i + 1}`);
      assert.strictEqual(r.length, m[1].length);
      checked++;
    });
    assert.ok(checked > 10, `only ${checked} bare-name xrefs found`);
  });

  test('xref of the form name+offset<arrow><type> resolves', () => {
    const i = find('factory_self_test+EE\u2193o');
    const r = sym.resolve(index, at(i, 'factory_self_test'));
    assert.strictEqual(r && r.name, 'factory_self_test');
  });

  test('xref suffix is not stripped outside comments', () => {
    const r = sym.resolve(index, { word: 'map_2d_interpolatep', start: 0, end: 19, inComment: false });
    assert.strictEqual(r, null);
  });

  test('mnemonics and registers resolve to nothing', () => {
    const i = find('\t\t\t\tld\td, var_temp_w');
    assert.strictEqual(sym.resolve(index, at(i, 'ld')), null);
    assert.strictEqual(sym.resolve(index, sym.wordAt(lines[i], lines[i].indexOf('d,'))), null);
    assert.strictEqual(sym.resolve(index, at(i, 'var_temp_w')).def.kind, sym.KIND.DATA);
  });

  test('definition kinds', () => {
    assert.strictEqual(index.defs.get('DDRA').kind, sym.KIND.DATA);
    assert.strictEqual(index.defs.get('ignition_timing_to_cpr').kind, sym.KIND.CODE);
    assert.strictEqual(index.defs.get('var_trim_state_alias').kind, sym.KIND.EQU);
    const d = index.defs.get('COUNTER_ARG');
    assert.strictEqual(d.kind, sym.KIND.DEFINE);
    assert.strictEqual(lines[d.line].slice(d.start, d.end), 'COUNTER_ARG');
  });

  test('references: every code use, no xref-list entries', () => {
    const name = 'ignition_timing_to_cpr';
    const all = sym.findOccurrences(lines, index, name);
    const code = all.filter(o => !o.inComment);
    // Independent count: code-side whole-word matches.
    const re = new RegExp('(?<![.A-Za-z0-9_$])' + name + '(?![A-Za-z0-9_$])');
    const expected = lines.filter(t => {
      const c = sym.commentStart(t);
      return re.test(c < 0 ? t : t.slice(0, c));
    }).length;
    assert.strictEqual(code.length, expected);
    assert.ok(code.length >= 2, 'expected a definition and a call');
    for (const o of all) {
      const x = index.xref[o.line];
      assert.ok(x < 0 || o.start < x, `xref entry counted at line ${o.line + 1}`);
    }
  });

  test('references: includeComments=false keeps only code', () => {
    const all = sym.findOccurrences(lines, index, 'var_flags_4E');
    const codeOnly = sym.findOccurrences(lines, index, 'var_flags_4E', { includeComments: false });
    assert.ok(all.length > codeOnly.length);
    assert.ok(codeOnly.every(o => !o.inComment));
  });

  test('references do not match inside longer names', () => {
    for (const o of sym.findOccurrences(lines, index, 'TIMER')) {
      const t = lines[o.line];
      assert.ok(!/[A-Za-z0-9_$]/.test(t[o.end] || ''), `line ${o.line + 1}: ${t.trim()}`);
    }
  });

  test('hover: header block above a routine, banners and xrefs dropped', () => {
    const d = sym.describe(lines, index, index.defs.get('ignition_timing_to_cpr'));
    const text = d.above.join('\n');
    assert.ok(text.includes('convert ignition advance'), text);
    assert.ok(!/XREF/.test(text + d.beside.join('\n')));
    assert.ok(!/^; -{10,}$/m.test(text), 'rule lines should be dropped');
    assert.strictEqual(d.code, 'ignition_timing_to_cpr:');
  });

  test('hover: data comment beside it, xref list dropped, neighbour excluded', () => {
    const ddra = sym.describe(lines, index, index.defs.get('DDRA'));
    assert.deepStrictEqual(ddra.beside, ['Port A i/o config']);
    // DDRB follows DDRA's indented continuation lines, which are not DDRB's.
    const ddrb = sym.describe(lines, index, index.defs.get('DDRB'));
    assert.deepStrictEqual(ddrb.above, []);
    assert.deepStrictEqual(ddrb.beside, ['Port B i/o config']);
  });

  test('indexes a full ROM quickly', () => {
    const t0 = process.hrtime.bigint();
    for (let k = 0; k < 10; k++) sym.buildIndex(lines);
    const ms = Number(process.hrtime.bigint() - t0) / 1e7;
    assert.ok(ms < 100, `${ms.toFixed(1)} ms per index`);
    console.log(`       (${ms.toFixed(1)} ms per index of ${lines.length} lines)`);
  });
}

{
  const lines = load(SOURCES[3]);
  const index = sym.buildIndex(lines);
  test('raw CP437 source: 0x18/0x19 xref arrows are handled', () => {
    let offsetForm = 0, bareForm = 0;
    lines.forEach((t, i) => {
      if (index.xref[i] < 0) return;
      // name+offset<arrow><type>, and the bare name<arrow><type>
      const re = /(?<![A-Za-z0-9_:])([A-Za-z_][A-Za-z0-9_]*)(\+[0-9A-F]+)?[][rwopj]/g;
      let m;
      while ((m = re.exec(t)) !== null) {
        if (!index.defs.has(m[1])) continue;
        const r = sym.resolve(index, sym.wordAt(t, m.index + 1));
        assert.strictEqual(r && r.name, m[1], `line ${i + 1}: ${t.trim()}`);
        if (m[2]) offsetForm++; else bareForm++;
      }
    });
    assert.ok(offsetForm > 100 && bareForm > 10, `offset ${offsetForm}, bare ${bareForm}`);
  });
}

{
  const lines = load(SOURCES[4]);
  const index = sym.buildIndex(lines);
  test('DIAG16: .locallabelchar labels resolve with their dot', () => {
    assert.strictEqual(index.defs.get('.diag_flush').kind, sym.KIND.CODE);
    const i = lines.findIndex(t => /^\s+\S+\s+.*\.diag_flush\b/.test(t));
    assert.ok(i >= 0, 'no reference to .diag_flush');
    const hit = sym.wordAt(lines[i], lines[i].indexOf('.diag_flush') + 3);
    assert.strictEqual(sym.resolve(index, hit).name, '.diag_flush');
  });
}

// ---------------------------------------------------------------- grammar

function findVscodeApp() {
  if (process.env.VSCODE_APP) return process.env.VSCODE_APP;
  const root = path.join(process.env.LOCALAPPDATA || '', 'Programs', 'Microsoft VS Code');
  if (!fs.existsSync(root)) return null;
  let current = null;
  try { current = fs.readFileSync(path.join(root, 'updating_version'), 'utf8').trim().slice(0, 10); } catch (e) { /* none */ }
  const dirs = fs.readdirSync(root).filter(d => fs.existsSync(path.join(root, d, 'resources', 'app')));
  const pick = dirs.find(d => d === current) || dirs[0];
  return pick ? path.join(root, pick, 'resources', 'app') : null;
}

async function grammarTests() {
  const app = findVscodeApp();
  let tm, onig;
  try {
    tm = require(path.join(app, 'node_modules.asar', 'vscode-textmate'));
    onig = require(path.join(app, 'node_modules.asar', 'vscode-oniguruma'));
  } catch (e) {
    console.log('grammar: skipped (vscode-textmate not found; set VSCODE_APP)');
    return;
  }
  console.log('grammar');
  const wasm = fs.readFileSync(path.join(app, 'node_modules.asar.unpacked', 'vscode-oniguruma', 'release', 'onig.wasm'));
  await onig.loadWASM(wasm.buffer.slice(wasm.byteOffset, wasm.byteOffset + wasm.byteLength));
  const registry = new tm.Registry({
    onigLib: Promise.resolve({
      createOnigScanner: s => new onig.OnigScanner(s),
      createOnigString: s => new onig.OnigString(s),
    }),
    loadGrammar: async () => tm.parseRawGrammar(
      fs.readFileSync(path.join(__dirname, 'syntaxes', 'd8x.tmLanguage.json'), 'utf8'), 'd8x.tmLanguage.json'),
  });
  const grammar = await registry.loadGrammar('source.d8x');

  // Returns [text, innermost scope] for every non-blank token of one line.
  const tokenize = line => grammar.tokenizeLine(line, tm.INITIAL).tokens
    .map(t => [line.slice(t.startIndex, t.endIndex), t.scopes[t.scopes.length - 1]])
    .filter(([s]) => s.trim() !== '');
  const scopeOf = (line, text) => {
    const hit = tokenize(line).find(([s]) => s.trim() === text);
    return hit ? hit[1] : '(no token "' + text + '")';
  };
  const expect = (line, pairs) => () => {
    for (const [text, scope] of pairs) assert.strictEqual(scopeOf(line, text), scope, `"${text}" in ${JSON.stringify(line)}`);
  };

  test('load/store with register and symbol', expect('\t\t\t\tld\td, var_temp_w', [
    ['ld', 'keyword.other.mnemonic.d8x'], ['d', 'variable.language.register.d8x'],
    ['var_temp_w', 'variable.other.d8x']]));
  test('code label with xref comment', expect('loc_C47C:\t\t\t\t\t\t\t; CODE XREF: sub_C476+2j', [
    ['loc_C47C', 'entity.name.function.label.d8x'], ['CODE XREF:', 'comment.line.xref.d8x']]));
  test('data label and directive', expect('DDRA:\t\t\t\t.block 1\t\t\t; DATA XREF: ROM:C636w', [
    ['DDRA', 'entity.name.variable.d8x'], ['.block', 'keyword.control.directive.d8x'],
    ['1', 'constant.numeric.decimal.d8x']]));
  test('branch to an unnamed target', expect('\t\t\t\tbcc\tloc_C47C', [
    ['bcc', 'keyword.control.flow.d8x'], ['loc_C47C', 'entity.name.function.unnamed.d8x']]));
  test('bit-test branch', expect('\t\t\t\ttbbc\tbit3, var_flags_4E, loc_D905', [
    ['tbbc', 'keyword.control.flow.d8x'], ['bit3', 'constant.language.bit.d8x'],
    ['var_flags_4E', 'variable.other.d8x'], ['loc_D905', 'entity.name.function.unnamed.d8x']]));
  test('call to a named routine', expect('\t\t\t\tjsr\tvalidate_nv_trim_pim', [
    ['jsr', 'keyword.control.flow.d8x'], ['validate_nv_trim_pim', 'entity.name.function.d8x']]));
  test('indexed operand and trailing comment', expect('\t\t\t\tld\ta, y + 01h\t\t; A = max', [
    ['y', 'variable.language.register.d8x'], ['01h', 'constant.numeric.hex.d8x'],
    ['A = max', 'comment.line.semicolon.d8x']]));
  test('immediate and hardware register', expect('\t\t\t\tst\ta, PORTA', [
    ['PORTA', 'variable.other.constant.hwreg.d8x']]));
  test('immediate hex', expect('\t\t\t\tcmp\td, #0AA55h', [
    ['#', 'keyword.operator.immediate.d8x'], ['0AA55h', 'constant.numeric.hex.d8x']]));
  test('.define', expect('\t\t\t\t.define COUNTER_ARG(counter, n) ((counter << 8) + n)', [
    ['.define', 'keyword.control.directive.d8x'], ['COUNTER_ARG', 'entity.name.function.macro.d8x'],
    ['counter, n', 'variable.parameter.d8x']]));
  test('.equ alias', expect('var_trim_state_alias:\t\t.equ var_flags_4E', [
    ['var_trim_state_alias', 'entity.name.variable.d8x'], ['.equ', 'keyword.control.directive.d8x']]));
  test('unnamed data symbol', expect('\t\t\t\tst\ty, unk_7A', [['unk_7A', 'variable.other.unnamed.d8x']]));
  test('register is not a symbol inside a longer word', expect('\t\t\t\tld\ta, ab_count', [
    ['ab_count', 'variable.other.d8x']]));

  for (const src of SOURCES) {
    test(`${src.file}: every instruction is a known mnemonic or directive`, () => {
      const lines = load(src);
      let state = tm.INITIAL;
      const unknown = [];
      const t0 = Date.now();
      lines.forEach((line, i) => {
        const r = grammar.tokenizeLine(line, state);
        state = r.ruleStack;
        for (const t of r.tokens) {
          if (t.scopes.includes('entity.name.function.macro.call.d8x')) {
            unknown.push(`${i + 1}: ${line.slice(t.startIndex, t.endIndex)}`);
          }
        }
      });
      assert.deepStrictEqual(unknown.slice(0, 10), [], `${unknown.length} unknown`);
      console.log(`       (${lines.length} lines in ${Date.now() - t0} ms)`);
    });
  }
}

grammarTests().then(() => {
  console.log(failures ? `\n${failures} failed` : '\nall passed');
  process.exit(failures ? 1 : 0);
}, e => {
  console.error(e);
  process.exit(2);
});
