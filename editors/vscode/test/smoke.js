#!/usr/bin/env node
'use strict';

/* Smoke checks for the Pentest VSCode extension.
 *
 * Node-only: no network, no dependencies beyond the repo itself.
 *
 *   node editors/vscode/test/smoke.js     (from the repo root)
 *   node test/smoke.js                    (from editors/vscode)
 *   npm test                              (from editors/vscode, same thing)
 *
 * What it checks:
 *   - the TextMate grammar: repository shape, keyword/operator/literal rules,
 *     and a tiny line tokenizer that exercises the real regexes (so `b"x"`
 *     matching, slice colons, etc. are tested behaviorally, not just by
 *     eyeballing the JSON);
 *   - snippets: every declared snippet has a prefix/body/description and the
 *     expected first body line;
 *   - the completion provider: the module is loaded with a stubbed `vscode`
 *     API, `activate()` is run, and the registered provider is invoked with a
 *     fake document — keywords, builtins, reserved-name suppression and (2B)
 *     method completions after `.` are all checked on real returned items;
 *   - the 0.2.1 surface: `variable.` cross-scope highlighting, qualifier
 *     completions (global/parent/sibling functions, never methods), the
 *     `variable` contextual completion + snippet, and the Run button's
 *     "run the file you are on" resolution (clicked resource → active
 *     editor → save-before-run → closed-terminal recovery);
 *   - package.json + language-configuration.json contributions.
 *
 * Exit code 0 = all green, 1 = at least one failure.
 */

const fs = require('fs');
const path = require('path');
const Module = require('module');

const EXT_ROOT = path.join(__dirname, '..');
const readJson = (p) => JSON.parse(fs.readFileSync(path.join(EXT_ROOT, p), 'utf8'));
const readText = (p) => fs.readFileSync(path.join(EXT_ROOT, p), 'utf8');
const fileExists = (p) => fs.existsSync(path.join(EXT_ROOT, p));

const grammar = readJson('syntaxes/pentest.tmLanguage.json');
const snippets = readJson('snippets/pentest.json');
const pkg = readJson('package.json');
const langCfg = readJson('language-configuration.json');
const extSrc = readText('src/extension.js');
const readme = readText('README.md');

// ---------------------------------------------------------------------------
// Check bookkeeping
// ---------------------------------------------------------------------------

const sections = [];
let total = 0;
let failed = 0;
const failures = [];

function section(name) {
  sections.push({ name, at: total });
}

function check(desc, ok) {
  total += 1;
  if (!ok) {
    failed += 1;
    failures.push(desc);
  }
}

// ---------------------------------------------------------------------------
// Mini TextMate tokenizer (line-oriented, enough for smoke purposes)
//
// Semantics mirror TextMate: at each position the rule with the LOWEST match
// index wins; ties go to the rule that appears FIRST in the pattern list.
// begin/end regions consume the whole region (unterminated regions run to the
// end of the line) and their inner patterns are scanned recursively.
// ---------------------------------------------------------------------------

const reCache = new Map();
function rx(src, extra) {
  const key = src + '|' + (extra || 'g');
  let r = reCache.get(key);
  if (!r) {
    // `d` (hasIndices) gives exact capture-group offsets; `g` for lastIndex.
    r = new RegExp(src, extra ? extra + 'gd' : 'gd');
    reCache.set(key, r);
  }
  return r;
}

function flatten(patterns, depth) {
  const out = [];
  if (depth > 32) return out;
  for (const p of patterns) {
    if (!p) continue;
    if (p.include) {
      if (p.include === '$self' || p.include === '$base') {
        out.push(...flatten(grammar.patterns, depth + 1));
      } else if (p.include[0] === '#') {
        const r = grammar.repository[p.include.slice(1)];
        if (!r) continue;
        if (r.patterns && !r.match && !r.begin) out.push(...flatten(r.patterns, depth + 1));
        else out.push(r);
      }
      continue;
    }
    if (p.match || p.begin) out.push(p);
    else if (p.patterns) out.push(...flatten(p.patterns, depth + 1));
  }
  return out;
}

let ROOT_RULES = null;
function rootRules() {
  if (!ROOT_RULES) ROOT_RULES = flatten(grammar.patterns, 0);
  return ROOT_RULES;
}

function pushTok(out, s, e, scopes, text) {
  if (e <= s) return;
  out.push({ s, e, t: text.slice(s, e), scopes });
}

function emitCaptures(m, captures, baseScopes, text, out) {
  if (!captures || !m.indices) return;
  for (const key of Object.keys(captures)) {
    const cap = captures[key];
    if (!cap || !cap.name) continue;
    const idx = Number(key);
    const span = m.indices[idx];
    if (!span) continue;
    pushTok(out, span[0], span[1], baseScopes.concat([cap.name]), text);
  }
}

function scan(text, rules, from, to, inherited, out, depth) {
  if (depth > 32 || from >= to) return;
  let pos = from;
  let guard = 0;
  while (pos < to && guard++ < 5000) {
    let bestIdx = Infinity;
    let bestRule = null;
    let bestM = null;
    for (const r of rules) {
      const re = rx(r.match || r.begin);
      re.lastIndex = pos;
      const m = re.exec(text);
      if (!m) continue;
      if (m.index >= to) continue;
      if (m.index + m[0].length > to) continue; // would overrun the region
      if (m.index < bestIdx) {
        bestIdx = m.index;
        bestRule = r;
        bestM = m;
      }
    }
    if (!bestRule) break;

    const m = bestM;
    if (bestRule.begin) {
      const bEnd = m.index + m[0].length;
      const er = rx(bestRule.end);
      er.lastIndex = bEnd;
      const em = er.exec(text);
      const closed = !!em && em.index <= to;
      const regionEnd = closed ? em.index + em[0].length : to;
      const innerEnd = closed ? em.index : to;
      const scopes = inherited.concat(bestRule.name ? [bestRule.name] : []);
      if (bestRule.name) pushTok(out, m.index, regionEnd, scopes, text);
      emitCaptures(m, bestRule.beginCaptures, scopes, text, out);
      if (closed) emitCaptures(em, bestRule.endCaptures, scopes, text, out);
      if (bestRule.patterns) {
        scan(text, flatten(bestRule.patterns, 0), bEnd, innerEnd, scopes, out, depth + 1);
      }
      pos = closed && em[0].length === 0 ? bEnd : regionEnd;
    } else {
      const scopes = inherited.concat(bestRule.name ? [bestRule.name] : []);
      if (bestRule.name) pushTok(out, m.index, m.index + m[0].length, scopes, text);
      emitCaptures(m, bestRule.captures, scopes, text, out);
      pos = m[0].length === 0 ? m.index + 1 : m.index + m[0].length;
    }
  }
}

function tokenize(line) {
  const out = [];
  scan(line, rootRules(), 0, line.length, [], out, 0);
  return out;
}

function hasScope(tok, scope) {
  return !!tok && tok.scopes.indexOf(scope) !== -1;
}

// All tokens with exact text.
function tokensWithText(line, str) {
  return tokenize(line).filter((t) => t.t === str);
}

function anyScope(toks, scope) {
  return toks.some((t) => hasScope(t, scope));
}

// Debug aid: `node test/smoke.js --tokens 'b"x"' 'xs[1:2]'` prints the
// tokenizer's tokens + scopes for eyeballing the grammar.
if (process.argv[2] === '--tokens') {
  for (const line of process.argv.slice(3)) {
    process.stdout.write(line + '\n');
    for (const t of tokenize(line)) {
      process.stdout.write(
        `  [${t.s},${t.e}) ${JSON.stringify(t.t)}  ${t.scopes.join(' ')}\n`
      );
    }
  }
  process.exit(0);
}

// ---------------------------------------------------------------------------
// Completion-provider harness: load extension.js with a stubbed `vscode`
// ---------------------------------------------------------------------------

const captured = {
  provider: null,
  selector: null,
  triggers: null,
  hoverProvider: null,
  commands: {},
  terminals: [],
};
// Resolved when the (async) dirty-run scenario finishes; the report waits
// for it when set — see the bottom of this file.
let late = null;

const fakeVscode = {
  window: {
    createOutputChannel: () => ({ appendLine() {} }),
    showWarningMessage() {},
    showInformationMessage() {},
    showErrorMessage() {},
    activeTextEditor: undefined,
    createTerminal: (name) => {
      const t = {
        name,
        shown: 0,
        sent: [],
        show() { t.shown += 1; },
        sendText(text) { t.sent.push(text); },
        dispose() {},
      };
      captured.terminals.push(t);
      return t;
    },
    onDidCloseTerminal: () => ({ dispose() {} }),
  },
  languages: {
    createDiagnosticCollection: () => ({ set() {}, delete() {} }),
    registerCompletionItemProvider: (selector, provider, ...rest) => {
      captured.provider = provider;
      captured.selector = selector;
      captured.triggers = rest;
      return { dispose() {} };
    },
    registerHoverProvider: (selector, provider) => {
      captured.hoverProvider = provider;
      return { dispose() {} };
    },
  },
  commands: {
    registerCommand: (id, fn) => {
      captured.commands[id] = fn;
      return { dispose() {} };
    },
  },
  workspace: {
    textDocuments: [],
    onDidChangeTextDocument: () => ({ dispose() {} }),
    onDidOpenTextDocument: () => ({ dispose() {} }),
    onDidSaveTextDocument: () => ({ dispose() {} }),
    onDidCloseTextDocument: () => ({ dispose() {} }),
    getConfiguration: () => ({ get: (k, d) => d }),
  },
  CompletionItem: class {
    constructor(label, kind) {
      this.label = label;
      this.kind = kind;
    }
  },
  CompletionItemKind: {
    Method: 1, Function: 2, Variable: 5, Keyword: 13,
  },
  MarkdownString: class {
    constructor(value) { this.value = value === undefined ? '' : String(value); }
    appendMarkdown(s) { this.value += String(s); return this; }
    appendCodeblock(code, lang) {
      this.value += '```' + (lang || '') + '\n' + String(code) + '\n```\n';
      return this;
    }
  },
  Hover: class {
    constructor(contents, range) {
      this.contents = contents;
      this.range = range;
    }
  },
  SnippetString: class {
    constructor(value) { this.value = String(value); }
  },
  Range: class {
    constructor(a, b, c, d) {
      if (typeof a === 'object') { this.start = a; this.end = b; }
      else { this.start = { line: a, character: b }; this.end = { line: c, character: d }; }
    }
  },
  DiagnosticSeverity: { Error: 0 },
};

const origLoad = Module._load;
Module._load = function (request, parent, isMain) {
  if (request === 'vscode') return fakeVscode;
  return origLoad.call(this, request, parent, isMain);
};

const ext = require(path.join(EXT_ROOT, 'src', 'extension.js'));
Module._load = origLoad;

ext.activate({ subscriptions: [] });

function makeDoc(text) {
  const lines = text.split('\n');
  const isWord = (c) => !!c && /[A-Za-z0-9_]/.test(c);
  return {
    languageId: 'pentest',
    isClosed: false,
    version: 1,
    lineCount: lines.length,
    // The method-completion branch resolves imported modules relative to
    // the document's folder (document.uri.fsPath) — the fake doc needs one.
    uri: {
      fsPath: path.join(EXT_ROOT, 'smoke-fake.pen'),
      toString: () => 'file://' + path.join(EXT_ROOT, 'smoke-fake.pen'),
    },
    getText: (range) => {
      if (!range) return text;  // no range: the whole document (userDecls)
      const ln = lines[range.start.line] !== undefined ? lines[range.start.line] : '';
      return ln.slice(range.start.character, range.end.character);
    },
    lineAt: (i) => ({ text: lines[i] !== undefined ? lines[i] : '' }),
    getWordRangeAtPosition: (pos) => {
      const line = lines[pos.line] !== undefined ? lines[pos.line] : '';
      let s = pos.character;
      let e = pos.character;
      while (s > 0 && isWord(line[s - 1])) s--;
      while (e < line.length && isWord(line[e])) e++;
      if (s === e) return undefined;
      return { start: { line: pos.line, character: s }, end: { line: pos.line, character: e } };
    },
  };
}

/** Run the registered completion provider; cursor sits at `cursorIndex`. */
function completions(text, cursorIndex) {
  const before = text.slice(0, cursorIndex);
  const line = (before.match(/\n/g) || []).length;
  const character = cursorIndex - (before.lastIndexOf('\n') + 1);
  const position = { line, character };
  return captured.provider.provideCompletionItems
    ? captured.provider.provideCompletionItems(makeDoc(text), position)
    : captured.provider(makeDoc(text), position);
}

/** Run the registered hover provider; cursor sits at `cursorIndex`. */
function hover(text, cursorIndex) {
  const before = text.slice(0, cursorIndex);
  const line = (before.match(/\n/g) || []).length;
  const character = cursorIndex - (before.lastIndexOf('\n') + 1);
  const position = { line, character };
  return captured.hoverProvider
    ? captured.hoverProvider.provideHover(makeDoc(text), position)
    : undefined;
}
const hoverText = (h) => (h && h.contents && h.contents.value) || '';

const labelOf = (item) => (typeof item.label === 'string' ? item.label : item.label.label);
const findItem = (items, label) => items.find((i) => labelOf(i) === label);
const docText = (item) => (item.documentation ? item.documentation.value : '');
const snippetText = (item) => (item.insertText && item.insertText.value ? item.insertText.value : '');

// ===========================================================================
// Sections
// ===========================================================================

// ---------------------------------------------------------------------------
section('grammar structure');

check('grammar has scopeName source.pentest', grammar.scopeName === 'source.pentest');
check('grammar name is Pentest', grammar.name === 'Pentest');
const ROOT_INCLUDES = [
  'comments', 'strings', 'numbers', 'function-definition', 'main-block',
  'assignment', 'keywords', 'keywords-control', 'keywords-other',
  'constants', 'builtin', 'function-call', 'operators', 'punctuation',
];
const rootIncludeNames = grammar.patterns.map((p) => p.include).filter(Boolean);
for (const inc of ROOT_INCLUDES) {
  check(`root patterns include #${inc}`, rootIncludeNames.indexOf('#' + inc) !== -1);
  check(`repository has #${inc}`, !!grammar.repository[inc]);
}
check('every root include resolves', rootIncludeNames.every((n) => !!grammar.repository[n.slice(1)]));

// ---------------------------------------------------------------------------
section('keywords (M1 + 2A)');

function ruleMatches(name, word) {
  const r = new RegExp(grammar.repository[name].match);
  r.lastIndex = 0;
  return r.test(word);
}

const M1_KEYWORDS = ['main', 'return'];
const A2_KEYWORDS = [
  'if', 'elif', 'else', 'for', 'while', 'break', 'continue',
  'switch', 'case', 'pass', 'and', 'or', 'not', 'in',
];
const CONSTANTS = ['True', 'False', 'None'];

for (const w of M1_KEYWORDS) {
  check(`keywords rule matches ${w}`, ruleMatches('keywords', w));
  check(`${w} tokenizes as keyword.control`, anyScope(tokenize(w), 'keyword.control.pentest'));
}
for (const w of A2_KEYWORDS) {
  const inControl = ruleMatches('keywords-control', w);
  const inOther = ruleMatches('keywords-other', w);
  check(`2A keyword ${w} present in a keyword rule`, inControl || inOther);
  const toks = tokenize(w);
  check(
    `${w} tokenizes as keyword.*`,
    anyScope(toks, 'keyword.control.pentest') || anyScope(toks, 'keyword.other.pentest')
  );
}
for (const w of CONSTANTS) {
  check(`constants rule matches ${w}`, ruleMatches('constants', w));
  check(`${w} tokenizes as constant.language`, anyScope(tokenize(w), 'constant.language.pentest'));
}
check('keywords scope is keyword.control.pentest',
  grammar.repository.keywords.name === 'keyword.control.pentest');
check('keywords-control scope is keyword.control.pentest',
  grammar.repository['keywords-control'].name === 'keyword.control.pentest');
check('keywords-other scope is keyword.other.pentest',
  grammar.repository['keywords-other'].name === 'keyword.other.pentest');
check('word boundary: iffy not a keyword', !ruleMatches('keywords-control', 'iffy'));
check('word boundary: returning not a keyword', !ruleMatches('keywords', 'returning'));

// ---------------------------------------------------------------------------
section('operators (2A surface)');

const OPERATOR_SRC = grammar.repository.operators.match;
check('operators scope is keyword.operator.pentest',
  grammar.repository.operators.name === 'keyword.operator.pentest');
const OPERATORS = [
  '==', '!=', '<=', '>=', '**', '<<', '>>',
  '+=', '-=', '*=', '/=', '%=', '&=', '|=', '^=', '**=', '<<=', '>>=',
  '+', '-', '*', '/', '%', '<', '>', '=', '&', '|', '^', '~',
];
for (const op of OPERATORS) {
  check(`operators rule matches ${op}`, new RegExp('^(?:' + OPERATOR_SRC + ')$').test(op));
}
for (const op of ['+', '-=', '**=']) {
  const toks = tokenize('a ' + op + ' b');
  const opTok = toks.find((t) => t.t === op);
  check(`tokenize: ${op} gets operator scope`, hasScope(opTok, 'keyword.operator.pentest'));
}
check('operators rule does not match -> (retired)', !new RegExp('^(?:' + OPERATOR_SRC + ')$').test('->'));
check('retired-arrow rule exists', !!grammar.repository['retired-arrow']);
check('retired-arrow scope is invalid.illegal',
  grammar.repository['retired-arrow'].name === 'invalid.illegal.retired.pentest');
{
  const toks = tokenize('a -> b');
  const arrow = toks.find((t) => t.t === '->');
  check('tokenize: -> gets invalid.illegal scope',
    !!arrow && hasScope(arrow, 'invalid.illegal.retired.pentest'));
}

// ---------------------------------------------------------------------------
section('literals, comments, calls');

check('numbers rule matches int', ruleMatches('numbers', '42'));
check('numbers rule matches float', ruleMatches('numbers', '3.14'));
check('numbers rule scope', grammar.repository.numbers.name === 'constant.numeric.pentest');
check('builtin rule matches print()', ruleMatches('builtin', 'print()'));
check('builtin rule matches range()', ruleMatches('builtin', 'range()'));
check('builtin rule: bare print not builtin', !ruleMatches('builtin', 'print'));
check('builtin rule: printx(not builtin)', !ruleMatches('builtin', 'printx()'));
check('builtin scope is support.function.pentest',
  grammar.repository.builtin.name === 'support.function.pentest');
check('function-call matches foo()', ruleMatches('function-call', 'foo()'));
check('function-call does not match bare foo', !ruleMatches('function-call', 'foo'));
check('function-call scope is variable.function.pentest',
  grammar.repository['function-call'].name === 'variable.function.pentest');

const fnDef = new RegExp(grammar.repository['function-definition'].match, 'd');
const fnM = fnDef.exec('add(a):');
check('function-definition matches declaration', !!fnM && fnM[2] === 'add');
const fnM2 = fnDef.exec('greet():');
check('function-definition matches parameterless declaration', !!fnM2 && fnM2[2] === 'greet');
check('function-definition rejects bare `greet:` (not a declaration)', !fnDef.exec('greet:'));
check('function-definition rejects a call statement `greet(x)`', !fnDef.exec('greet(x)'));
check('function-definition does not match `else:`', !fnDef.exec('else:'));
check('function-definition does not match `if x:`', !fnDef.exec('if x:'));
check('function-definition name capture scope',
  grammar.repository['function-definition'].captures['2'].name === 'entity.name.function.pentest');

const mainM = new RegExp(grammar.repository['main-block'].match, 'd').exec('main:');
check('main-block matches main:', !!mainM && mainM[2] === 'main');

check('string scope is string.quoted.double.pentest',
  grammar.repository.strings.name === 'string.quoted.double.pentest');
check('string escape rule present',
  (grammar.repository.strings.patterns || []).some((p) => p.name === 'constant.character.escape.pentest'));
const commentNames = grammar.repository.comments.patterns.map((p) => p.name);
check('# line comment scoped', commentNames.indexOf('comment.line.number-sign.pentest') !== -1);
check('// line comment scoped', commentNames.indexOf('comment.line.double-slash.pentest') !== -1);
check('/* */ block comment scoped', commentNames.indexOf('comment.block.pentest') !== -1);

const asg = new RegExp(grammar.repository.assignment.match, 'd');
let am = asg.exec('x = 1');
check('assignment matches `x = 1`', !!am);
check('assignment captures target x', am && am[2] === 'x');
check('assignment target scope is variable.other.readwrite.pentest',
  grammar.repository.assignment.captures['2'].name === 'variable.other.readwrite.pentest');
check('assignment does not match `x == 1`', !asg.test('x == 1'));
const AUGOPS = ['+', '-', '*', '/', '%', '**', '&', '|', '^', '<<', '>>'];
for (const op of AUGOPS) {
  check(`assignment matches \`x ${op}= 1\``, asg.test(`x ${op}= 1`));
}

// ---------------------------------------------------------------------------
section('grammar behavior (tokenizer)');

{
  const t = tokenize('x = 1');
  check('`x = 1`: x is assignment target', hasScope(t.find((k) => k.t === 'x'), 'variable.other.readwrite.pentest'));
  check('`x = 1`: = is operator', hasScope(t.find((k) => k.t === '='), 'keyword.operator.pentest'));
  check('`x = 1`: 1 is number', hasScope(t.find((k) => k.t === '1'), 'constant.numeric.pentest'));
}
{
  const t = tokenize('x += 1');
  check('`x += 1`: target scoped', hasScope(t.find((k) => k.t === 'x'), 'variable.other.readwrite.pentest'));
  check('`x += 1`: += operator scoped', hasScope(t.find((k) => k.t === '+='), 'keyword.operator.pentest'));
}
{
  const t = tokenize('if x > 0:');
  check('`if x > 0:`: if keyword', hasScope(t.find((k) => k.t === 'if'), 'keyword.control.pentest'));
  check('`if x > 0:`: colon punctuation', hasScope(t.find((k) => k.t === ':'), 'punctuation.pentest'));
  check('`if x > 0:`: > operator', hasScope(t.find((k) => k.t === '>'), 'keyword.operator.pentest'));
}
{
  const t = tokenize('add(a):');
  check('declaration: add entity.name.function', hasScope(t.find((k) => k.t === 'add'), 'entity.name.function.pentest'));
  check('declaration: open paren punctuation', hasScope(t.find((k) => k.t === '('), 'punctuation.pentest'));
  check('declaration: colon punctuation', hasScope(t.find((k) => k.t === ':'), 'punctuation.pentest'));
}
{
  const t = tokenize('main:');
  check('main: entity.name.function', hasScope(t.find((k) => k.t === 'main'), 'entity.name.function.pentest'));
}
{
  const t = tokenize('for i in range(3):');
  check('for header: for keyword', hasScope(t.find((k) => k.t === 'for'), 'keyword.control.pentest'));
  check('for header: in keyword', hasScope(t.find((k) => k.t === 'in'), 'keyword.other.pentest'));
  check('for header: call paren punctuation', hasScope(t.find((k) => k.t === '('), 'punctuation.pentest'));
  check('for header: range builtin', hasScope(t.find((k) => k.t === 'range'), 'support.function.pentest'));
  check('for header: colon punctuation', hasScope(t.find((k) => k.t === ':'), 'punctuation.pentest'));
}
{
  const t = tokenize('switch v:');
  check('switch header keyword', hasScope(t.find((k) => k.t === 'switch'), 'keyword.control.pentest'));
  const c = tokenize('case 1:');
  check('case label keyword', hasScope(c.find((k) => k.t === 'case'), 'keyword.control.pentest'));
  check('case label number', hasScope(c.find((k) => k.t === '1'), 'constant.numeric.pentest'));
  check('case colon punctuation', hasScope(c.find((k) => k.t === ':'), 'punctuation.pentest'));
}
{
  const t = tokenize('print("hi")');
  check('call: print builtin', hasScope(t.find((k) => k.t === 'print'), 'support.function.pentest'));
  check('call: string token', hasScope(t.find((k) => k.t === '"hi"'), 'string.quoted.double.pentest'));
  check('call: open paren punctuation', hasScope(t.find((k) => k.t === '('), 'punctuation.pentest'));
}
{
  const t = tokenize('s = "it\'s"');
  check('string with apostrophe stays one token',
    hasScope(t.find((k) => k.t === '"it\'s"'), 'string.quoted.double.pentest'));
}
{
  const t = tokenize('# comment if x');
  check('# comment token covers line', hasScope(t.find((k) => k.t === '# comment if x'), 'comment.line.number-sign.pentest'));
  check('# comment hides keyword if', !anyScope(t.filter((k) => k.t === 'if'), 'keyword.control.pentest'));
}
{
  const t = tokenize('// note while y');
  check('// comment token covers line', hasScope(t.find((k) => k.t === '// note while y'), 'comment.line.double-slash.pentest'));
}
{
  const t = tokenize('x = True');
  check('True constant scoped', hasScope(t.find((k) => k.t === 'True'), 'constant.language.pentest'));
}
{
  const t = tokenize('a and b');
  check('and keyword.other scoped', hasScope(t.find((k) => k.t === 'and'), 'keyword.other.pentest'));
}
{
  const t = tokenize('return x');
  check('return keyword scoped', hasScope(t.find((k) => k.t === 'return'), 'keyword.control.pentest'));
}
{
  const t = tokenize('if a: print(a)');
  check('single-line body: if keyword', hasScope(t.find((k) => k.t === 'if'), 'keyword.control.pentest'));
  check('single-line body: colon punctuation', hasScope(t.find((k) => k.t === ':'), 'punctuation.pentest'));
  check('single-line body: print builtin', hasScope(t.find((k) => k.t === 'print'), 'support.function.pentest'));
}
{
  const t = tokenize('pass');
  check('pass keyword.other scoped', hasScope(t.find((k) => k.t === 'pass'), 'keyword.other.pentest'));
}
{
  const t = tokenize('x = 3.14');
  check('float is a single number token', hasScope(t.find((k) => k.t === '3.14'), 'constant.numeric.pentest'));
  check('float: no stray dot token', tokensWithText('x = 3.14', '.').length === 0);
}

// ---------------------------------------------------------------------------
section('snippets (M1 + 2A)');

const SNIPPET_FIRST_LINE = {
  'function': '${1:name}(${2:args}):',
  'main': 'main:',
  'print': 'print(${1:value})$0',
  'return': 'return ${1:value}$0',
  'if': 'if ${1:condition}:',
  'if-elif-else': 'if ${1:condition}:',
  'if-membership': 'if ${1:item} in ${2:haystack}:',
  'while': 'while ${1:condition}:',
  'for-range': 'for ${1:i} in range(${2:stop}):',
  'for-collection': 'for ${1:item} in ${2:collection}:',
  'switch': 'switch ${1:value}:',
  'pass': 'pass$0',
};
for (const [key, firstLine] of Object.entries(SNIPPET_FIRST_LINE)) {
  const sn = snippets[key];
  check(`snippet ${key} exists`, !!sn);
  if (!sn) continue;
  check(`snippet ${key} has prefix`, typeof sn.prefix === 'string' && sn.prefix.length > 0);
  check(`snippet ${key} has non-empty body`, Array.isArray(sn.body) && sn.body.length > 0
    && sn.body.every((l) => typeof l === 'string'));
  check(`snippet ${key} has description`, typeof sn.description === 'string' && sn.description.length > 0);
  check(`snippet ${key} first body line`, Array.isArray(sn.body) && sn.body[0] === firstLine);
}
check('package contributes snippets for pentest',
  Array.isArray(pkg.contributes.snippets) && pkg.contributes.snippets.length === 1
  && pkg.contributes.snippets[0].language === 'pentest');

// ---------------------------------------------------------------------------
section('completion provider (M1 + 2A)');

check('completion provider registered on activation',
  typeof captured.provider === 'function'
  || (captured.provider && typeof captured.provider.provideCompletionItems === 'function'));
check('provider selector targets pentest language',
  !!captured.selector && captured.selector.language === 'pentest');

{
  const items = completions('x = 1', 5);
  const A2_DOC_KEYWORDS = [
    'main', 'return', 'True', 'False', 'None',
    'if', 'elif', 'else', 'for', 'while', 'break', 'continue',
    'switch', 'case', 'pass', 'and', 'or', 'not', 'in', '->',
  ];
  for (const kw of A2_DOC_KEYWORDS) {
    const it = findItem(items, kw);
    check(`completion: keyword ${kw} offered`, !!it);
    if (it) {
      check(`completion: ${kw} has Keyword kind`, it.kind === fakeVscode.CompletionItemKind.Keyword);
      check(`completion: ${kw} has documentation`, docText(it).length > 0);
    }
  }
  const arrow = findItem(items, '->');
  check('completion: -> offered with a retirement note', !!arrow);
  if (arrow) {
    const d = docText(arrow).toLowerCase();
    check('completion: -> documents the retirement',
      d.indexOf('not an operator') !== -1 && d.indexOf('f(args)') !== -1
      && d.indexOf('name():') !== -1);
  }
  const p = findItem(items, 'print');
  check('completion: print offered as Function', !!p && p.kind === fakeVscode.CompletionItemKind.Function);
  check('completion: print snippet insert', !!p && snippetText(p) === 'print(${1:value})');
  check('completion: print detail is builtin function', !!p && p.detail === 'builtin function');
  const r = findItem(items, 'range');
  check('completion: range offered as Function', !!r && r.kind === fakeVscode.CompletionItemKind.Function);
  check('completion: range snippet insert', !!r && snippetText(r) === 'range(${1:stop})');
  check('completion: range docs mention exclusive stop', !!r && docText(r).indexOf('exclusive') !== -1);
}
{
  // Reserved words in the document must not appear as plain variables.
  const items = completions('if = 1\nprint = 2\nrange = 3\npass = 4', 35);
  for (const w of ['if', 'print', 'range', 'pass']) {
    const asVar = items.find((i) => labelOf(i) === w && i.kind === fakeVscode.CompletionItemKind.Variable);
    check(`completion: reserved ${w} not offered as variable`, !asVar);
  }
}
{
  const src = 'foo(a):\n    return a\nbar = foo\n';
  const items = completions(src, src.length);
  check('completion: document function foo offered', !!findItem(items, 'foo'));
  const foo = findItem(items, 'foo');
  check('completion: foo offered as function (declared)', !!foo && foo.kind === fakeVscode.CompletionItemKind.Function);
  check('completion: document variable bar offered', !!findItem(items, 'bar'));
  check('completion: unknown word baz not required', items.length > 0);
}

// ---------------------------------------------------------------------------
section('package.json contributions');

check('name unchanged (pentest-lang)', pkg.name === 'pentest-lang');
check('displayName unchanged', pkg.displayName === 'Pentest Language');
check('publisher unchanged', pkg.publisher === 'pentest-lang');
check('main points at src/extension.js', pkg.main === './src/extension.js');
check('main file exists', fileExists('src/extension.js'));
check('activation event onLanguage:pentest',
  Array.isArray(pkg.activationEvents) && pkg.activationEvents.indexOf('onLanguage:pentest') !== -1);
const lang = pkg.contributes.languages && pkg.contributes.languages[0];
check('language id is pentest', !!lang && lang.id === 'pentest');
check('language has .pen', !!lang && lang.extensions.indexOf('.pen') !== -1);
check('language has .pent', !!lang && lang.extensions.indexOf('.pent') !== -1);
check('language configuration path', !!lang && fileExists(lang.configuration.replace('./', '')));
const gr = pkg.contributes.grammars && pkg.contributes.grammars[0];
check('grammar contribution scope source.pentest', !!gr && gr.scopeName === 'source.pentest');
check('grammar contribution path exists', !!gr && fileExists(gr.path.replace('./', '')));
const sn = pkg.contributes.snippets && pkg.contributes.snippets[0];
check('snippet contribution path exists', !!sn && fileExists(sn.path.replace('./', '')));
check('tabSize default 4',
  pkg.contributes.configurationDefaults
  && pkg.contributes.configurationDefaults['[pentest]']['editor.tabSize'] === 4);
check('package keywords mention pentest',
  Array.isArray(pkg.keywords) && pkg.keywords.indexOf('pentest') !== -1);

// ---------------------------------------------------------------------------
section('language-configuration.json');

check('line comment is #', langCfg.comments.lineComment === '#');
check('block comment /* */',
  Array.isArray(langCfg.comments.blockComment)
  && langCfg.comments.blockComment.join('') === '/**/');
for (const pair of [['(', ')'], ['[', ']'], ['{', '}']]) {
  check(`brackets ${pair.join('')}`, langCfg.brackets.some((p) => p[0] === pair[0] && p[1] === pair[1]));
}
for (const open of ['(', '[', '{', '"']) {
  check(`autoClosingPairs ${open}`, langCfg.autoClosingPairs.some((p) => (typeof p === 'string' ? p === open : p.open === open)));
}
const inc = new RegExp(langCfg.indentationRules.increaseIndentPattern);
check('increaseIndent: if x:', inc.test('if x:'));
check('increaseIndent: elif x:', inc.test('elif x:'));
check('increaseIndent: else:', inc.test('else:'));
check('increaseIndent: while x:', inc.test('while x:'));
check('increaseIndent: for header', inc.test('for i in range(3):'));
check('increaseIndent: switch v:', inc.test('switch v:'));
check('increaseIndent: case 1:', inc.test('case 1:'));
check('increaseIndent: f(a):', inc.test('f(a):'));
check('increaseIndent: greet():', inc.test('greet():'));
check('increaseIndent: bare greet: is not a declaration', !inc.test('greet:'));
check('increaseIndent: main:', inc.test('main:'));
check('increaseIndent: plain stmt not indented', !inc.test('x = 1'));
check('increaseIndent: call stmt not indented', !inc.test('foo(bar)'));
check('increaseIndent: commented line not indented', !inc.test('# if x:'));
const dec = new RegExp(langCfg.indentationRules.decreaseIndentPattern);
check('decreaseIndent: else:', dec.test('else:'));
check('decreaseIndent: elif:', dec.test('elif:'));
check('decreaseIndent: case 1:', dec.test('case 1:'));
check('decreaseIndent: if x: not dedented', !dec.test('if x:'));
check('wordPattern defined', typeof langCfg.wordPattern === 'string' && langCfg.wordPattern.length > 0);

// ---------------------------------------------------------------------------
section('README');

check('README documents syntax coloring', readme.indexOf('Syntax coloring') !== -1);
check('README documents autocomplete', readme.indexOf('Autocomplete') !== -1);
check('README documents snippets', readme.indexOf('Snippets') !== -1);
check('README documents live diagnostics', readme.indexOf('Live error squiggles') !== -1);

// ===========================================================================
// Stage 2B surface (collections / Text / Bytes / functions)
// ===========================================================================

section('2B grammar structure');

for (const inc of ['bytes', 'keywords-declaration', 'method-call', 'subscript']) {
  check(`root patterns include #${inc}`, rootIncludeNames.indexOf('#' + inc) !== -1);
  check(`repository has #${inc}`, !!grammar.repository[inc]);
}
const BUILTINS_ALL = [
  'print', 'range', 'len', 'str', 'int', 'float', 'bool', 'type', 'encode', 'decode', 'set',
];
for (const b of BUILTINS_ALL) {
  check(`builtin rule matches ${b}()`, ruleMatches('builtin', b + '()'));
}
check('builtin rule: bare `set` is not builtin', !ruleMatches('builtin', 'set'));
check('builtin rule: `setting()` is not builtin', !ruleMatches('builtin', 'setting()'));
check('builtin rule: `lint()` is not builtin', !ruleMatches('builtin', 'lint()'));
check('builtin rule: `decodes()` is not builtin', !ruleMatches('builtin', 'decodes()'));
const punctSrc = grammar.repository.punctuation.match;
for (const pch of ['(', ')', '[', ']', '{', '}', ',', ':', '.']) {
  check(`punctuation rule matches ${pch}`, new RegExp('^(?:' + punctSrc + ')$').test(pch));
}
const asg2 = new RegExp(grammar.repository.assignment.match, 'd');
check('assignment matches `xs[0] = 1`', asg2.test('xs[0] = 1'));
check('assignment matches `xs[i] += v`', asg2.test('xs[i] += v'));
check('assignment matches `m[k] = v`', asg2.test('m[k] = v'));
check('assignment still rejects `xs[0] == 1`', !asg2.test('xs[0] == 1'));
check('assignment still rejects `x == 1`', !asg2.test('x == 1'));
const mcSrc = grammar.repository['method-call'].match;
check('method-call rule matches `.append()`',
  new RegExp(mcSrc).test('.append()'));
check('method-call rule does not match `append()`',
  !new RegExp(mcSrc).test('append()'));
check('method-call name capture is variable.function',
  grammar.repository['method-call'].captures['2'].name === 'variable.function.pentest');

section('2B keywords (global/nonlocal)');

check('keywords-declaration matches global', ruleMatches('keywords-declaration', 'global'));
check('keywords-declaration matches nonlocal', ruleMatches('keywords-declaration', 'nonlocal'));
check('word boundary: `globalx` is not a keyword', !ruleMatches('keywords-declaration', 'globalx'));
check('word boundary: `nonlocality` is not a keyword', !ruleMatches('keywords-declaration', 'nonlocality'));
check('keywords-declaration scope is storage.modifier.pentest',
  grammar.repository['keywords-declaration'].name === 'storage.modifier.pentest');
{
  const t = tokenize('global count');
  const g = t.find((k) => k.t === 'global');
  check('`global count`: global is storage.modifier', hasScope(g, 'storage.modifier.pentest'));
  check('`global count`: not mis-scoped keyword.control', !hasScope(g, 'keyword.control.pentest'));
}
{
  const t = tokenize('global a, b');
  check('`global a, b`: global scoped',
    hasScope(t.find((k) => k.t === 'global'), 'storage.modifier.pentest'));
  check('`global a, b`: comma is plain punctuation',
    hasScope(t.find((k) => k.t === ','), 'punctuation.pentest'));
  check('`global a, b`: no slice scope introduced',
    t.every((k) => !hasScope(k, 'meta.subscript.slice.pentest')));
}
{
  const t = tokenize('nonlocal a, b');
  check('`nonlocal a, b`: nonlocal scoped',
    hasScope(t.find((k) => k.t === 'nonlocal'), 'storage.modifier.pentest'));
  check('`nonlocal a, b`: comma is plain punctuation',
    hasScope(t.find((k) => k.t === ','), 'punctuation.pentest'));
}

section('2B bytes literals');

check('bytes rule scope is string.quoted.double.bytes.pentest',
  grammar.repository.bytes.name === 'string.quoted.double.bytes.pentest');
const bytesBegin = new RegExp(grammar.repository.bytes.begin);
check('bytes begin matches b"x"', bytesBegin.test('b"x"'));
check('bytes begin does NOT match `bad`', !bytesBegin.test('bad'));
check('bytes begin does NOT match bare `b`', !bytesBegin.test('b'));
check('bytes begin does NOT match `b "x"` (space)', !bytesBegin.test('b "x"'));
check('bytes begin does NOT match `cab"x"`', !bytesBegin.test('cab"x"'));
check('bytes begin does NOT match `2b"x"`', !bytesBegin.test('2b"x"'));
{
  const t = tokenize('b"x"');
  const bt = t.find((k) => k.s === 0 && k.e === 4);
  check('`b"x"` tokenizes as one bytes literal', !!bt && bt.t === 'b"x"');
  check('`b"x"` carries the bytes scope', hasScope(bt, 'string.quoted.double.bytes.pentest'));
}
{
  const t = tokenize('bad = 1');
  const bad = t.find((k) => k.t === 'bad');
  check('`bad` is an identifier, not a string',
    !!bad && !bad.scopes.some((s) => s.indexOf('string') !== -1));
}
{
  const t = tokenize('b = 1');
  check('`b = 1`: no bytes literal at line start',
    t.every((k) => (k.s === 0 ? !k.scopes.some((s) => s.indexOf('string') !== -1) : true)));
}
{
  const t = tokenize('b "x"');
  const strTok = t.find((k) => k.t === '"x"');
  check('`b "x"`: string starts at the quote (index 2)', !!strTok && strTok.s === 2);
  check('`b "x"`: no bytes scope anywhere',
    t.every((k) => !hasScope(k, 'string.quoted.double.bytes.pentest')));
}
{
  const t = tokenize('x = b"\\x41"');
  const bt = t.find((k) => k.t === 'b"\\x41"');
  check('`b"\\x41"` tokenizes as one bytes literal', !!bt);
  check('`b"\\x41"` carries the bytes scope', hasScope(bt, 'string.quoted.double.bytes.pentest'));
  check('`\\x41` is scoped as escape',
    hasScope(t.find((k) => k.t === '\\x41'), 'constant.character.escape.pentest'));
}
{
  const t = tokenize('print((b"hi"))');
  check('bytes argument tokenizes',
    hasScope(t.find((k) => k.t === 'b"hi"'), 'string.quoted.double.bytes.pentest'));
  check('grouping parens still punctuation',
    hasScope(t.find((k) => k.t === '('), 'punctuation.pentest'));
}
{
  const t = tokenize('x = "a\\0b"');
  check('`\\0` escape scoped in text string',
    hasScope(t.find((k) => k.t === '\\0'), 'constant.character.escape.pentest'));
}

section('2B slices');

const SLICE_FORMS = ['a[1:2]', 'x = a[:2]', 'x = a[1:]', 'x = a[1:2:3]', 'x = a[:]'];
for (const line of SLICE_FORMS) {
  const t = tokenize(line);
  const region = t.find((k) => hasScope(k, 'meta.subscript.slice.pentest') && k.t[0] === '[');
  check(`slice \`${line}\`: bracket region scoped`, !!region);
  const colons = t.filter((k) => k.t === ':');
  check(`slice \`${line}\`: colon present`, colons.length > 0);
  check(`slice \`${line}\`: colons are slice operators`,
    colons.length > 0 && colons.every((c) => hasScope(c, 'keyword.operator.slice.pentest')
      && hasScope(c, 'meta.subscript.slice.pentest')));
}
{
  const t = tokenize('t[0]');
  const idx = t.find((k) => k.t === '[');
  check('plain index: bracket is punctuation, not slice',
    !!idx && hasScope(idx, 'punctuation.pentest') && !hasScope(idx, 'meta.subscript.slice.pentest'));
  check('plain index: no colon token at all', t.every((k) => k.t !== ':'));
}
{
  const t = tokenize('if x:');
  const c = t.find((k) => k.t === ':');
  check('statement colon is punctuation, not slice',
    !!c && hasScope(c, 'punctuation.pentest') && !hasScope(c, 'meta.subscript.slice.pentest'));
}
{
  const t = tokenize('case 1:');
  const c = t.find((k) => k.t === ':');
  check('case colon is punctuation, not slice',
    !!c && hasScope(c, 'punctuation.pentest') && !hasScope(c, 'meta.subscript.slice.pentest'));
}
{
  const t = tokenize('while x:');
  const c = t.find((k) => k.t === ':');
  check('while colon is punctuation, not slice',
    !!c && hasScope(c, 'punctuation.pentest') && !hasScope(c, 'meta.subscript.slice.pentest'));
}
{
  const t = tokenize('m = {"k": v}');
  const c = t.find((k) => k.t === ':');
  check('map-literal colon is punctuation, not slice',
    !!c && hasScope(c, 'punctuation.pentest') && !hasScope(c, 'meta.subscript.slice.pentest'));
}
{
  const t = tokenize('print((a[1:2]))');
  check('slice in call: closing paren intact',
    hasScope(t.filter((k) => k.t === ')').pop(), 'punctuation.pentest'));
  check('slice in call: print still builtin',
    hasScope(t.find((k) => k.t === 'print'), 'support.function.pentest'));
  check('slice in call: inner slice scoped',
    hasScope(t.find((k) => k.t === '['), 'meta.subscript.slice.pentest'));
}
{
  const t = tokenize('t = "a:b"');
  check('colon inside a string never becomes a slice operator',
    t.filter((k) => k.t === ':').every((c) => !hasScope(c, 'keyword.operator.slice.pentest')));
  check('string with colon stays one token',
    hasScope(t.find((k) => k.t === '"a:b"'), 'string.quoted.double.pentest'));
}
{
  const t = tokenize('xs = [a[1:2], b]');
  check('nested subscript: inner slice scoped',
    t.some((k) => hasScope(k, 'meta.subscript.slice.pentest')));
  check('nested subscript: closing bracket tokenized',
    hasScope(t.filter((k) => k.t === ']').pop(), 'punctuation.pentest')
    || hasScope(t.filter((k) => k.t === ']').pop(), 'punctuation.section.brackets.end.pentest'));
}

section('2B collection literals');

{
  const t = tokenize('xs = []');
  check('`[]`: open bracket punctuation',
    hasScope(t.find((k) => k.t === '['), 'punctuation.pentest'));
  check('`[]`: close bracket punctuation',
    hasScope(t.find((k) => k.t === ']'), 'punctuation.pentest'));
}
{
  const t = tokenize('e = {}');
  check('`{}`: open brace punctuation',
    hasScope(t.find((k) => k.t === '{'), 'punctuation.pentest'));
  check('`{}`: close brace punctuation',
    hasScope(t.find((k) => k.t === '}'), 'punctuation.pentest'));
}
{
  const t = tokenize('t = ()');
  check('`()` open paren punctuation',
    hasScope(t.find((k) => k.t === '('), 'punctuation.pentest'));
  check('`()` close paren punctuation',
    hasScope(t.find((k) => k.t === ')'), 'punctuation.pentest'));
}
{
  const t = tokenize('xs = [1, 2]');
  check('list literal: comma punctuation',
    hasScope(t.find((k) => k.t === ','), 'punctuation.pentest'));
  check('list literal: first element number',
    hasScope(t.find((k) => k.t === '1'), 'constant.numeric.pentest'));
  check('list literal: second element number',
    hasScope(t.find((k) => k.t === '2'), 'constant.numeric.pentest'));
}
{
  const t = tokenize('s = {1, 2}');
  check('set literal: braces punctuation',
    hasScope(t.find((k) => k.t === '{'), 'punctuation.pentest'));
  check('set literal: elements tokenized',
    hasScope(t.find((k) => k.t === '1'), 'constant.numeric.pentest'));
}
{
  const t = tokenize('m = {"k": v}');
  check('map literal: key string',
    hasScope(t.find((k) => k.t === '"k"'), 'string.quoted.double.pentest'));
  check('map literal: braces punctuation',
    hasScope(t.find((k) => k.t === '{'), 'punctuation.pentest'));
}
{
  const t = tokenize('e = set()');
  check('`set()` is the builtin constructor scope',
    hasScope(t.find((k) => k.t === 'set'), 'support.function.pentest'));
}
{
  // Comma-join needs (and gets) NO special treatment: the comma stays plain
  // punctuation and neither operand is disturbed.
  const t = tokenize('m = "hello", "world"');
  check('comma-join: comma is plain punctuation',
    hasScope(t.find((k) => k.t === ','), 'punctuation.pentest'));
  check('comma-join: first string intact',
    hasScope(t.find((k) => k.t === '"hello"'), 'string.quoted.double.pentest'));
  check('comma-join: second string intact',
    hasScope(t.find((k) => k.t === '"world"'), 'string.quoted.double.pentest'));
  check('comma-join: no slice scope anywhere',
    t.every((k) => !hasScope(k, 'meta.subscript.slice.pentest')));
}
{
  const t = tokenize('xs[0] = 1');
  check('index-assign: target scoped',
    hasScope(t.find((k) => k.t === 'xs'), 'variable.other.readwrite.pentest'));
  check('index-assign: `=` operator',
    hasScope(t.find((k) => k.t === '='), 'keyword.operator.pentest'));
  check('index-assign: index number',
    hasScope(t.find((k) => k.t === '0'), 'constant.numeric.pentest'));
}
{
  const t = tokenize('xs[i] += v');
  check('index compound-assign: target scoped',
    hasScope(t.find((k) => k.t === 'xs'), 'variable.other.readwrite.pentest'));
  check('index compound-assign: `+=` operator',
    hasScope(t.find((k) => k.t === '+='), 'keyword.operator.pentest'));
}

section('2B method-call grammar');

{
  const t = tokenize('xs.append(1)');
  check('`xs.append(1)`: dot has accessor scope',
    hasScope(t.find((k) => k.t === '.'), 'punctuation.accessor.dot.pentest'));
  check('`xs.append(1)`: append is a function-name scope',
    hasScope(t.find((k) => k.t === 'append'), 'variable.function.pentest'));
}
{
  const t = tokenize('b.decode()');
  const dec = t.find((k) => k.t === 'decode');
  check('`b.decode()`: decode scoped as method call, not the builtin',
    !!dec && hasScope(dec, 'variable.function.pentest')
    && !hasScope(dec, 'support.function.pentest'));
}
{
  const t = tokenize('s = set(1)');
  check('bare `set()` still builtin scope in an expression',
    hasScope(t.find((k) => k.t === 'set'), 'support.function.pentest'));
}

section('2B snippets');

check('snippet file has exactly 27 snippets (12 M1/2A + 4 2B + 4 2C + 4 0.2.1 + 3 HTTP)',
  Object.keys(snippets).length === 27);
{
  const s = snippets['function-variadic'];
  check('snippet function-variadic exists', !!s);
  if (s) {
    check('function-variadic prefix', s.prefix === 'functionargs');
    check('function-variadic first line',
      s.body[0] === '${1:name}(${2:*args}, ${3:**kwargs}):');
    check('function-variadic declares *args', s.body[0].indexOf('*args') !== -1);
    check('function-variadic declares **kwargs', s.body[0].indexOf('**kwargs') !== -1);
    check('function-variadic indented body', s.body[1] === '\t${0}');
    check('function-variadic description', typeof s.description === 'string'
      && s.description.indexOf('*args') !== -1);
  }
}
{
  const s = snippets['function-nested'];
  check('snippet function-nested exists', !!s);
  if (s) {
    check('function-nested prefix', s.prefix === 'functionnested');
    check('function-nested outer function line', s.body[0] === '${1:outer}():');
    check('function-nested inner nested function line',
      s.body[1] === '\t${2:inner}(${3:args}):');
    check('function-nested description mentions closure/nested',
      typeof s.description === 'string'
      && /closure|nested/.test(s.description));
  }
}
{
  const s = snippets['global'];
  check('snippet global exists', !!s);
  if (s) {
    check('global snippet prefix', s.prefix === 'global');
    check('global snippet body', s.body[0] === 'global ${1:name}$0');
    check('global snippet description mentions module global',
      typeof s.description === 'string' && s.description.indexOf('module global') !== -1);
  }
}
{
  const s = snippets['nonlocal'];
  check('snippet nonlocal exists', !!s);
  if (s) {
    check('nonlocal snippet prefix', s.prefix === 'nonlocal');
    check('nonlocal snippet body', s.body[0] === 'nonlocal ${1:name}$0');
    check('nonlocal snippet description mentions enclosing',
      typeof s.description === 'string' && s.description.indexOf('enclosing') !== -1);
  }
}
check('original 12 snippets still present',
  Object.keys(SNIPPET_FIRST_LINE).every((k) => !!snippets[k]));

section('2B completion provider');

{
  const items = completions('x = 1', 5);
  for (const b of ['len', 'str', 'int', 'float', 'bool', 'type', 'encode', 'decode', 'set']) {
    const it = findItem(items, b);
    check(`2B builtin ${b} offered`, !!it);
    if (it) {
      check(`2B builtin ${b} kind Function`, it.kind === fakeVscode.CompletionItemKind.Function);
      check(`2B builtin ${b} snippet insert`, snippetText(it).indexOf(b + '(') === 0);
      check(`2B builtin ${b} marked call-only/reserved`,
        /builtin function/.test(it.detail) && /reserved/.test(it.detail));
    }
  }
}
{
  const items = completions('x = 1', 5);
  const g = findItem(items, 'global');
  check('global offered as Keyword', !!g && g.kind === fakeVscode.CompletionItemKind.Keyword);
  check('global documented (placement rule)', !!g && /before/.test(docText(g))
    && docText(g).indexOf('global count') !== -1);
  const n = findItem(items, 'nonlocal');
  check('nonlocal offered as Keyword', !!n && n.kind === fakeVscode.CompletionItemKind.Keyword);
  check('nonlocal documented (enclosing binding)', !!n && /enclosing/.test(docText(n)));
}
{
  const items = completions('x = 1', 5);
  const t = findItem(items, 'type');
  const d = t ? docText(t) : '';
  for (const tn of ['none', 'int', 'float', 'bool', 'text', 'bytes', 'list', 'map', 'set', 'tuple', 'function', 'range']) {
    check(`type() doc lists \`${tn}\``, d.indexOf('`' + tn + '`') !== -1);
  }
  check('type() doc lists text (not string as a type name)',
    d.indexOf('`text`') !== -1 && d.indexOf('`string`') === -1);
}
{
  const items = completions('len = 1\nset = 2\nglobal = 3\ndecode = 4\nnonlocal = 5', 50);
  for (const w of ['len', 'set', 'global', 'decode', 'nonlocal']) {
    const asVar = items.find((i) => labelOf(i) === w
      && i.kind === fakeVscode.CompletionItemKind.Variable);
    check(`2B reserved name ${w} not offered as variable`, !asVar);
  }
}
const EXPECTED_METHODS = [
  'append', 'insert', 'index', 'reverse', 'pop', 'remove', 'clear',
  'get', 'keys', 'values', 'items',
  'add', 'discard',
  'upper', 'lower', 'split', 'join', 'find', 'replace', 'strip', 'starts_with', 'ends_with',
  'decode',
];
{
  const items = completions('xs.', 3);
  for (const m of EXPECTED_METHODS) {
    const it = items.find((i) => labelOf(i) === m);
    check(`method ${m} offered after dot`, !!it);
    if (it) {
      check(`method ${m} kind Method`, it.kind === fakeVscode.CompletionItemKind.Method);
      check(`method ${m} documented`, docText(it).length > 0);
    }
  }
  const labels = items.filter((i) => i.kind === fakeVscode.CompletionItemKind.Method)
    .map(labelOf).sort();
  check('method surface is exactly the contract list (no extras)',
    labels.join(',') === EXPECTED_METHODS.slice().sort().join(','));
  const append = items.find((i) => labelOf(i) === 'append');
  check('append detail names list', !!append && /list/.test(append.detail));
  const sw = items.find((i) => labelOf(i) === 'starts_with');
  check('text method is starts_with (not startswith)', !!sw && /text/.test(sw.detail));
  const ew = items.find((i) => labelOf(i) === 'ends_with');
  check('text method is ends_with (not endswith)', !!ew && /text/.test(ew.detail));
  const dec = items.find((i) => labelOf(i) === 'decode');
  check('decode method detail names bytes', !!dec && /bytes/.test(dec.detail));
  const get = items.find((i) => labelOf(i) === 'get');
  check('get method detail names map', !!get && /map/.test(get.detail));
}
{
  const items = completions('xs.app', 6);
  const append = items.find((i) => labelOf(i) === 'append');
  check('partial word after dot still offers append',
    !!append && append.kind === fakeVscode.CompletionItemKind.Method);
}
{
  const items = completions('f().', 4);
  check('method access on a call result offers methods',
    items.some((i) => i.kind === fakeVscode.CompletionItemKind.Method));
}
{
  const items = completions('x = 1.5', 7);
  check('float literal does NOT trigger method completions',
    !items.some((i) => i.kind === fakeVscode.CompletionItemKind.Method));
}
{
  const items = completions('x = 1', 5);
  check('no method completions outside dot context',
    !items.some((i) => i.kind === fakeVscode.CompletionItemKind.Method));
}
check('provider registered with `.` trigger character',
  Array.isArray(captured.triggers) && captured.triggers.indexOf('.') !== -1);

section('2B README');

check('README mentions bytes literals', readme.indexOf('b"..."') !== -1);
check('README mentions global/nonlocal', readme.indexOf('`global`/`nonlocal`') !== -1);
check('README mentions slice forms', readme.indexOf('a[1:2]') !== -1);
check('README mentions method names', readme.indexOf('starts_with') !== -1);
check('README documents the smoke script', readme.indexOf('test/smoke.js') !== -1);

// ===========================================================================
// Stage 2C surface (garbage collection / exceptions / classes / identity)
// ===========================================================================

section('2C grammar structure');

for (const inc of ['class-definition', 'attribute']) {
  check(`root patterns include #${inc}`, rootIncludeNames.indexOf('#' + inc) !== -1);
  check(`repository has #${inc}`, !!grammar.repository[inc]);
}
check('builtin rule matches collect()', ruleMatches('builtin', 'collect()'));
check('builtin rule: bare `collect` is not builtin', !ruleMatches('builtin', 'collect'));
check('builtin rule: `collector(` is not builtin', !ruleMatches('builtin', 'collector('));
for (const kw of ['try', 'except', 'finally', 'raise']) {
  check(`keywords-control matches ${kw}`, ruleMatches('keywords-control', kw));
}
check('keywords-control word boundary: `tryx` is not a keyword',
  !ruleMatches('keywords-control', 'tryx'));
check('keywords-control word boundary: `raiseError` is not a keyword',
  !ruleMatches('keywords-control', 'raiseError'));
for (const kw of ['as', 'is']) {
  check(`keywords-other matches ${kw}`, ruleMatches('keywords-other', kw));
}
check('keywords-other word boundary: `aside` is not a keyword',
  !ruleMatches('keywords-other', 'aside'));
check('keywords-other word boundary: `island` is not a keyword',
  !ruleMatches('keywords-other', 'island'));
{
  const cd = grammar.repository['class-definition'];
  check('class-definition captures the class keyword',
    cd.captures['2'].name === 'storage.type.class.pentest');
  check('class-definition captures the class name',
    cd.captures['4'].name === 'entity.name.class.pentest');
  check('class-definition matches `class Point:`-style line',
    new RegExp(cd.match).test('class Point:'));
  check('class-definition does not match a mid-line word',
    !new RegExp('^(?:' + cd.match + ')$').test('x = classy'));
  const at = grammar.repository['attribute'];
  check('attribute rule captures member name',
    at.captures['2'].name === 'variable.other.member.pentest');
  check('attribute rule matches bare `.nope`', new RegExp(at.match).test('.nope'));
  {
    // Method calls are pre-empted by the earlier #method-call include, so
    // this is checked behaviorally (the isolated regex alone backtracks).
    const t = tokenize('xs.append(1)');
    check('`.append()` is method-call scope, not a bare attribute',
      hasScope(t.find((k) => k.t === 'append'), 'variable.function.pentest')
      && !hasScope(t.find((k) => k.t === 'append'), 'variable.other.member.pentest'));
  }
}

section('2C tokenizer behavior');

{
  const t = tokenize('class Point:');
  check('`class Point:`: class keyword scoped',
    hasScope(t.find((k) => k.t === 'class'), 'storage.type.class.pentest'));
  check('`class Point:`: name is entity.name.class',
    hasScope(t.find((k) => k.t === 'Point'), 'entity.name.class.pentest'));
  const c = t.find((k) => k.t === ':');
  check('`class Point:`: colon is punctuation, not slice',
    !!c && hasScope(c, 'punctuation.pentest') && !hasScope(c, 'meta.subscript.slice.pentest'));
}
{
  const t = tokenize('try:');
  check('`try:`: try is control keyword',
    hasScope(t.find((k) => k.t === 'try'), 'keyword.control.pentest'));
  const c = t.find((k) => k.t === ':');
  check('`try:`: colon is punctuation', !!c && hasScope(c, 'punctuation.pentest'));
}
{
  const t = tokenize('except ValueError as e:');
  check('except is control keyword',
    hasScope(t.find((k) => k.t === 'except'), 'keyword.control.pentest'));
  check('as is keyword.other',
    hasScope(t.find((k) => k.t === 'as'), 'keyword.other.pentest'));
  const c = t.find((k) => k.t === ':');
  check('except colon is punctuation, not slice',
    !!c && hasScope(c, 'punctuation.pentest') && !hasScope(c, 'meta.subscript.slice.pentest'));
}
{
  const t = tokenize('raise ValueError');
  check('raise is control keyword',
    hasScope(t.find((k) => k.t === 'raise'), 'keyword.control.pentest'));
}
{
  const t = tokenize('if a is b:');
  check('`is` inside a condition is keyword.other',
    hasScope(t.find((k) => k.t === 'is'), 'keyword.other.pentest'));
}
{
  const t = tokenize('p.x');
  check('`p.x`: dot has accessor scope',
    hasScope(t.find((k) => k.t === '.'), 'punctuation.accessor.dot.pentest'));
  check('`p.x`: member is variable.other.member',
    hasScope(t.find((k) => k.t === 'x'), 'variable.other.member.pentest'));
  check('`p.x` bare: member is NOT scoped as a function name',
    !hasScope(t.find((k) => k.t === 'x'), 'variable.function.pentest'));
}
{
  const t = tokenize('p.x = 1');
  const asg = new RegExp(grammar.repository.assignment.match, 'd');
  check('assignment rule matches dotted target `p.x = 1`', asg.test('p.x = 1'));
  check('`p.x = 1`: whole dotted target scoped readwrite',
    t.some((k) => k.t === 'p.x'
      && hasScope(k, 'variable.other.readwrite.pentest')));
  check('`p.x = 1`: `=` operator scoped',
    hasScope(t.find((k) => k.t === '='), 'keyword.operator.pentest'));
}
{
  const t = tokenize('n = collect()');
  check('`collect()` gets the builtin scope',
    hasScope(t.find((k) => k.t === 'collect'), 'support.function.pentest'));
}

section('2C snippets');

{
  const s = snippets['class'];
  check('snippet class exists', !!s);
  if (s) {
    check('class snippet prefix', s.prefix === 'class');
    check('class snippet first line', s.body[0] === 'class ${1:Name}:');
    check('class snippet has a method with self',
      s.body[1].indexOf('(') !== -1 && s.body[1].indexOf('self') !== -1);
    check('class snippet indented body', s.body[2] === '\t\t${0}');
    check('class snippet description mentions methods/instances',
      typeof s.description === 'string' && /methods|instances/.test(s.description));
  }
}
{
  const s = snippets['init'];
  check('snippet init exists', !!s);
  if (s) {
    check('init snippet prefix', s.prefix === 'init');
    check('init snippet first line', s.body[0] === '__init__(self, ${1:args}):');
    check('init snippet description mentions constructor',
      typeof s.description === 'string' && s.description.indexOf('constructor') !== -1);
  }
}
{
  const s = snippets['try-except'];
  check('snippet try-except exists', !!s);
  if (s) {
    check('try snippet prefix', s.prefix === 'try');
    check('try snippet first line', s.body[0] === 'try:');
    check('try snippet has except-as binding',
      s.body[2].indexOf('except') !== -1 && s.body[2].indexOf(' as ') !== -1);
    check('try snippet description mentions exact class identity',
      typeof s.description === 'string' && /exact class identity/.test(s.description));
  }
}
{
  const s = snippets['raise'];
  check('snippet raise exists', !!s);
  if (s) {
    check('raise snippet prefix', s.prefix === 'raise');
    check('raise snippet body', s.body[0] === 'raise ${1:expr}$0');
    check('raise snippet description mentions instance/class',
      typeof s.description === 'string' && /instance/.test(s.description));
  }
}
check('all 12 M1/2A + 4 2B + 4 2C snippets present',
  Object.keys(SNIPPET_FIRST_LINE).every((k) => !!snippets[k])
  && !!snippets.class && !!snippets.init
  && !!snippets['try-except'] && !!snippets.raise);

section('2C completion provider');

{
  const items = completions('x = 1', 5);
  const c = findItem(items, 'collect');
  check('collect offered as builtin Function', !!c
    && c.kind === fakeVscode.CompletionItemKind.Function);
  if (c) {
    check('collect snippet insert', snippetText(c).indexOf('collect()') === 0);
    check('collect marked call-only/reserved',
      /builtin function/.test(c.detail) && /reserved/.test(c.detail));
    check('collect documented (freed blocks)',
      /freed/.test(docText(c)));
  }
}
{
  const items = completions('x = 1', 5);
  for (const kw of ['try', 'except', 'finally', 'raise', 'as', 'class', 'is']) {
    const it = findItem(items, kw);
    check(`2C keyword ${kw} offered as Keyword`, !!it
      && it.kind === fakeVscode.CompletionItemKind.Keyword);
    if (it) check(`2C keyword ${kw} documented`, docText(it).length > 0);
  }
  const cls = findItem(items, 'class');
  check('class keyword doc shows a class block', !!cls && docText(cls).indexOf('class Point:') !== -1);
  const tr = findItem(items, 'try');
  check('try keyword doc shows try/except/finally', !!tr
    && docText(tr).indexOf('finally') !== -1 && docText(tr).indexOf('except') !== -1);
  const r = findItem(items, 'raise');
  check('raise keyword doc mentions the TypeError wording', !!r
    && docText(r).indexOf('exceptions must be class or instance') !== -1);
  const iso = findItem(items, 'is');
  check('is keyword doc mentions None is None', !!iso && docText(iso).indexOf('None') !== -1);
}
{
  const items = completions('x = 1', 5);
  const t = findItem(items, 'type');
  const d = t ? docText(t) : '';
  for (const tn of ['class', 'method']) {
    check(`type() doc lists \`${tn}\``, d.indexOf('`' + tn + '`') !== -1);
  }
  check('type() doc mentions instance reports its class name',
    d.indexOf('class name') !== -1);
}
{
  const items = completions('class = 1\nis = 2\ncollect = 3\nraise = 4', 38);
  for (const w of ['class', 'is', 'collect', 'raise']) {
    const asVar = items.find((i) => labelOf(i) === w
      && i.kind === fakeVscode.CompletionItemKind.Variable);
    check(`2C reserved name ${w} not offered as variable`, !asVar);
  }
}

section('2C language-configuration');

check('increaseIndent: class Point:', inc.test('class Point:'));
check('increaseIndent: try:', inc.test('try:'));
check('increaseIndent: except ValueError as e:', inc.test('except ValueError as e:'));
check('increaseIndent: finally:', inc.test('finally:'));
check('decreaseIndent: except ValueError as e:', dec.test('except ValueError as e:'));
check('decreaseIndent: finally:', dec.test('finally:'));
check('decreaseIndent: raise e: not dedented', !dec.test('raise e:'));

section('2C README');

check('README mentions the 2C exception keywords',
  readme.indexOf('`try`/`except`/`finally`/`raise`') !== -1);
check('README mentions the 2C class/identity keywords',
  readme.indexOf('`class`/`is`') !== -1);
check('README mentions collect', readme.indexOf('collect') !== -1);
check('README mentions 2C snippets', readme.indexOf('`init`') !== -1);

// ===========================================================================
section('post-2C basics: run button + input/time/clock');

// Run command (TODO.txt: "run button not working for .pen").
check('extension registers pentest.run command',
  typeof captured.commands['pentest.run'] === 'function');
check('extension still registers pentest.toggleLineComment',
  typeof captured.commands['pentest.toggleLineComment'] === 'function');
{
  const cmds = (pkg.contributes.commands || []);
  const run = cmds.find((c) => c.command === 'pentest.run');
  check('package contributes pentest.run command', !!run);
  if (run) {
    check('run command has title', typeof run.title === 'string' && run.title.length > 0);
    check('run command has play icon', run.icon === '$(play)');
    check('run command is in the Pentest category', run.category === 'Pentest');
  }
  const title = ((pkg.contributes.menus || {})['editor/title'] || []);
  const titleRun = title.find((m) => m.command === 'pentest.run');
  check('run button in editor/title', !!titleRun);
  if (titleRun) {
    check('editor/title run button gated to pentest files',
      /resourceLangId == pentest/.test(titleRun.when || ''));
    check('editor/title run button is navigation-grouped',
      /^navigation/.test(titleRun.group || ''));
  }
  const ctx = ((pkg.contributes.menus || {})['editor/context'] || []);
  check('run command in editor/context menu',
    !!ctx.find((m) => m.command === 'pentest.run'));
  const palette = ((pkg.contributes.menus || {})['commandPalette'] || []);
  check('run command in command palette (language-gated)',
    !!palette.find((m) => m.command === 'pentest.run'));
}
check('run implementation spawns `pentest run` in a Pentest terminal',
  extSrc.indexOf("createTerminal('Pentest')") !== -1
  && extSrc.indexOf("' run '") !== -1);
check('run implementation saves dirty documents first',
  extSrc.indexOf('doc.save()') !== -1);
check('run paths are shell-quoted',
  extSrc.indexOf('shellQuote(doc.uri.fsPath)') !== -1);
// Invoking with no active editor must not throw (info message + return).
check('pentest.run is safe with no active editor', (() => {
  try {
    captured.commands['pentest.run']();
    return true;
  } catch (e) {
    return false;
  }
})());

// 0.2.1: the Run button runs THE FILE THE DEVELOPER IS ON — the resource the
// ▶ button was clicked for (passed by the menu), else the active editor.
check('run resolves the clicked editor resource first',
  extSrc.indexOf('typeof resource.fsPath') !== -1
  && extSrc.indexOf('openTextDocument(resource)') !== -1);
check('run falls back to the active editor document',
  extSrc.indexOf('doc = editor && editor.document') !== -1);
check('run listens for a closed Pentest terminal',
  extSrc.indexOf('onDidCloseTerminal') !== -1);
{
  const docOf = (fsPath) => ({
    languageId: 'pentest',
    isUntitled: false,
    isDirty: false,
    version: 1,
    lineCount: 1,
    uri: { fsPath, toString: () => 'file://' + fsPath },
    getText: () => '',
    lineAt: () => ({ text: '' }),
    save: () => true,
  });
  const clicked = docOf('/tmp/opencode/clicked.pen');
  fakeVscode.workspace.textDocuments = [clicked];
  fakeVscode.window.activeTextEditor = undefined;
  captured.commands['pentest.run'](clicked.uri); // menu passes the resource
  const t1 = captured.terminals[0];
  check('resource-arg run executes the clicked file', !!t1
    && t1.sent.length === 1
    && t1.sent[0].indexOf('/tmp/opencode/clicked.pen') !== -1);
  check('resource-arg run shows the terminal', !!t1 && t1.shown >= 1);

  const active = docOf('/tmp/opencode/active.pen');
  fakeVscode.workspace.textDocuments = [];
  fakeVscode.window.activeTextEditor = { document: active };
  captured.commands['pentest.run'](); // palette: no resource argument
  check('active-editor fallback runs the active file', !!t1
    && t1.sent.length === 2
    && t1.sent[1].indexOf('/tmp/opencode/active.pen') !== -1);
  check('repeat runs reuse one terminal', captured.terminals.length === 1);
  fakeVscode.window.activeTextEditor = undefined;
}
{
  // Dirty documents are saved BEFORE execution — `await doc.save()` makes
  // this scenario async, so its final assertions land in `late` below.
  const dirty = {
    languageId: 'pentest',
    isUntitled: false,
    isDirty: true,
    saveCalls: 0,
    uri: {
      fsPath: '/tmp/opencode/dirty.pen',
      toString: () => 'file:///tmp/opencode/dirty.pen',
    },
    getText: () => '',
    lineAt: () => ({ text: '' }),
    save() { this.saveCalls += 1; return Promise.resolve(true); },
  };
  fakeVscode.window.activeTextEditor = { document: dirty };
  const p = captured.commands['pentest.run']();
  check('dirty run saves the document first', dirty.saveCalls === 1);
  late = Promise.resolve(p).then(() => {
    const t = captured.terminals[0];
    check('dirty run executes after the save', !!t
      && t.sent.length === 3
      && t.sent[2].indexOf('/tmp/opencode/dirty.pen') !== -1);
    fakeVscode.window.activeTextEditor = undefined;
  }, (err) => {
    check('dirty run completes without error' + (err ? ` (${err})` : ''), false);
  });
}

// input / time / clock — grammar, completions, reserved-ness, README.
{
  for (const name of ['input', 'time', 'clock']) {
    const t = tokenize(`${name}(x)`);
    const tok = t.find((k) => k.t === name);
    check(`grammar: ${name}(call scoped as builtin)`, !!tok
      && hasScope(tok, 'support.function.pentest'));
  }
  const builtinRule = JSON.stringify(grammar.repository && grammar.repository.builtin
    ? grammar.repository.builtin : grammar);
  check('grammar builtin pattern lists input/time/clock',
    builtinRule.indexOf('input|time|clock') !== -1);
}
{
  const items = completions('x = 1', 5);
  for (const name of ['input', 'time', 'clock']) {
    const it = findItem(items, name);
    check(`completion: ${name} offered as builtin`, !!it
      && it.kind === fakeVscode.CompletionItemKind.Function);
    check(`completion: ${name} has documentation`, !!it && docText(it).length > 0);
  }
}
{
  const items = completions(
    'input = 1\ntime = 2\nclock = 3\n', 30);
  for (const w of ['input', 'time', 'clock']) {
    const asVar = items.find((i) => labelOf(i) === w
      && i.kind === fakeVscode.CompletionItemKind.Variable);
    check(`completion: reserved ${w} not offered as variable`, !asVar);
  }
}
check('README documents the run button', readme.indexOf('Run button') !== -1);
check('README documents input', readme.indexOf('`input()`') !== -1);

// ===========================================================================
section('HTTP builtins: http_get / http_post / url_encode');

// The three libcurl-backed builtins landed in the runtime (semantic.cpp
// is_reserved): http_get(url[, headers]), http_post(url, body[, headers])
// -> text "STATUS\nBODY", and url_encode(text) -> percent-encoded text.
const HTTP_BUILTINS = ['http_get', 'http_post', 'url_encode'];
{
  // Grammar: scoped as builtin before `()`, never bare, no prefix bleed.
  for (const name of HTTP_BUILTINS) {
    const t = tokenize(`${name}(x)`);
    const tok = t.find((k) => k.t === name);
    check(`grammar: ${name}(...) scoped as builtin`, !!tok
      && hasScope(tok, 'support.function.pentest'));
    check(`grammar: bare ${name} not builtin`, !ruleMatches('builtin', name));
    check(`grammar: ${name}x() not builtin`, !ruleMatches('builtin', name + 'x()'));
  }
  const builtinRule = JSON.stringify(grammar.repository.builtin || {});
  check('grammar builtin pattern lists all three HTTP names',
    HTTP_BUILTINS.every((n) => builtinRule.indexOf(n) !== -1));
  check('grammar builtin pattern keeps http_post next to the new pair',
    builtinRule.indexOf('http_post|http_get|url_encode') !== -1);
}
{
  // Completions: Function kind, call-only/reserved detail, docs, snippet.
  const items = completions('x = 1', 5);
  for (const name of HTTP_BUILTINS) {
    const it = findItem(items, name);
    check(`completion: ${name} offered as builtin Function`, !!it
      && it.kind === fakeVscode.CompletionItemKind.Function);
    check(`completion: ${name} marked call-only/reserved`, !!it
      && /builtin function/.test(it.detail) && /reserved/.test(it.detail));
    check(`completion: ${name} documented`, !!it && docText(it).length > 0);
    check(`completion: ${name} snippet inserts ${name}(...)`, !!it
      && snippetText(it).indexOf(name + '(') === 0);
  }
  const hp = findItem(items, 'http_post');
  check('completion: http_post doc mentions the optional 3rd headers argument',
    !!hp && /headers/.test(docText(hp)) && /3rd/.test(docText(hp)));
  const hg = findItem(items, 'http_get');
  check('completion: http_get doc covers STATUS\\nBODY + optional headers',
    !!hg && /STATUS/.test(docText(hg)) && /BODY/.test(docText(hg))
      && /headers/.test(docText(hg)));
  const ue = findItem(items, 'url_encode');
  check('completion: url_encode doc covers percent-encoding', !!ue
    && /percent/i.test(docText(ue)));
}
{
  // Reserved: never offered as a plain variable when present in the file.
  const src = HTTP_BUILTINS.map((w, i) => `${w} = ${i}`).join('\n') + '\n';
  const items = completions(src, src.length);
  for (const w of HTTP_BUILTINS) {
    const asVar = items.find((i) => labelOf(i) === w
      && i.kind === fakeVscode.CompletionItemKind.Variable);
    check(`completion: reserved ${w} not offered as variable`, !asVar);
  }
}
// Source tables carry the names (RESERVED set + BUILTIN_DOCS entries).
check('extension source reserves all three HTTP builtins',
  HTTP_BUILTINS.every((n) => extSrc.indexOf("'" + n + "'") !== -1));
check('extension source documents every HTTP builtin',
  HTTP_BUILTINS.every((n) => new RegExp("'" + n + "':\\s*\\{").test(extSrc)));
// Hover mirrors the docs (placeholders stripped).
{
  const t = hoverText(hover('r = http_get(url\n)', 10));
  check('hover http_get: plain signature + STATUS\\nBODY contract',
    t.indexOf('http_get(url)') !== -1 && /STATUS/.test(t) && t.indexOf('${') === -1);
  const t2 = hoverText(hover('r = url_encode(q\n)', 10));
  check('hover url_encode: percent-encoding contract', /percent/i.test(t2));
  const t3 = hoverText(hover('r = http_post(u, b\n)', 10));
  check('hover http_post: headers mentioned', /headers/.test(t3));
}
// Snippets for the three builtins.
{
  const expected = {
    'http-get': { prefix: 'httpget', first: '${1:resp} = http_get(${2:url})$0' },
    'http-post': { prefix: 'httppost', first: '${1:resp} = http_post(${2:url}, ${3:body})$0' },
    'url-encode': { prefix: 'urlencode', first: '${1:enc} = url_encode(${2:text})$0' },
  };
  for (const [key, exp] of Object.entries(expected)) {
    const s = snippets[key];
    check(`snippet ${key} exists`, !!s);
    if (!s) continue;
    check(`snippet ${key} prefix`, s.prefix === exp.prefix);
    check(`snippet ${key} first body line`, s.body[0] === exp.first);
    check(`snippet ${key} has prefix/body/description shape`,
      typeof s.prefix === 'string' && Array.isArray(s.body) && s.body.length > 0
      && typeof s.description === 'string' && s.description.length > 0);
  }
}

// ===========================================================================
section('0.2.1: variable. cross-scope access');

check('root patterns include #varqual', rootIncludeNames.indexOf('#varqual') !== -1);
check('repository has #varqual', !!grammar.repository.varqual);
{
  const vq = grammar.repository.varqual;
  check('varqual rule scope is meta.variable-access.pentest',
    !!vq && vq.name === 'meta.variable-access.pentest');
  if (vq) {
    check('varqual match covers `variable.owner`', new RegExp(vq.match).test('variable.owner'));
    check('varqual does not match bare `variable`', !new RegExp(vq.match).test('variable'));
    check('varqual keyword capture is keyword.other',
      vq.captures['1'].name === 'keyword.other.pentest');
    check('varqual dot capture is punctuation.accessor.dot',
      vq.captures['2'].name === 'punctuation.accessor.dot.pentest');
    check('varqual qualifier capture is variable.other.qualifier',
      vq.captures['3'].name === 'variable.other.qualifier.pentest');
    check('varqual include precedes #assignment (same-index win)',
      rootIncludeNames.indexOf('#varqual') < rootIncludeNames.indexOf('#assignment'));
  }
}
{
  const t = tokenize('variable.global count');
  check('`variable.global count`: variable is keyword.other',
    hasScope(t.find((k) => k.t === 'variable'), 'keyword.other.pentest'));
  check('`variable.global count`: dot is accessor punctuation',
    hasScope(t.find((k) => k.t === '.'), 'punctuation.accessor.dot.pentest'));
  check('`variable.global count`: global is the qualifier',
    hasScope(t.find((k) => k.t === 'global'), 'variable.other.qualifier.pentest'));
}
{
  const t = tokenize('print((variable.owner k))');
  check('`variable.owner` in a call: qualifier scoped',
    hasScope(t.find((k) => k.t === 'owner'), 'variable.other.qualifier.pentest'));
  check('`print()` still builtin in the same line',
    hasScope(t.find((k) => k.t === 'print'), 'support.function.pentest'));
}
{
  const t = tokenize('variable.parent p += 100');
  check('`variable.parent`: parent qualifier scoped',
    hasScope(t.find((k) => k.t === 'parent'), 'variable.other.qualifier.pentest'));
  check('`variable.parent p += 100`: compound operator intact',
    hasScope(t.find((k) => k.t === '+='), 'keyword.operator.pentest'));
}
{
  const t = tokenize('variable = 1');
  check('bare `variable = 1`: identifier, NOT keyword.other (not reserved)',
    !hasScope(t.find((k) => k.t === 'variable'), 'keyword.other.pentest'));
}
{
  const items = completions('variable.', 9);
  const g = findItem(items, 'global');
  check('after `variable.`: global qualifier offered', !!g
    && g.kind === fakeVscode.CompletionItemKind.Keyword);
  check('after `variable.`: global documented (module global)', !!g
    && /module global/.test(docText(g)));
  const p = findItem(items, 'parent');
  check('after `variable.`: parent qualifier offered', !!p
    && p.kind === fakeVscode.CompletionItemKind.Keyword);
  check('after `variable.`: parent documented (enclosing binding)', !!p
    && /enclosing/.test(docText(p)));
  check('after `variable.`: NO method completions',
    !items.some((i) => i.kind === fakeVscode.CompletionItemKind.Method));
}
{
  const src = 'holder:\n    pass\nworker(x):\n    pass\nmain:\n    variable.';
  const items = completions(src, src.length);
  const h = findItem(items, 'holder');
  check('after `variable.`: document function offered as qualifier', !!h
    && h.kind === fakeVscode.CompletionItemKind.Function);
  check('function qualifier detail shows variable.<fn>', !!h
    && /variable\.holder/.test(h.detail));
  check('function qualifier documented (same scope rule)', !!h
    && /share this scope/.test(docText(h)));
  const w = findItem(items, 'worker');
  check('after `variable.`: parameterized function offered as qualifier', !!w
    && w.kind === fakeVscode.CompletionItemKind.Function);
}
{
  const src = 'class C:\n    act(self):\n        pass\nmain:\n    variable.';
  const items = completions(src, src.length);
  check('after `variable.`: class methods are not qualifiers', !findItem(items, 'act'));
}
{
  const items = completions('variable.gl', 11);
  check('partial qualifier `variable.gl` still offers global',
    !!findItem(items, 'global'));
}
{
  const items = completions('x = 1', 5);
  const v = findItem(items, 'variable');
  check('`variable` offered as contextual keyword', !!v
    && v.kind === fakeVscode.CompletionItemKind.Keyword);
  check('`variable` docs list all three forms', !!v
    && docText(v).indexOf('variable.global') !== -1
    && docText(v).indexOf('variable.parent') !== -1
    && docText(v).indexOf('variable.<fn>') !== -1);
  check('`variable` snippet inserts the qualified shape',
    !!v && snippetText(v) === 'variable.${1:global} ${2:name}');
  const asVar = items.find((i) => labelOf(i) === 'variable'
    && i.kind === fakeVscode.CompletionItemKind.Variable);
  check('`variable` not duplicated as a plain variable item', !asVar);
}
{
  const s = snippets.variable;
  check('snippet variable exists', !!s);
  if (s) {
    check('variable snippet prefix', s.prefix === 'variable');
    check('variable snippet first line',
      s.body[0] === 'variable.${1:global} ${2:name}$0');
    check('variable snippet has prefix/body/description shape',
      typeof s.prefix === 'string' && Array.isArray(s.body) && s.body.length > 0
      && typeof s.description === 'string' && s.description.length > 0);
    check('variable snippet description names the three forms + doc',
      /variable\.global/.test(s.description)
      && /variable\.parent/.test(s.description)
      && /cross-scope-access/.test(s.description));
  }
}
check('extension README documents variable.global', readme.indexOf('variable.global') !== -1);
check('extension README documents variable.parent', readme.indexOf('variable.parent') !== -1);
check('extension README documents variable.<fn>', readme.indexOf('variable.<fn>') !== -1);
check('extension source carries the cross-scope docs',
  extSrc.indexOf('VARQUAL_DOC') !== -1
  && extSrc.indexOf('variable.${1:global} ${2:name}') !== -1);

// ===========================================================================
section('0.2.1: Python-parity builtins (SPEC §5.11)');

// The 43 implemented entries + nine always-fail stubs + the rejected trio.
const PARITY_BUILTINS = [
  'abs', 'all', 'any', 'ascii', 'bin', 'bytes', 'callable', 'chr',
  'delattr', 'dict', 'dir', 'divmod', 'enumerate', 'filter', 'format',
  'getattr', 'globals', 'hasattr', 'hash', 'hex', 'id', 'isinstance',
  'iter', 'list', 'locals', 'map', 'max', 'min', 'next', 'oct', 'open',
  'ord', 'pow', 'repr', 'reversed', 'round', 'setattr', 'slice', 'sorted',
  'sum', 'tuple', 'vars', 'zip',
  'breakpoint', 'classmethod', 'help', 'issubclass', 'memoryview',
  'object', 'property', 'staticmethod', 'super',
  'eval', 'exec', 'compile',
];
check('parity set is 55 names', PARITY_BUILTINS.length === 55);

// Grammar: every name scoped as a builtin before `()`, never without one.
for (const name of PARITY_BUILTINS) {
  const tok = tokenize(`${name}(x)`).find((k) => k.t === name);
  check(`grammar: ${name}(call scoped as builtin)`, !!tok
    && hasScope(tok, 'support.function.pentest'));
}
check('grammar: bare `sorted` not builtin', !ruleMatches('builtin', 'sorted'));
check('grammar: `sortedx()` not builtin', !ruleMatches('builtin', 'sortedx()'));
check('grammar: `object()` not swallowed by `objectx` confusion',
  ruleMatches('builtin', 'object()') && !ruleMatches('builtin', 'objectx()'));
{
  const builtinRule = JSON.stringify(grammar.repository.builtin || {});
  check('builtin pattern lists the parity head (abs|all|any|ascii)',
    builtinRule.indexOf('abs|all|any|ascii') !== -1);
  check('builtin pattern lists the stubs (breakpoint|classmethod|help)',
    builtinRule.indexOf('breakpoint|classmethod|help') !== -1);
  check('builtin pattern lists the trio (eval|exec + compile)',
    builtinRule.indexOf('enumerate|eval|exec|filter') !== -1
    && builtinRule.indexOf('chr|compile|delattr') !== -1);
}

// Completions: Function kind, call-only/reserved detail, docs, snippet insert.
{
  const items = completions('x = 1', 5);
  for (const name of PARITY_BUILTINS) {
    const it = findItem(items, name);
    check(`completion: ${name} offered as builtin`, !!it
      && it.kind === fakeVscode.CompletionItemKind.Function);
    check(`completion: ${name} marked call-only/reserved`, !!it
      && /builtin function/.test(it.detail) && /reserved/.test(it.detail));
    check(`completion: ${name} documented`, !!it && docText(it).length > 0);
    check(`completion: ${name} snippet inserts ${name}(...)`, !!it
      && snippetText(it).indexOf(name + '(') === 0);
  }
}

// Reserved: never offered as a plain variable when it appears in the file.
{
  const src = PARITY_BUILTINS.map((w, i) => `${w} = ${i}`).join('\n') + '\n';
  const items = completions(src, src.length);
  for (const w of PARITY_BUILTINS) {
    const asVar = items.find((i) => labelOf(i) === w
      && i.kind === fakeVscode.CompletionItemKind.Variable);
    check(`reserved: ${w} not offered as variable`, !asVar);
  }
}

// The doc highlights users need to see (contract phrases).
{
  const items = completions('x = 1', 5);
  const docOf = (n) => { const it = findItem(items, n); return it ? docText(it) : ''; };
  check('super docs: always raises RuntimeError', /RuntimeError/.test(docOf('super')));
  check('eval docs: compile-time error', /compile-time/.test(docOf('eval')));
  check('open docs: FileNotFoundError + methods', /FileNotFoundError/.test(docOf('open'))
    && /readline/.test(docOf('open')));
  check('bytes docs: encoding required for text', /encoding/.test(docOf('bytes')));
  check('isinstance docs: exact type match', /exact/.test(docOf('isinstance')));
  check('slice docs: (start, stop, step) tuple', /\(start, stop, step\)/.test(docOf('slice')));
  check('divmod docs: truncating deviation', /truncat/.test(docOf('divmod')));
  check('dir docs: globals table deviation', /globals/.test(docOf('dir')));
  check('sorted docs: stable merge sort', /stable/.test(docOf('sorted')));
  check('next docs: StopIteration', /StopIteration/.test(docOf('next')));
}

// 0.2.1 snippets: open / sorted / isinstance.
{
  const expected = {
    'open-loop': { prefix: 'open', first: 'f = open(${1:path})' },
    'sorted': { prefix: 'sorted', first: '${1:result} = sorted(${2:iterable})$0' },
    'isinstance': { prefix: 'isinstance', first: 'if isinstance(${1:value}, ${2:type}):' },
  };
  for (const [key, exp] of Object.entries(expected)) {
    const s = snippets[key];
    check(`snippet ${key} exists`, !!s);
    if (!s) continue;
    check(`snippet ${key} prefix`, s.prefix === exp.prefix);
    check(`snippet ${key} first body line`, s.body[0] === exp.first);
    check(`snippet ${key} has prefix/body/description shape`,
      typeof s.prefix === 'string' && Array.isArray(s.body) && s.body.length > 0
      && typeof s.description === 'string' && s.description.length > 0);
    check(`snippet ${key} description names SPEC §5.11`, /SPEC §5\.11/.test(s.description));
    check(`snippet ${key} not offered as a variable`, !completions(s.prefix + ' = 1\n', 20)
      .find((i) => labelOf(i) === s.prefix && i.kind === fakeVscode.CompletionItemKind.Variable));
  }
}

// Source tables carry the whole set.
check('extension source reserves all 55 parity names',
  PARITY_BUILTINS.every((n) => extSrc.indexOf("'" + n + "'") !== -1));
check('extension source documents every parity builtin',
  PARITY_BUILTINS.every((n) => new RegExp("'" + n + "':\\s*\\{").test(extSrc)));

// README parity row.
check('README documents the Python-parity builtins', /Python-parity/.test(readme));
check('README lists sorted/isinstance/open',
  readme.indexOf('sorted') !== -1 && readme.indexOf('isinstance') !== -1
  && readme.indexOf('`open()`') !== -1);
check('README mentions the unavailable trio', /eval/.test(readme) && /compile/.test(readme));

// ===========================================================================
section('0.2.1: hover provider (docs on pointer-over)');

check('hover provider registered for pentest', !!captured.hoverProvider);

// Builtins: plain signature (placeholders stripped) + contract doc + footer.
{
  const h = hover('xs = sorted(xs\n)', 7);
  const t = hoverText(h);
  check('hover sorted: plain signature', t.indexOf('sorted(iterable)') !== -1);
  check('hover sorted: no snippet placeholders', t.indexOf('${') === -1);
  check('hover sorted: doc mentions stable merge sort', /stable merge sort/.test(t));
  check('hover sorted: footer names call-only + SPEC', /call-only/.test(t) && /SPEC §5\.11/.test(t));
  check('hover sorted: carries a range', !!h && !!h.range);
}
{
  const t = hoverText(hover('print("hi"\n)', 1));
  check('hover print: signature print(value)', t.indexOf('print(value)') !== -1);
}
{
  const t = hoverText(hover('super()\n', 1));
  check('hover stub super: always raises RuntimeError', /RuntimeError/.test(t));
  check('hover stub super: reason mentions inheritance', /inheritance/.test(t));
}
{
  const t = hoverText(hover('eval(src)\n', 1));
  check('hover eval: not available', /not available/.test(t));
  check('hover eval: compile-time error', /compile-time/.test(t));
}

// Keywords and constants.
{
  const t = hoverText(hover('main:\n    pass\n', 1));
  check('hover keyword main: entry-block doc', /entry block/.test(t));
  check('hover keyword True: bool literal', /bool literal/.test(hoverText(hover('x = True\n', 5))));
}

// The twelve builtin exception classes.
{
  const t = hoverText(hover('except TypeError as e:\n', 8));
  check('hover TypeError: wrong-type doc', /Wrong \*\*type\*\*/.test(t));
  check('hover TypeError: footer says exact class identity', /exact class identity/.test(t));
  check('hover TypeError: instances expose .message', /\.message/.test(t));
  check('hover FileNotFoundError: missing-path doc', /path that does not exist/.test(
    hoverText(hover('raise FileNotFoundError\n', 7))));
}
// SPEC §6.10 correction (known issue #5): neither class inherits or
// catches-all — matching is exact identity.
{
  const t = hoverText(hover('raise Exception("x")\n', 8));
  check('hover Exception: exact matching documented', /exact/.test(t));
  check('hover Exception: no inheritance claim', !/Base catchable/.test(t));
}
{
  const t = hoverText(hover('raise RuntimeError("x")\n', 8));
  check('hover RuntimeError: exact matching documented', /exact/.test(t));
  check('hover RuntimeError: no catch-all claim', !/catch-all/.test(t));
}

// Methods: right of a dot — per-type contract; bare identifier — nothing.
{
  const h = hover('xs.append(1\n)', 5);
  const t = hoverText(h);
  check('hover .append: method-of header', /Method of/.test(t));
  check('hover .append: list contract', /`list`/.test(t) && /append one value/.test(t));
  check('hover bare append: not a hover', !hover('append = 1\nprint(append\n)', 21));
}

// `variable.` qualifier form.
{
  const t = hoverText(hover('g = 0\ntick:\n    variable.global g += 1\n', 18));
  check('hover variable: qualifier doc lists variable.global', /variable\.global/.test(t));
  check('hover variable: lists variable.parent', /variable\.parent/.test(t));
}

// User functions and classes: declaration signature + line.
{
  const doc = 'greet(name):\n    print("hi", name\n\ngreet("you"\n))';
  const t = hoverText(hover(doc, 36));   // inside the greet call on line 4
  check('hover user function: declaration signature', t.indexOf('greet(name):') !== -1);
  check('hover user function: declared-at line', /Declared at line 1/.test(t));
}
{
  const doc = 'class Point:\n    __init__(self, x):\n        self.x = x\n\np = Point(1\n)';
  const t = hoverText(hover(doc, 63));
  check('hover user class: declaration signature', t.indexOf('class Point:') !== -1);
  check('hover user class: declared-at line', /Declared at line 1/.test(t));
}

// Plain identifiers have no hover.
{
  const src = 'foo = 1\nprint(foo\n)';
  check('hover plain local: none', !hover(src, src.indexOf('foo', 1)));
}

// README documents hover.
check('README documents Hover', /Pointer-over/.test(readme));

// ===========================================================================
// Report
// ===========================================================================

function finish() {
  sections.forEach((s, i) => {
    const end = i + 1 < sections.length ? sections[i + 1].at : total;
    const count = end - s.at;
    process.stdout.write(`  ${String(count).padStart(4)}  ${s.name}\n`);
  });

  process.stdout.write('\n');
  if (failures.length) {
    process.stdout.write('FAILURES:\n');
    for (const f of failures) process.stdout.write('  ✗ ' + f + '\n');
    process.stdout.write('\n');
  }
  process.stdout.write(
    `TOTAL: ${total} checks — ${total - failed} passed, ${failed} failed\n`
  );
  process.exit(failed ? 1 : 0);
}

// The dirty-run scenario resolves asynchronously (await doc.save()); every
// other check above has run synchronously by now.
if (late) late.then(finish, finish);
else finish();
