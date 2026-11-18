/* The Gateway's USB player on the Speakers page (gw_player.c serves this
 * file; ROADMAP_USB_PLAYER.md phase 5). As the radio: a Music button in the
 * header while it does not play, a player bar above the Room while it plays
 * (what plays, what each speaker gets, play/pause/next/prev/seek, volume),
 * and a Library panel with the USB drive's folders and what comes next. A
 * file played while AirPlay or the radio plays takes the speakers over. */
(function () {
'use strict';
const css = `
.p-bar{position:relative;overflow:hidden;margin-bottom:14px}
.p-bar::before{content:'';position:absolute;left:0;right:0;top:0;height:2px;background:var(--glow)}
.p-art{flex:none;width:52px;height:52px;border-radius:12px;background:var(--panel2);border:1px solid var(--line);display:flex;align-items:center;justify-content:center;font-weight:700;font-size:12px;letter-spacing:.5px;color:#c4b2ff}
.p-seek{display:flex;align-items:center;gap:10px;margin-top:12px}
.p-seek span{font-size:11px;color:var(--muted);font-variant-numeric:tabular-nums;min-width:38px}
.p-seek span:last-child{text-align:right}
.p-seek input{flex:1;width:auto;padding:0;border:none;background:none;accent-color:var(--accent)}
.p-btns{display:flex;gap:8px;align-items:center}
.p-ib{width:40px;height:40px;border-radius:50%;border:1px solid var(--line2);background:none;color:var(--text);font-size:15px;cursor:pointer;display:flex;align-items:center;justify-content:center;font-family:inherit;padding:0}
.p-ib:hover{border-color:#fff}
.p-ib.main{width:46px;height:46px;background:#fff;color:#000;border-color:#fff;font-size:17px}
.p-ib.main:hover{background:#dcdcdc}
.p-ib:disabled{opacity:.35;cursor:not-allowed}
.p-gets{display:flex;flex-wrap:wrap;gap:6px;margin-top:12px}
.p-get{font-size:11px;padding:4px 10px;border-radius:10px;background:#0a0a0b;border:1px solid var(--line);color:var(--text);white-space:nowrap}
.p-get b{font-weight:600;margin-right:6px}
.p-get small{color:var(--muted);margin-left:6px}
.p-path{display:flex;flex-wrap:wrap;gap:4px;align-items:center;font-size:12px;color:var(--muted);margin-top:10px}
.p-path button{background:none;border:none;color:var(--accent);font-size:12px;cursor:pointer;font-family:inherit;padding:2px 0}
.p-list{overflow-y:auto;margin-top:10px;-webkit-overflow-scrolling:touch;display:flex;flex-direction:column;gap:6px;padding-bottom:4px}
.p-row{display:flex;align-items:center;gap:10px;background:#0a0a0b;border:1px solid var(--line);border-radius:12px;padding:9px 10px;cursor:pointer;color:var(--text);text-align:left;font-family:inherit;font-size:13px;width:100%}
.p-row:hover{border-color:var(--line2)}
.p-row.on{box-shadow:0 0 0 2px #8b5cff;border-color:transparent}
.p-row.off{cursor:default;opacity:.38}
.p-row .ic{flex:none;width:30px;height:30px;border-radius:8px;background:var(--panel2);display:flex;align-items:center;justify-content:center;font-size:10px;font-weight:700;color:var(--muted);letter-spacing:.3px}
.p-row .nm{flex:1;min-width:0;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.p-row small{color:var(--muted);font-size:11px;white-space:nowrap}
.p-add{flex:none;background:none;border:1px solid var(--line2);border-radius:999px;color:var(--text);font-size:13px;width:28px;height:28px;cursor:pointer;padding:0;font-family:inherit}
.p-add:hover{border-color:#fff}
.p-tools{display:flex;gap:8px;align-items:center;margin-top:10px;flex-wrap:wrap}
`;
const el = (tag, cls, txt) => { const e = document.createElement(tag); if (cls) e.className = cls; if (txt !== undefined) e.textContent = txt; return e; };
const st = document.createElement('style'); st.textContent = css; document.head.appendChild(st);
const fetchJ = async (u, o) => { const r = await fetch(u, Object.assign({ cache: 'no-store' }, o)); if (!r.ok) throw new Error(await r.text() || r.status); return r.json(); };
const post = b => fetchJ('/api/player', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(b) });

/* ---- the header button, the bar, the panel (the radio's look) ---- */
const pill = el('button', 'r-pill', 'Music'); pill.type = 'button'; pill.style.display = 'none';
const nowBox = document.querySelector('.header .now'); if (nowBox) nowBox.appendChild(pill);

const bar = el('div', 'panel p-bar'); bar.style.display = 'none';
const art = el('div', 'p-art'), bName = el('div', 'r-name'), bSub = el('div', 'r-title'), bChips = el('div', 'r-chips');
const bTxt = el('div', 'r-txt'); bTxt.append(bName, bSub);
const row = el('div', 'r-row'); row.append(art, bTxt, bChips);
const pos = el('input'); pos.type = 'range'; pos.min = 0; pos.max = 1; pos.step = 0.1; pos.value = 0; pos.setAttribute('aria-label', 'Position');
const tPos = el('span', '', '0:00'), tDur = el('span', '', '0:00');
const seekRow = el('div', 'p-seek'); seekRow.append(tPos, pos, tDur);
const bPrev = el('button', 'p-ib', '⏮'), bPlay = el('button', 'p-ib main', '⏸'), bNext = el('button', 'p-ib', '⏭');
[bPrev, bPlay, bNext].forEach(b => { b.type = 'button'; });
bPrev.title = 'Previous (the start of this one after 3 s)'; bNext.title = 'Next'; bPlay.title = 'Pause';
const btns = el('div', 'p-btns'); btns.append(bPrev, bPlay, bNext);
const vol = el('input'); vol.type = 'range'; vol.min = -60; vol.max = 0; vol.step = 1; vol.setAttribute('aria-label', 'Player volume');
const volT = el('span'); const volBox = el('div', 'r-vol'); volBox.append(el('span', '', 'Volume'), vol, volT);
const bLib = el('button', 'btn sec', 'Library'), bStop = el('button', 'btn', 'Stop');
const ctl = el('div', 'r-ctl'); const gap = el('span'); gap.style.flex = '1';
ctl.append(btns, volBox, gap, bLib, bStop);
const gets = el('div', 'p-gets');
bar.append(row, seekRow, ctl, gets);
const room = document.querySelector('.wrap > .panel'); if (room) room.parentNode.insertBefore(bar, room);

const root = el('div'); document.body.appendChild(root);
requestAnimationFrame(() => requestAnimationFrame(() => root.classList.add('r-ready')));
const shade = el('div', 'r-shade'), sheet = el('div', 'r-sheet'); sheet.setAttribute('role', 'dialog'); sheet.setAttribute('aria-label', 'Music library');
root.append(shade, sheet);
const tabs = el('div', 'r-tabs'), x = el('button', 'r-x', 'Close ✕');
const head = el('div', 'r-head'); head.append(tabs, x);
const msg = el('div', 'r-msg'), path = el('div', 'p-path'), tools = el('div', 'p-tools'), list = el('div', 'p-list');
sheet.append(el('div', 'r-grip'), head, msg, path, tools, list);
let tab = 'usb';
try { tab = localStorage.getItem('playerTab') || 'usb'; } catch (e) {}
[['usb', 'USB'], ['next', 'Up next']].forEach(([k, t]) => { const b = el('button', 'r-tab', t); b.type = 'button'; b.dataset.k = k; b.onclick = () => show(k); tabs.appendChild(b); });

/* ---- state ---- */
let cur = null, dir = '/', entries = [], speakers = [], dragging = false, seeking = false, pollT = null;
let lastKib = {}, lastT = 0, kbps = {};
try { dir = localStorage.getItem('playerDir') || '/'; } catch (e) {}
const ROLE_IX = { mix: -1, L: 0, R: 1, C: 2, LFE: 3, Ls: 4, Rs: 5, Lrs: 6, Rrs: 7 };
const ROLE_NAME = { mix: 'Mono mix', L: 'Left', R: 'Right', C: 'Centre', LFE: 'Subwoofer', Ls: 'Side L', Rs: 'Side R', Lrs: 'Rear L', Rrs: 'Rear R' };
const mmss = s => { s = Math.max(0, Math.floor(s || 0)); const h = Math.floor(s / 3600), m = Math.floor(s / 60) % 60, ss = String(s % 60).padStart(2, '0'); return h ? h + ':' + String(m).padStart(2, '0') + ':' + ss : m + ':' + ss; };
const baseName = u => { const n = (u || '').split('/').pop() || u || ''; return n.replace(/\.[^.]+$/, ''); };
const folderOf = u => { const p = (u || '').replace(/^usb:/, '').split('/'); p.pop(); return p.join('/') || '/'; };
const join = (d, n) => (d.endsWith('/') ? d : d + '/') + n;
function setMsg(t, err) { msg.textContent = t || ''; msg.className = 'r-msg' + (err ? ' err' : ''); }
function chip(t, c) { return el('span', 'chip ' + c, t); }

/* ---- the drive ---- */
async function openDir(d) {
  dir = d; try { localStorage.setItem('playerDir', d); } catch (e) {}
  if (tab !== 'usb') return;
  list.textContent = ''; list.appendChild(el('div', 'r-empty', 'Reading the drive…'));
  let r;
  try { r = await fetchJ('/api/player/files?path=' + encodeURIComponent(d)); } catch (e) { list.textContent = ''; list.appendChild(el('div', 'r-empty', 'The Gateway did not answer.')); return; }
  if (r.usb !== 'ready') { entries = []; drawPath(); tools.textContent = ''; list.textContent = ''; list.appendChild(el('div', 'r-empty', r.usb === 'not FAT32' ? 'The drive is not FAT32 (exFAT, NTFS, APFS and ext4 are not read).' : r.usb === 'no drive' ? 'Plug a FAT32 USB drive into the Gateway.' : 'The USB drive: ' + r.usb)); return; }
  if (r.error && d !== '/') { openDir('/'); return; }
  entries = r.entries || [];
  drawPath(); drawTools(r.more); drawList();
}
function drawPath() {
  path.textContent = '';
  if (tab !== 'usb') return;
  const parts = dir.split('/').filter(Boolean);
  const mk = (t, d) => { const b = el('button', '', t); b.type = 'button'; b.onclick = () => openDir(d); return b; };
  path.appendChild(mk('USB', '/'));
  let acc = '';
  parts.forEach(p => { acc += '/' + p; path.append(el('span', '', '›'), mk(p, acc)); });
}
const playable = () => entries.filter(e => !e.dir && e.play);
function drawTools(more) {
  tools.textContent = '';
  if (tab !== 'usb') return;
  const n = playable().length;
  if (n) {
    const b = el('button', 'btn', 'Play folder (' + n + ')'); b.type = 'button'; b.onclick = () => playFrom(0);
    tools.appendChild(b);
  }
  if (more) tools.appendChild(el('span', 'msg', 'The first 400 shown.'));
}
const sizeOf = b => b >= 1e9 ? (b / 1e9).toFixed(1) + ' GB' : b >= 1e6 ? (b / 1e6).toFixed(1) + ' MB' : Math.round(b / 1e3) + ' kB';
function drawList() {
  list.textContent = '';
  if (!entries.length) { list.appendChild(el('div', 'r-empty', 'Nothing here.')); return; }
  const playing = cur && cur.active ? cur.url : '';
  entries.forEach(e => {
    const r = el('div', 'p-row'); r.tabIndex = 0;
    const ic = el('div', 'ic', e.dir ? '▸' : (e.type || '·').toUpperCase().slice(0, 4));
    const nm = el('div', 'nm', e.dir ? e.name : e.name);
    r.append(ic, nm);
    if (e.dir) {
      r.onclick = () => openDir(join(dir, e.name));
    } else if (e.play) {
      r.append(el('small', '', sizeOf(e.size || 0)));
      const add = el('button', 'p-add', '+'); add.type = 'button'; add.title = 'Add to Up next';
      add.onclick = ev => { ev.stopPropagation(); addOne(e.name); };
      r.append(add);
      const url = 'usb:' + join(dir, e.name);
      if (url === playing) r.classList.add('on');
      r.onclick = () => playFrom(playable().indexOf(e));
    } else {
      r.classList.add('off'); r.title = 'Not a file the player takes';
      r.append(el('small', '', sizeOf(e.size || 0)));
    }
    r.onkeydown = ev => { if (ev.key === 'Enter') { ev.preventDefault(); r.click(); } };
    list.appendChild(r);
  });
}
/* The folder's playable files from the one tapped on (the queue takes 64). */
async function playFrom(i) {
  const files = playable(); if (i < 0 || !files.length) return;
  const first = Math.max(0, Math.min(i, files.length - 1));
  const urls = files.slice(first, first + 64).map(e => 'usb:' + join(dir, e.name));
  setMsg((cur && cur.active ? 'Switching to ' : 'Starting ') + files[first].name + '…');
  try { render(await post({ cmd: 'play', urls, index: 0 })); close(); } catch (e) { setMsg(String(e.message || e), true); } /* the Gateway keeps the volume */
  poll(500);
}
async function addOne(name) {
  const url = 'usb:' + join(dir, name);
  try {
    if (cur && cur.active) { render(await post({ cmd: 'add', url })); setMsg('Added: ' + name); }
    else { render(await post({ cmd: 'play', url })); close(); }
  } catch (e) { setMsg(String(e.message || e), true); }
}

/* ---- up next ---- */
function drawQueue() {
  path.textContent = ''; tools.textContent = ''; list.textContent = '';
  const q = (cur && cur.queue) || [];
  if (!q.length || !(cur && cur.active)) { list.appendChild(el('div', 'r-empty', 'Nothing plays. Pick a file or a folder on the USB tab.')); return; }
  q.forEach((u, k) => {
    const r = el('div', 'p-row'); r.tabIndex = 0;
    r.append(el('div', 'ic', String(k + 1)), el('div', 'nm', (u || '').split('/').pop()), el('small', '', folderOf(u)));
    if (k === cur.index) r.classList.add('on');
    r.onclick = async () => { try { render(await post({ cmd: 'jump', index: k })); } catch (e) { setMsg(String(e.message || e), true); } };
    list.appendChild(r);
  });
}
function show(k) {
  tab = k; try { localStorage.setItem('playerTab', k); } catch (e) {}
  [...tabs.children].forEach(b => b.classList.toggle('on', b.dataset.k === k));
  setMsg(cur && cur.state === 'failed' && cur.message ? 'Last file: ' + cur.message : '', true);
  if (k === 'usb') openDir(dir); else drawQueue();
}
function open() { root.classList.add('r-open'); document.body.style.overflow = 'hidden'; show(tab); }
function close() { root.classList.remove('r-open'); document.body.style.overflow = ''; }
pill.onclick = open; bLib.onclick = open; x.onclick = close; shade.onclick = close;
document.addEventListener('keydown', e => { if (e.key === 'Escape' && root.classList.contains('r-open')) close(); });

/* ---- the controls ---- */
const act = async (b) => { try { render(await post(b)); } catch (e) {} poll(400); };
bStop.onclick = () => act({ cmd: 'stop' });
bNext.onclick = () => act({ cmd: 'next' });
bPrev.onclick = () => act({ cmd: 'prev' });
bPlay.onclick = () => act({ cmd: cur && cur.state === 'paused' ? 'resume' : 'pause' });
pos.oninput = () => { seeking = true; tPos.textContent = mmss(+pos.value); };
pos.onchange = () => { act({ cmd: 'seek', pos: +pos.value }); setTimeout(() => { seeking = false; }, 1500); };
let volT0 = 0, volTimer = null;
const sendVol = () => post({ cmd: 'volume', vol: +vol.value }).catch(() => {});
vol.oninput = () => { dragging = true; volT.textContent = vol.value + ' dB'; const n = Date.now(); clearTimeout(volTimer); if (n - volT0 > 250) { volT0 = n; sendVol(); } else volTimer = setTimeout(sendVol, 250); };
vol.onchange = () => { clearTimeout(volTimer); sendVol(); setTimeout(() => { dragging = false; }, 600); };

/* ---- what the Gateway says ---- */
function laneRates(d) {
  const t = Date.now() / 1000;
  (d.lanes || []).forEach(l => {
    const k = l.role, prev = lastKib[k];
    if (prev !== undefined && t > lastT && l.sent_kib >= prev) {
      const r = (l.sent_kib - prev) * 8.192 / (t - lastT); /* KiB to kbit */
      kbps[k] = kbps[k] === undefined ? r : kbps[k] * 0.6 + r * 0.4;
    }
    lastKib[k] = l.sent_kib;
  });
  lastT = t;
}
function drawGets(d) {
  gets.textContent = '';
  (d.lanes || []).forEach(l => {
    const ix = ROLE_IX[l.role];
    const who = speakers.filter(s => s.channel === ix).map(s => s.name || s.ip);
    const g = el('span', 'p-get');
    g.append(el('b', '', ROLE_NAME[l.role] || l.role), document.createTextNode(who.join(', ') || '—'));
    const r = d.state === 'playing' ? kbps[l.role] : undefined; /* paused: nothing goes */
    g.append(el('small', '', 'ALAC ' + l.bits + (r !== undefined && r > 1 ? ' · ' + (r >= 1000 ? (r / 1000).toFixed(2) + ' Mbit/s' : Math.round(r) + ' kbit/s') : '')));
    gets.appendChild(g);
  });
}
function render(d) {
  cur = d;
  const active = d.active || ['starting', 'playing', 'buffering', 'paused', 'finishing', 'stopping'].includes(d.state);
  pill.style.display = active ? 'none' : '';
  pill.classList.toggle('err', d.state === 'failed');
  pill.title = d.state === 'failed' && d.message ? 'Last file: ' + d.message : 'Play music from the USB drive';
  bar.style.display = active ? '' : 'none';
  if (root.classList.contains('r-open') && tab === 'next') drawQueue();
  if (!active) { lastKib = {}; kbps = {}; return; }
  const fmt = d.format || '';
  art.textContent = (fmt.split(' ')[0] || '♪').slice(0, 4);
  bName.textContent = baseName(d.url) || 'Starting…';
  const q = d.queue || [];
  const where = q.length > 1 ? (d.index + 1) + ' of ' + q.length + ' · ' : '';
  bSub.textContent = d.state === 'failed' || (d.message && d.state !== 'playing') ? (d.message || '') : where + folderOf(d.url);
  bChips.textContent = '';
  const m = /^(.*?) (\d+) Hz \/ (\d+) bit \/ (\S+?)(?:, \/(\d) to (\d+))?$/.exec(fmt);
  if (m) {
    const k = r => (r % 1000 ? (r / 1000).toFixed(1) : r / 1000);
    bChips.appendChild(chip(m[1] + ' ' + k(+m[2]) + '/' + m[3] + (m[5] ? ' → ' + k(+m[6]) + '/24' : ''), 'c-fmt'));
    bChips.appendChild(chip(m[4] === '5.1' ? '5.1' : m[4] === '7.1' ? '7.1' : m[4] === 'mono' ? 'Mono' : 'Stereo', m[4] === '7.1' ? 'c-71' : m[4] === '5.1' ? 'c-51' : 'c-st'));
    /* the lanes carry the decoder's samples as they are: bit for bit with
     * the file only where the file is lossless */
    const lossless = /^(FLAC|WAV|ALAC)/.test(m[1]);
    if (m[5]) bChips.appendChild(chip('down ÷' + m[5], 'c-dim'));
    else if (lossless) bChips.appendChild(chip('bit for bit', 'c-ok'));
  }
  if (d.state !== 'playing') bChips.appendChild(chip(d.state === 'paused' ? 'Paused' : d.state === 'buffering' ? 'Buffering…' : d.state === 'finishing' ? 'Ending…' : d.state === 'stopping' ? 'Stopping…' : 'Starting…', 'c-warn'));
  bPlay.textContent = d.state === 'paused' ? '▶' : '⏸'; bPlay.title = d.state === 'paused' ? 'Play' : 'Pause';
  bNext.disabled = !(q.length > d.index + 1);
  const dur = d.duration_s || 0;
  pos.max = dur > 0 ? dur : 1; pos.disabled = !(dur > 0);
  if (!seeking) { pos.value = Math.min(d.position_s || 0, pos.max); tPos.textContent = mmss(d.position_s); }
  tDur.textContent = dur > 0 ? (d.duration_estimated ? '~' : '') + mmss(dur) : '–:––';
  if (!dragging) { vol.value = d.volume_db; volT.textContent = d.volume_db + ' dB'; }
  laneRates(d); drawGets(d);
}
async function loadSpeakers() {
  try { const s = await fetchJ('/api/speakers'); speakers = (s && s.speakers) || []; } catch (e) {}
}
async function poll(ms) {
  clearTimeout(pollT);
  pollT = setTimeout(async () => {
    let d = null; try { d = await fetchJ('/api/player'); render(d); } catch (e) {}
    poll(d && (d.active || ['starting', 'buffering', 'stopping'].includes(d.state)) ? 1000 : 3000);
  }, ms);
}
loadSpeakers(); setInterval(loadSpeakers, 10000); poll(0);
})();
