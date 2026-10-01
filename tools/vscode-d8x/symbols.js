'use strict';
// Symbol indexing for D8X assembly - kept free of the 'vscode' module so it
// can be tested against the real ROM sources under plain node (see test.js).
//
// Scope is one file. Every .ASM in this repo is a complete ROM with no
// .include, and the same name in two ROMs (9651 vs 9661, say) is two
// different things, so nothing here looks across files.
//
// The syntax mirrors roms/d8x_assembler/asm_d8x.py: a label is whatever sits
// at column 0, with or without a colon; '.equ' binds a label to a value;
// '.define NAME' and 'NAME .macro' introduce macros. Labels are
// case-sensitive, mnemonics and registers are not.

// A symbol, as a word: an optional leading '.' (the .locallabelchar labels in
// the DIAG16 sources) then an identifier. No label in the repo has an
// internal '.', so a trailing '.' is sentence punctuation, not part of it.
const WORD_RE = /\.?[A-Za-z_][A-Za-z0-9_$]*/;
const LABEL_RE = /^([.A-Za-z0-9_$]+)(:?)/;
const DATA_RE = /^[ \t]+\.(?:block|db|dw)\b/i;
const EQU_RE = /^[ \t]+\.equ\b/i;
const MACRO_RE = /^[ \t]+\.macro\b/i;
const DEFINE_RE = /^[ \t]*\.define[ \t]+(\.?[A-Za-z_][A-Za-z0-9_]*)/i;

// IDA appends an access-type letter straight onto the name in its xref
// comments ('map_2d_interpolatep', 'sub_C59B+1B5w'), after an up/down arrow
// in the UTF-8 copies or CP437 0x18/0x19 in the raw ones.
const XREF_SUFFIX_RE = /^(.+?)[↑↓\x18\x19]?[rwopj]$/;
// One continuation entry of an xref list: a single token, optionally '...'.
const XREF_ENTRY_RE = /^\S+[rwopj](?:\s+\.\.\.)?$|^\.\.\.$/;

const KIND = { CODE: 'code', DATA: 'data', EQU: 'equ', MACRO: 'macro', DEFINE: 'define' };

// Offset of the ';' that starts the comment, or -1. A ';' inside a quoted
// string ('.locallabelchar ";"' is legal) does not count.
function commentStart(text) {
  let quote = null;
  for (let i = 0; i < text.length; i++) {
    const c = text[i];
    if (quote) {
      if (c === quote) quote = null;
    } else if (c === '"' || c === '\'') {
      quote = c;
    } else if (c === ';') {
      return i;
    }
  }
  return -1;
}

// Classifies every line's comment as prose or as part of an IDA xref list,
// returning per line the offset from which comment text is xref, or -1.
// A list starts at 'CODE XREF:'/'DATA XREF:' and continues over following
// comment-only lines that each hold one xref entry.
function xrefStarts(lines) {
  const out = new Array(lines.length).fill(-1);
  let inList = false;
  for (let i = 0; i < lines.length; i++) {
    const text = lines[i];
    const c = commentStart(text);
    if (c < 0) { inList = false; continue; }
    const m = /\b(?:CODE|DATA) XREF:/.exec(text.slice(c));
    if (m) {
      out[i] = c + m.index;
      inList = true;
      continue;
    }
    const commentOnly = text.slice(0, c).trim() === '';
    if (inList && commentOnly && XREF_ENTRY_RE.test(text.slice(c + 1).trim())) {
      out[i] = c;
    } else {
      inList = false;
    }
  }
  return out;
}

function buildIndex(lines) {
  const defs = new Map();      // name -> first definition
  const ordered = [];          // every definition, in file order
  const duplicates = [];       // redefinitions, which the assembler rejects
  for (let i = 0; i < lines.length; i++) {
    const text = lines[i];
    const c = commentStart(text);
    const code = c < 0 ? text : text.slice(0, c);
    let def = null;

    if (code.length && code[0] !== ' ' && code[0] !== '\t') {
      const m = LABEL_RE.exec(code);
      if (m) {
        const rest = code.slice(m[0].length);
        const kind = EQU_RE.test(rest) ? KIND.EQU
          : MACRO_RE.test(rest) ? KIND.MACRO
          : DATA_RE.test(rest) ? KIND.DATA
          : KIND.CODE;
        def = { name: m[1], line: i, start: 0, end: m[1].length, kind };
      }
    } else {
      const m = DEFINE_RE.exec(code);
      if (m) {
        const start = m.index + m[0].length - m[1].length;
        def = { name: m[1], line: i, start, end: start + m[1].length, kind: KIND.DEFINE };
      }
    }

    if (def) {
      ordered.push(def);
      if (defs.has(def.name)) duplicates.push(def);
      else defs.set(def.name, def);
    }
  }
  return { defs, ordered, duplicates, xref: xrefStarts(lines) };
}

// The word under the cursor and whether it sits in a comment.
function wordAt(text, character) {
  const re = new RegExp(WORD_RE.source, 'g');
  let m;
  while ((m = re.exec(text)) !== null) {
    if (m.index > character) break;
    if (character <= m.index + m[0].length) {
      // 'x.foo' is not a reference to '.foo': a word may only start with '.'
      // when nothing symbol-like precedes it.
      let start = m.index;
      let word = m[0];
      if (word[0] === '.' && start > 0 && /[A-Za-z0-9_$]/.test(text[start - 1])) {
        start += 1;
        word = word.slice(1);
      }
      const c = commentStart(text);
      return { word, start, end: start + word.length, inComment: c >= 0 && start > c };
    }
  }
  return null;
}

// Resolves a word to a definition, allowing for how IDA writes names in its
// xref comments. Returns { def, name, length } where length is how much of
// the word is the name, or null.
function resolve(index, hit) {
  if (!hit) return null;
  const direct = index.defs.get(hit.word);
  if (direct) return { def: direct, name: hit.word, length: hit.word.length };
  if (hit.inComment) {
    const m = XREF_SUFFIX_RE.exec(hit.word);
    if (m && index.defs.has(m[1])) {
      return { def: index.defs.get(m[1]), name: m[1], length: m[1].length };
    }
  }
  return null;
}

function escapeRe(s) {
  return s.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
}

// Every occurrence of a name. Occurrences in an IDA xref list are left out:
// each one only restates, from the target's side, a reference that is
// already found in code.
function findOccurrences(lines, index, name, { includeComments = true } = {}) {
  const re = new RegExp('(?<![.A-Za-z0-9_$])' + escapeRe(name) + '(?![A-Za-z0-9_$])', 'g');
  const out = [];
  for (let i = 0; i < lines.length; i++) {
    const text = lines[i];
    if (text.indexOf(name) < 0) continue;
    const c = commentStart(text);
    const xref = index.xref[i];
    re.lastIndex = 0;
    let m;
    while ((m = re.exec(text)) !== null) {
      const inComment = c >= 0 && m.index > c;
      if (inComment && (!includeComments || (xref >= 0 && m.index >= xref))) continue;
      out.push({ line: i, start: m.index, end: m.index + name.length, inComment });
    }
  }
  return out;
}

// IDA decoration that carries no information in a hover.
function isNoiseComment(body) {
  return body === ''
    || /^[-=─-╿▀-▟_\s]+$/.test(body)       // rules and banners
    || /S U B\s*R O U T\s*I N E/.test(body)
    || /^Segment type:/.test(body)
    || /�/.test(body) && !/[A-Za-z]{3}/.test(body);        // mis-decoded CP437 banner
}

// The source behind a hover: the comment block above the definition, the
// definition line itself, and the comments beside and below it - with xref
// lists and IDA banners dropped.
function describe(lines, index, def, maxCommentLines = 40) {
  const above = [];
  for (let j = def.line - 1; j >= 0 && above.length < maxCommentLines; j--) {
    const t = lines[j];
    const c = commentStart(t);
    if (t.trim() === '') continue;
    // Only a column-0 comment is a header. An indented comment-only line is
    // IDA continuing the comment of the line above it, i.e. something else's.
    if (c !== 0) break;
    const body = t.slice(c + 1).trim();
    if (/^End of function\b/.test(body)) break;                 // previous routine
    if (index.xref[j] >= 0) continue;                           // a neighbour's xref list
    // Blank comment lines are kept - they separate paragraphs - but not
    // the rules and banners.
    if (body !== '' && isNoiseComment(body)) continue;
    above.unshift(t.trimEnd());
  }
  const blank = s => s.slice(1).trim() === '';
  while (above.length && blank(above[0])) above.shift();
  while (above.length && blank(above[above.length - 1])) above.pop();
  // Collapse the runs left behind where a banner was dropped.
  for (let k = above.length - 1; k > 0; k--) {
    if (blank(above[k]) && blank(above[k - 1])) above.splice(k, 1);
  }

  const text = lines[def.line];
  const c = commentStart(text);
  const code = (c < 0 ? text : text.slice(0, c)).trim().replace(/[ \t]+/g, ' ');
  const beside = [];
  const pushComment = (lineNo, offset) => {
    const xref = index.xref[lineNo];
    if (xref >= 0 && xref <= offset) return;
    const body = lines[lineNo].slice(offset + 1, xref >= 0 ? xref : undefined)
      .replace(/\t/g, ' ').trim();
    if (body && !isNoiseComment(body)) beside.push(body);
  };
  if (c >= 0) pushComment(def.line, c);
  for (let j = def.line + 1; j < lines.length && beside.length < maxCommentLines; j++) {
    const t = lines[j];
    const cj = commentStart(t);
    // Continuations are indented; a column-0 comment heads what follows.
    if (cj <= 0 || t.slice(0, cj).trim() !== '') break;
    pushComment(j, cj);
  }
  return { above, code, beside };
}

module.exports = {
  KIND, WORD_RE, commentStart, buildIndex, wordAt, resolve, findOccurrences, describe,
};
