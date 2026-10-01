'use strict';
// VS Code glue for D8X assembly. All the parsing lives in symbols.js; this
// file only adapts it to the editor's provider interfaces.

const vscode = require('vscode');
const sym = require('./symbols');

const SELECTOR = { language: 'd8x' };

// One index per open document, rebuilt when its version changes. A full
// ROM is ~22k lines and indexes in a few milliseconds, so there is no
// incremental update.
const cache = new Map();

function analyse(document) {
  const key = document.uri.toString();
  const hit = cache.get(key);
  if (hit && hit.version === document.version) return hit;
  const lines = document.getText().split(/\r?\n/);
  const entry = { version: document.version, lines, index: sym.buildIndex(lines) };
  cache.set(key, entry);
  return entry;
}

function lookup(document, position) {
  const { lines, index } = analyse(document);
  const hit = sym.wordAt(lines[position.line] || '', position.character);
  const r = sym.resolve(index, hit);
  if (!r) return null;
  const origin = new vscode.Range(position.line, hit.start, position.line, hit.start + r.length);
  return { lines, index, hit, r, origin };
}

function defRange(def) {
  return new vscode.Range(def.line, def.start, def.line, def.end);
}

function config() {
  return vscode.workspace.getConfiguration('d8x');
}

const KIND_LABEL = {
  [sym.KIND.CODE]: 'code label',
  [sym.KIND.DATA]: 'data',
  [sym.KIND.EQU]: '.equ',
  [sym.KIND.MACRO]: '.macro',
  [sym.KIND.DEFINE]: '.define',
};

const KIND_SYMBOL = {
  [sym.KIND.CODE]: vscode.SymbolKind.Function,
  [sym.KIND.DATA]: vscode.SymbolKind.Variable,
  [sym.KIND.EQU]: vscode.SymbolKind.Constant,
  [sym.KIND.MACRO]: vscode.SymbolKind.Operator,
  [sym.KIND.DEFINE]: vscode.SymbolKind.Operator,
};

const definitionProvider = {
  provideDefinition(document, position) {
    const f = lookup(document, position);
    if (!f) return null;
    return [{
      originSelectionRange: f.origin,
      targetUri: document.uri,
      targetRange: new vscode.Range(f.r.def.line, 0, f.r.def.line, f.lines[f.r.def.line].length),
      targetSelectionRange: defRange(f.r.def),
    }];
  },
};

const referenceProvider = {
  provideReferences(document, position, context) {
    const f = lookup(document, position);
    if (!f) return null;
    const includeComments = config().get('references.includeComments', true);
    const def = f.r.def;
    return sym.findOccurrences(f.lines, f.index, f.r.name, { includeComments })
      .filter(o => context.includeDeclaration || !(o.line === def.line && o.start === def.start))
      .map(o => new vscode.Location(document.uri, new vscode.Range(o.line, o.start, o.line, o.end)));
  },
};

const highlightProvider = {
  provideDocumentHighlights(document, position) {
    const f = lookup(document, position);
    if (!f) return null;
    const def = f.r.def;
    return sym.findOccurrences(f.lines, f.index, f.r.name, { includeComments: true })
      .map(o => new vscode.DocumentHighlight(
        new vscode.Range(o.line, o.start, o.line, o.end),
        o.line === def.line && o.start === def.start
          ? vscode.DocumentHighlightKind.Write
          : vscode.DocumentHighlightKind.Read));
  },
};

const hoverProvider = {
  provideHover(document, position) {
    const f = lookup(document, position);
    if (!f) return null;
    const def = f.r.def;
    // On the definition itself the hover would only repeat what is in view.
    if (position.line === def.line && f.hit.start === def.start) return null;

    const max = config().get('hover.maxCommentLines', 40);
    const d = sym.describe(f.lines, f.index, def, max);
    const md = new vscode.MarkdownString();
    let heading = `${KIND_LABEL[def.kind]} · line ${def.line + 1}`;
    if (f.index.duplicates.some(x => x.name === def.name)) heading += ' · **defined more than once**';
    md.appendMarkdown(heading);
    md.appendCodeblock(
      [...d.above, d.code, ...d.beside.map(b => '        ; ' + b)].join('\n'), 'd8x');
    return new vscode.Hover(md, f.origin);
  },
};

const documentSymbolProvider = {
  provideDocumentSymbols(document) {
    const { lines, index } = analyse(document);
    const defs = index.ordered;
    return defs.map((def, i) => {
      // A label owns the lines up to the next one, so breadcrumbs follow the
      // cursor through a routine; a .define is a single line.
      let last = def.line;
      if (def.kind !== sym.KIND.DEFINE) {
        last = (i + 1 < defs.length ? defs[i + 1].line : lines.length) - 1;
        if (last < def.line) last = def.line;
      }
      const range = new vscode.Range(def.line, 0, last, (lines[last] || '').length);
      return new vscode.DocumentSymbol(
        def.name, KIND_LABEL[def.kind], KIND_SYMBOL[def.kind], range, defRange(def));
    });
  },
};

function activate(context) {
  context.subscriptions.push(
    vscode.languages.registerDefinitionProvider(SELECTOR, definitionProvider),
    vscode.languages.registerReferenceProvider(SELECTOR, referenceProvider),
    vscode.languages.registerDocumentHighlightProvider(SELECTOR, highlightProvider),
    vscode.languages.registerHoverProvider(SELECTOR, hoverProvider),
    vscode.languages.registerDocumentSymbolProvider(SELECTOR, documentSymbolProvider, { label: 'D8X' }),
    vscode.workspace.onDidCloseTextDocument(doc => cache.delete(doc.uri.toString())),
  );
}

function deactivate() {
  cache.clear();
}

module.exports = { activate, deactivate };
