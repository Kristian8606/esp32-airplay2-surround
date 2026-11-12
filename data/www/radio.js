/* Web radio controls for the Speakers page (gw_radio.c serves this file).
 * Adds a Radio button to the header while nothing plays, a player bar above
 * the Room while the radio plays, and a Stations panel. While AirPlay plays
 * none of it shows. Station search: Radio Browser (radio-browser.info),
 * straight from this browser; the Gateway keeps only the favourites. */
(function () {
'use strict';
const RB = ['de1', 'fi1', 'nl1', 'de2'].map(h => 'https://' + h + '.api.radio-browser.info');
const CODECS = /^(MP3|AAC|AAC\+)$/i;
const css = `
.r-pill{font-size:12px;color:var(--text);border:1px solid var(--line2);border-radius:999px;padding:7px 16px;background:none;font-family:inherit;cursor:pointer;align-self:center;position:relative}
.r-pill:hover{border-color:#fff}
.r-pill.err::after{content:'';position:absolute;top:2px;right:2px;width:8px;height:8px;border-radius:50%;background:#ff8a80}
.r-bar{position:relative;overflow:hidden;margin-bottom:14px}
.r-bar::before{content:'';position:absolute;left:0;right:0;top:0;height:2px;background:var(--glow)}
.r-row{display:flex;align-items:center;gap:12px;flex-wrap:wrap}
.r-logo{flex:none;width:52px;height:52px;border-radius:12px;background:var(--panel2);border:1px solid var(--line);overflow:hidden;display:flex;align-items:center;justify-content:center;font-weight:700;font-size:15px;letter-spacing:.5px}
.r-logo img{width:100%;height:100%;object-fit:cover;background:#fff}
.r-txt{flex:1;min-width:150px}
.r-name{font-size:16px;font-weight:600;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.r-title{font-size:12px;color:var(--muted);margin-top:3px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;min-height:15px}
.r-chips{display:flex;gap:6px;flex-wrap:wrap}
.r-ctl{display:flex;align-items:center;gap:10px;margin-top:14px;flex-wrap:wrap}
.r-vol{display:flex;align-items:center;gap:10px;flex:1;min-width:200px;max-width:360px}
.r-vol span{font-size:11px;color:var(--muted);white-space:nowrap;min-width:44px}
.r-vol input{flex:1;width:auto;padding:0;border:none;background:none;accent-color:var(--accent)}
.r-shade{position:fixed;inset:0;background:rgba(0,0,0,.55);opacity:0;pointer-events:none;transition:opacity .25s;z-index:50}
.r-sheet{position:fixed;left:0;right:0;bottom:0;max-height:84vh;display:flex;flex-direction:column;background:var(--panel);border-top:1px solid var(--line2);border-radius:20px 20px 0 0;padding:10px 16px 18px;transform:translateY(105%);visibility:hidden;z-index:51}
.r-ready .r-sheet{transition:transform .28s ease,visibility 0s linear .28s}
.r-open .r-shade{opacity:1;pointer-events:auto}
.r-open .r-sheet{transform:none;visibility:visible}
.r-ready.r-open .r-sheet{transition:transform .28s ease,visibility 0s}
.r-grip{width:38px;height:4px;border-radius:2px;background:var(--line2);margin:0 auto 12px}
.r-head{display:flex;justify-content:space-between;align-items:center;gap:10px;flex-wrap:wrap}
.r-tabs{display:flex;gap:6px;flex-wrap:wrap}
.r-tab{font-size:12px;padding:7px 14px;border-radius:999px;border:1px solid var(--line2);background:none;color:var(--text);font-family:inherit;cursor:pointer}
.r-tab.on{background:#fff;color:#000;border-color:#fff;font-weight:600}
.r-x{background:none;border:none;color:var(--muted);font-size:13px;cursor:pointer;font-family:inherit}
.r-x:hover{color:var(--text)}
.r-msg{font-size:12px;color:var(--muted);margin-top:10px;min-height:0}
.r-msg.err{color:#ff8a80}
.r-form{margin-top:10px;display:grid;gap:8px}
.r-list{overflow-y:auto;margin-top:12px;-webkit-overflow-scrolling:touch;display:grid;grid-template-columns:repeat(auto-fill,minmax(132px,1fr));gap:10px;padding-bottom:4px}
.r-t{position:relative;background:#0a0a0b;border:1px solid var(--line);border-radius:14px;padding:12px;display:flex;flex-direction:column;gap:7px;cursor:pointer;text-align:left;color:var(--text);font-family:inherit;transition:border-color .2s}
.r-t:hover{border-color:var(--line2)}
.r-t.on{box-shadow:0 0 0 2px #8b5cff;border-color:transparent}
.r-t .r-logo{width:42px;height:42px;font-size:12px;border-radius:10px}
.r-t b{font-size:13px;font-weight:500;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.r-t small{font-size:11px;color:var(--muted);white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.r-star{position:absolute;top:6px;right:6px;background:none;border:none;font-size:16px;line-height:1;color:var(--muted);cursor:pointer;padding:4px}
.r-star.on{color:#f7c46c}
.r-empty{grid-column:1/-1;text-align:center;color:var(--muted);font-size:13px;padding:26px 10px;line-height:1.6}
@media(min-width:760px){.r-sheet{left:auto;top:0;width:460px;max-height:none;border-top:none;border-left:1px solid var(--line2);border-radius:20px 0 0 20px;padding:20px;transform:translateX(105%)}.r-grip{display:none}}
`;
const el = (tag, cls, txt) => { const e = document.createElement(tag); if (cls) e.className = cls; if (txt !== undefined) e.textContent = txt; return e; };
const st = document.createElement('style'); st.textContent = css; document.head.appendChild(st);

/* ---- logos: the station's own, initials where it has none ---- */
const HUES = [['rgba(255,61,127,.16)', '#ff8fb5'], ['rgba(64,120,255,.16)', '#9dbaff'], ['rgba(40,200,200,.14)', '#7fe0e0'], ['rgba(255,150,60,.16)', '#ffbd85'], ['rgba(139,92,255,.18)', '#c4b2ff'], ['rgba(48,209,88,.14)', '#7ee2a0']];
function initials(n) { const w = (n || '?').replace(/[^\p{L}\p{N} ]/gu, ' ').trim().split(/\s+/); return ((w[0] || '?')[0] + (w[1] ? w[1][0] : (w[0] || '')[1] || '')).toUpperCase(); }
function logoInto(box, name, logo) {
  box.textContent = '';
  let h = 0; for (const c of name || '') h = (h * 31 + c.charCodeAt(0)) >>> 0;
  const [bg, fg] = HUES[h % HUES.length];
  const fall = () => { box.textContent = initials(name); box.style.background = bg; box.style.color = fg; };
  if (logo && /^https?:\/\//i.test(logo)) {
    const i = el('img'); i.alt = ''; i.loading = 'lazy'; i.referrerPolicy = 'no-referrer';
    i.onerror = () => { i.remove(); fall(); };
    i.src = logo; box.style.background = ''; box.appendChild(i);
  } else fall();
}

/* ---- the header button, the bar, the panel ---- */
const pill = el('button', 'r-pill', 'Radio'); pill.type = 'button'; pill.style.display = 'none';
const now = document.querySelector('.header .now'); if (now) now.appendChild(pill);

const bar = el('div', 'panel r-bar'); bar.style.display = 'none';
const bLogo = el('div', 'r-logo'), bName = el('div', 'r-name'), bTitle = el('div', 'r-title'), bChips = el('div', 'r-chips');
const bTxt = el('div', 'r-txt'); bTxt.append(bName, bTitle);
const row = el('div', 'r-row'); row.append(bLogo, bTxt, bChips);
const vol = el('input'); vol.type = 'range'; vol.min = -60; vol.max = 0; vol.step = 1; vol.setAttribute('aria-label', 'Radio volume');
const volT = el('span'); const volBox = el('div', 'r-vol'); volBox.append(el('span', '', 'Volume'), vol, volT);
const bSt = el('button', 'btn sec', 'Stations'), bStop = el('button', 'btn', 'Stop');
const ctl = el('div', 'r-ctl'); ctl.append(volBox, el('span', '', ''), bSt, bStop); ctl.children[1].style.flex = '1';
bar.append(row, ctl);
const room = document.querySelector('.wrap > .panel'); if (room) room.parentNode.insertBefore(bar, room);

const root = el('div'); document.body.appendChild(root);
requestAnimationFrame(() => requestAnimationFrame(() => root.classList.add('r-ready'))); /* no slide at load */
const shade = el('div', 'r-shade'), sheet = el('div', 'r-sheet'); sheet.setAttribute('role', 'dialog'); sheet.setAttribute('aria-label', 'Radio stations');
root.append(shade, sheet);
const tabs = el('div', 'r-tabs'), x = el('button', 'r-x', 'Close ✕');
const head = el('div', 'r-head'); head.append(tabs, x);
const msg = el('div', 'r-msg'), form = el('div', 'r-form'), list = el('div', 'r-list');
sheet.append(el('div', 'r-grip'), head, msg, form, list);
const TABS = [['fav', 'Favourites'], ['bg', 'Bulgaria'], ['search', 'Search'], ['url', 'URL']];
let tab = 'fav';
try { tab = localStorage.getItem('radioTab') || 'fav'; } catch (e) {}
TABS.forEach(([k, t]) => { const b = el('button', 'r-tab', t); b.type = 'button'; b.dataset.k = k; b.onclick = () => show(k); tabs.appendChild(b); });

const q = el('input'); q.type = 'search'; q.placeholder = 'Station name or genre'; q.setAttribute('aria-label', 'Search stations');
const uName = el('input'); uName.placeholder = 'Name (optional)';
const uUrl = el('input'); uUrl.type = 'url'; uUrl.placeholder = 'http://… stream address (MP3 or AAC)';
const uPlay = el('button', 'btn', 'Play'), uSave = el('button', 'btn sec', '★ Save');
const uBtns = el('div', 'r-ctl'); uBtns.style.marginTop = '2px'; uBtns.append(uPlay, uSave);

/* ---- state ---- */
let favs = [], cur = null, lastUrl = '', dragging = false, pollT = null, bgCache = null, searchT = null;
const fetchJ = async (u, o) => { const r = await fetch(u, Object.assign({ cache: 'no-store' }, o)); if (!r.ok) throw new Error(await r.text() || r.status); return r.json(); };
async function rb(path) {
  for (const base of RB) {
    const ac = new AbortController(), t = setTimeout(() => ac.abort(), 7000);
    try { const r = await fetch(base + path, { signal: ac.signal }); clearTimeout(t); if (r.ok) return { data: await r.json(), base }; } catch (e) { clearTimeout(t); }
  }
  throw new Error('offline');
}
const toStation = s => ({ name: (s.name || '').trim(), url: s.url_resolved || s.url, logo: s.favicon || '', cc: s.countrycode || '', codec: (s.codec || '').toUpperCase(), br: s.bitrate || 0, uuid: s.stationuuid || '', hls: s.hls === 1 });
const usable = s => s.url && /^https?:\/\//i.test(s.url) && CODECS.test(s.codec) && !s.hls;
const isFav = u => favs.some(f => f.url === u);

function setMsg(t, err) { msg.textContent = t || ''; msg.className = 'r-msg' + (err ? ' err' : ''); }
function tile(s) {
  const b = el('div', 'r-t'); b.tabIndex = 0; b.setAttribute('role', 'button');
  const lg = el('div', 'r-logo'); logoInto(lg, s.name, s.logo);
  const meta = [s.cc, s.codec, s.br ? s.br : ''].filter(Boolean).join(' · ');
  const star = el('button', 'r-star' + (isFav(s.url) ? ' on' : ''), isFav(s.url) ? '★' : '☆');
  star.type = 'button'; star.title = isFav(s.url) ? 'Remove from favourites' : 'Add to favourites';
  star.onclick = e => { e.stopPropagation(); toggleFav(s); };
  b.append(star, lg, el('b', '', s.name || s.url), el('small', '', meta || ' '));
  if (cur && cur.url === s.url) b.classList.add('on');
  const go = () => play(s);
  b.onclick = go; b.onkeydown = e => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); go(); } };
  return b;
}
function fill(arr, empty) {
  list.textContent = '';
  if (!arr.length) { list.appendChild(el('div', 'r-empty', empty)); return; }
  arr.forEach(s => list.appendChild(tile(s)));
}
async function show(k) {
  tab = k; try { localStorage.setItem('radioTab', k); } catch (e) {}
  [...tabs.children].forEach(b => b.classList.toggle('on', b.dataset.k === k));
  form.textContent = ''; setMsg(cur && cur.state === 'failed' && cur.message ? 'Last station: ' + cur.message : '', true);
  if (k === 'fav') fill(favs, 'Tap ☆ on a station to keep it here.');
  else if (k === 'bg') {
    if (bgCache) { fill(bgCache, 'No stations.'); return; }
    fill([], 'Loading…');
    try { const r = await rb('/json/stations/bycountrycodeexact/BG?hidebroken=true&order=clickcount&reverse=true&limit=100'); bgCache = r.data.map(toStation).filter(usable); if (tab === 'bg') fill(bgCache, 'No stations.'); }
    catch (e) { if (tab === 'bg') fill([], 'The station list needs internet on this phone or computer.'); }
  } else if (k === 'search') {
    form.appendChild(q); fill([], 'Type a name or a genre (jazz, rock, news…).'); setTimeout(() => q.focus(), 50);
    if (q.value.trim()) search();
  } else {
    form.append(uName, uUrl, uBtns); list.textContent = '';
    list.appendChild(el('div', 'r-empty', 'Paste the address of a stream (an Icecast/Shoutcast URL, MP3 or AAC). Playlists (.m3u, .pls) and HLS (.m3u8) do not work yet.'));
  }
}
async function search() {
  const t = q.value.trim(); if (!t) { fill([], 'Type a name or a genre (jazz, rock, news…).'); return; }
  fill([], 'Searching…');
  const e = encodeURIComponent(t);
  try {
    const [a, b] = await Promise.all([
      rb('/json/stations/search?name=' + e + '&hidebroken=true&order=clickcount&reverse=true&limit=60'),
      rb('/json/stations/search?tag=' + e + '&hidebroken=true&order=clickcount&reverse=true&limit=40')]);
    const seen = new Set(), out = [];
    [...a.data, ...b.data].map(toStation).filter(usable).forEach(s => { if (!seen.has(s.url)) { seen.add(s.url); out.push(s); } });
    if (q.value.trim() === t && tab === 'search') fill(out, 'No station found.');
  } catch (er) { if (tab === 'search') fill([], 'The station search needs internet on this phone or computer.'); }
}
q.oninput = () => { clearTimeout(searchT); searchT = setTimeout(search, 450); };

async function loadFavs() { try { const f = await fetchJ('/api/radio/favs'); favs = Array.isArray(f) ? f : []; } catch (e) { favs = []; } }
async function toggleFav(s) {
  const was = isFav(s.url);
  const next = was ? favs.filter(f => f.url !== s.url) : favs.concat([{ name: s.name, url: s.url, logo: s.logo, cc: s.cc, codec: s.codec, br: s.br, uuid: s.uuid }]);
  try { const f = await fetchJ('/api/radio/favs', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(next) }); favs = f; }
  catch (e) { setMsg('Could not save the favourites (' + (next.length > favs.length ? 'too many?' : 'error') + ')', true); return; }
  show(tab);
}
async function play(s) {
  setMsg('Starting ' + (s.name || s.url) + '…');
  try {
    const d = await fetchJ('/api/radio/start', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ url: s.url, name: s.name || '', logo: s.logo || '' }) });
    render(d); close();
    if (s.uuid) rb('/json/url/' + s.uuid).catch(() => {}); /* Radio Browser's click count */
  } catch (e) { setMsg(String(e.message || e), true); }
  poll(500);
}
uPlay.onclick = () => { const u = uUrl.value.trim(); if (!/^https?:\/\//i.test(u)) { setMsg('The address starts with http:// or https://', true); return; } play({ name: uName.value.trim() || hostOf(u), url: u, logo: '' }); };
uSave.onclick = () => { const u = uUrl.value.trim(); if (!/^https?:\/\//i.test(u)) { setMsg('The address starts with http:// or https://', true); return; } if (!isFav(u)) toggleFav({ name: uName.value.trim() || hostOf(u), url: u, logo: '' }); setMsg('Saved to favourites'); };
const hostOf = u => { try { return new URL(u).hostname.replace(/^www\./, ''); } catch (e) { return u; } };

function open() { root.classList.add('r-open'); document.body.style.overflow = 'hidden'; show(tab); }
function close() { root.classList.remove('r-open'); document.body.style.overflow = ''; }
pill.onclick = open; bSt.onclick = open; x.onclick = close; shade.onclick = close;
document.addEventListener('keydown', e => { if (e.key === 'Escape') close(); });
bStop.onclick = async () => { bStop.disabled = true; try { render(await fetchJ('/api/radio/stop')); } catch (e) {} bStop.disabled = false; };

/* ---- volume: live while dragging, kept on the Gateway ---- */
let volT0 = 0, volTimer = null;
const sendVol = () => fetch('/api/radio/volume?db=' + vol.value, { cache: 'no-store' }).catch(() => {});
vol.oninput = () => { dragging = true; volT.textContent = vol.value + ' dB'; const n = Date.now(); clearTimeout(volTimer); if (n - volT0 > 250) { volT0 = n; sendVol(); } else volTimer = setTimeout(sendVol, 250); };
vol.onchange = () => { clearTimeout(volTimer); sendVol(); setTimeout(() => { dragging = false; }, 600); };

/* ---- what the Gateway says ---- */
const rate = r => r ? (r % 1000 ? (r / 1000).toFixed(1) : r / 1000) : '';
function chip(t, c) { return el('span', 'chip ' + c, t); }
function render(d) {
  cur = d;
  const active = d.active || ['starting', 'playing', 'buffering', 'stopping'].includes(d.state);
  const air = !!d.airplay;
  pill.style.display = !air && !active ? '' : 'none';
  pill.classList.toggle('err', d.state === 'failed');
  pill.title = d.state === 'failed' && d.message ? 'Last station: ' + d.message : 'Play web radio';
  bar.style.display = active && !air ? '' : 'none';
  if (!active) return;
  const name = d.name || hostOf(d.url);
  if (d.url !== lastUrl || bLogo.dataset.logo !== (d.logo || '')) { logoInto(bLogo, name, d.logo); bLogo.dataset.logo = d.logo || ''; lastUrl = d.url; }
  bName.textContent = name;
  bTitle.textContent = d.state === 'playing' ? (d.title || '') : (d.message || (d.state === 'starting' ? 'Starting…' : ''));
  bChips.textContent = '';
  if (d.codec) bChips.appendChild(chip(d.codec + (d.bitrate_kbps ? ' ' + d.bitrate_kbps : '') + (d.alac_rate ? ' → ALAC ' + rate(d.alac_rate) + '/' + d.alac_bits : ''), 'c-fmt'));
  if (d.state === 'playing') bChips.appendChild(chip(d.source_channels === 1 ? 'Mono' : 'Stereo', 'c-st'));
  else bChips.appendChild(chip(d.state === 'buffering' ? 'Buffering…' : d.state === 'stopping' ? 'Stopping…' : 'Starting…', 'c-warn'));
  if (!dragging) { vol.value = d.volume_db; volT.textContent = d.volume_db + ' dB'; }
}
async function poll(ms) {
  clearTimeout(pollT);
  pollT = setTimeout(async () => {
    let d = null; try { d = await fetchJ('/api/radio'); render(d); } catch (e) {}
    poll(d && ['starting', 'buffering', 'stopping'].includes(d.state) ? 700 : 2500);
  }, ms);
}
loadFavs(); poll(0);
})();
