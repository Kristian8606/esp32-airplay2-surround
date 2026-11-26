#!/usr/bin/env node
/* Test of web/rew.js's REW filter import (node tools/test_rew_parse.js).
 *
 * rew.js runs in a stub page; its parser turns REW's (RBJ Cookbook) filters
 * into the speaker's biquads. For every shelf and peak below, the speaker's
 * biquad (spkBq) must match RBJ's own over 20 Hz - 20 kHz at 48 kHz within
 * 0.01 dB. Also: the decimal comma, unknown types and empty slots. */
'use strict';
const fs = require('fs');
const path = require('path');
const vm = require('vm');

/* A page with nothing in it: every element is a stub that takes any call. */
function stub() {
  const fn = function () { return stub(); };
  return new Proxy(fn, {
    get(t, k) {
      if (k === Symbol.toPrimitive) return () => '';
      if (k === 'length') return 0;
      if (k === 'forEach') return () => {};
      if (k in t) return t[k];
      return stub();
    },
    set(t, k, v) { t[k] = v; return true; },
    apply() { return stub(); },
  });
}
const window = {};
const document = stub();
document.querySelectorAll = () => [];
document.querySelector = () => null;
document.getElementById = () => null;
const sandbox = {
  window, document, console, Math, Number, String, Array, Object, JSON, Promise,
  fetch: () => Promise.reject(new Error('no network in the test')),
  setTimeout: () => 0, clearTimeout: () => {}, IntersectionObserver: function () { return { observe() {} }; },
};
vm.createContext(sandbox);
vm.runInContext(fs.readFileSync(path.join(__dirname, '..', 'web', 'rew.js'), 'utf8'), sandbox);
const { parseRew, spkBq, bqDb, FS } = window.rewTest;

/* RBJ Audio EQ Cookbook: peaking EQ and shelves with S = 1 (Q = 1/sqrt 2). */
function rbj(type, f0, g, q, fs) {
  const A = Math.pow(10, g / 40), w = 2 * Math.PI * f0 / fs, c = Math.cos(w);
  const al = Math.sin(w) / (2 * q);
  let b, a;
  if (type === 'PK') {
    b = [1 + al * A, -2 * c, 1 - al * A]; a = [1 + al / A, -2 * c, 1 - al / A];
  } else {
    const s = 2 * Math.sqrt(A) * al;
    if (type === 'LS') {
      b = [A * ((A + 1) - (A - 1) * c + s), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - s)];
      a = [(A + 1) + (A - 1) * c + s, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - s];
    } else {
      b = [A * ((A + 1) + (A - 1) * c + s), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - s)];
      a = [(A + 1) - (A - 1) * c + s, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - s];
    }
  }
  return [b[0] / a[0], b[1] / a[0], b[2] / a[0], a[1] / a[0], a[2] / a[0]];
}
function maxDiff(k1, k2) {
  let d = 0;
  for (let i = 0; i <= 600; ++i) {
    const f = 20 * Math.pow(1000, i / 600);
    d = Math.max(d, Math.abs(bqDb(k1, f, FS) - bqDb(k2, f, FS)));
  }
  return d;
}

let failed = 0, n = 0;
function check(name, ok, detail) {
  n++;
  if (!ok) { failed++; console.log('FAIL ' + name + (detail ? ': ' + detail : '')); }
}

for (const t of ['LS', 'HS'])
  for (const g of [3, -3, 6, -6, 12, -12])
    for (const fc of [40, 300, 2000, 5000, 10000]) {
      const r = parseRew(`Filter 1: ON ${t} Fc ${fc} Hz Gain ${g} dB`);
      const f = r.filters[0] && r.filters[0].f;
      if (!f) { check(`${t} ${fc} ${g}`, false, 'not parsed'); continue; }
      const d = maxDiff(spkBq(f.type, f.frequency_hz, f.gain_db, f.q, FS), rbj(t, fc, g, Math.SQRT1_2, FS));
      check(`${t} ${fc} Hz ${g} dB`, d <= 0.01, d.toFixed(4) + ' dB');
    }
for (const g of [3, -3, 12, -12])
  for (const q of [0.5, 1, 2, 5, 10])
    for (const fc of [40, 1000, 10000]) {
      const f = parseRew(`Filter 1: ON PK Fc ${fc} Hz Gain ${g} dB Q ${q}`).filters[0].f;
      const d = maxDiff(spkBq('PK', f.frequency_hz, f.gain_db, f.q, FS), rbj('PK', fc, g, q, FS));
      check(`PK ${fc} Hz ${g} dB Q ${q}`, d <= 0.01, d.toFixed(4) + ' dB');
    }

/* The decimal comma (a European REW export), next to the point. */
const comma = parseRew('Preamp: -6,5 dB\nFilter  1: ON  PK  Fc  180,5 Hz  Gain  -3,40 dB  Q  4,905');
check('decimal comma: one filter', comma.filters.length === 1 && comma.errors.length === 0, JSON.stringify(comma.errors));
check('decimal comma: values', comma.filters[0] && comma.filters[0].src.fc === 180.5 && comma.filters[0].src.g === -3.4 &&
      comma.filters[0].src.q === 4.905 && comma.preamp === -6.5, JSON.stringify(comma.filters[0] && comma.filters[0].src));
const point = parseRew('Filter 1: ON PK Fc 180.5 Hz Gain -3.40 dB Q 4.905');
check('decimal point still works', point.filters.length === 1 && point.filters[0].src.fc === 180.5);
/* A comma that is not between digits is left alone. */
const notes = parseRew('Notes: speaker 1, position 2\nFilter 1: ON PK Fc 100 Hz Gain -3 dB Q 2');
check('a comma in a note', notes.filters.length === 1 && notes.errors.length === 0);

/* Unknown types and empty slots. */
const misc = parseRew('Filter 1: ON AP Fc 900 Hz Q 0.707\nFilter 2: ON None\nFilter 3: OFF PK Fc 100 Hz Gain 3 dB Q 1');
check('AP refused on its line', misc.errors.length === 1 && misc.errors[0].n === 1);
check('None and OFF skipped', misc.filters.length === 0);

console.log(failed ? `${failed} of ${n} checks failed` : `all ${n} checks passed`);
process.exit(failed ? 1 : 0);
