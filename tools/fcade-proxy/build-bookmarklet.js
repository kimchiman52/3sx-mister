#!/usr/bin/env node
// Rebuilds the one-line bookmarklet form of browser-catalog.js.
//
// Run: node build-bookmarklet.js
// Writes browser-catalog.bookmarklet.txt next to this script.
//
// browser-catalog.js is the single source of truth (readable, commented,
// meant to be pasted into DevTools console directly). This script does a
// mechanical minify -- strip `//` comments and blank lines, collapse to
// one line, wrap as a `javascript:` URI -- so the SAME logic can also be
// saved as a browser bookmark and clicked instead of pasted. Re-run this
// after any edit to browser-catalog.js; do not hand-edit the .txt output.
//
// Safe because browser-catalog.js never puts a literal `//` inside a
// string/template literal (only relative paths like '/api/', one slash).
// If that ever stops being true, this naive comment-stripper would need to
// get smarter (or the code would need reformatting to avoid it).

'use strict';

const fs = require('fs');
const path = require('path');

const srcPath = path.join(__dirname, 'browser-catalog.js');
const outPath = path.join(__dirname, 'browser-catalog.bookmarklet.txt');

const src = fs.readFileSync(srcPath, 'utf8');

const minified = src
    .split('\n')
    .map((line) => {
        const idx = line.indexOf('//');
        return idx === -1 ? line : line.slice(0, idx);
    })
    .map((line) => line.trim())
    .filter((line) => line.length > 0)
    .join(' ');

// `void 0` at the end: a `javascript:` URI navigates the page to the
// string representation of its completion value, if that value is a
// string. Our IIFE evaluates to a Promise (not a string) already, so this
// is a defence-in-depth belt-and-suspenders, not a fix for an observed bug.
const bookmarklet = `javascript:${minified} void 0;`;

fs.writeFileSync(outPath, bookmarklet + '\n', 'utf8');
console.log(`wrote ${outPath} (${bookmarklet.length} chars)`);
