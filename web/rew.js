/* REW measuring mode (ROADMAP_REW_AND_ALIGNMENT.md, phase 2), served by the
 * Gateway as /rew.js.
 *  - Every page that loads it: while the mode is on, a bar at the top
 *    ("REW: only <name> plays") with Exit.
 *  - /speakers (it has .rew-open-btn buttons): the "Set up with REW" window, the same
 *    room map as the page. Click a speaker: only it plays; click it again:
 *    none plays. A bi-amp speaker also gets Whole / Woofer / Tweeter and
 *    "Invert the tweeter". Closing the window keeps the mode on (measure
 *    with the window behind REW); Exit or 15 minutes without an action end
 *    it. The state lives on the Gateway (/api/rew), so two browsers show the
 *    same. */
(function(){
'use strict';
const css=`
.rew-bar{position:sticky;top:0;z-index:40;display:flex;align-items:center;gap:12px;flex-wrap:wrap;margin:0 0 14px;padding:10px 14px;border-radius:14px;background:var(--panel,#0f0f10);border:1px solid var(--accent,#8b7bff);box-shadow:0 6px 24px rgba(0,0,0,.45)}
.rew-bar[hidden]{display:none}
.rew-bar .rew-dot{width:10px;height:10px;border-radius:50%;background:var(--accent,#8b7bff);box-shadow:0 0 10px var(--accent-glow,rgba(139,92,255,.55));flex:none}
.rew-bar .rew-txt{flex:1;min-width:0;font-size:14px;color:var(--text,#f4f4f5)}
.rew-bar .rew-txt b{font-weight:700}
.rew-bar .rew-txt span{color:var(--muted,#8d8d93)}
.rew-bar button,.rew-dlg .rew-b{padding:7px 16px;border-radius:999px;font:inherit;font-size:13px;font-weight:600;cursor:pointer;border:1px solid var(--line2,rgba(255,255,255,.16));background:transparent;color:var(--text,#f4f4f5)}
.rew-bar button:hover,.rew-dlg .rew-b:hover{border-color:var(--text,#f4f4f5)}
.rew-bar button.rew-main,.rew-dlg .rew-b.rew-main{background:var(--text,#f4f4f5);color:var(--bg,#000);border-color:transparent}
.rew-ov{position:fixed;inset:0;z-index:50;background:rgba(0,0,0,.6);display:flex;align-items:flex-start;justify-content:center;padding:24px 16px;overflow:auto}
.rew-ov[hidden]{display:none}
.rew-dlg{width:100%;max-width:560px;background:var(--panel,#0f0f10);border:1px solid var(--line2,rgba(255,255,255,.16));border-radius:24px;padding:20px;color:var(--text,#f4f4f5)}
.rew-h{display:flex;align-items:center;justify-content:space-between;gap:10px;margin-bottom:8px}
.rew-h h2{font-size:22px;font-weight:700}
.rew-x{width:36px;height:36px;border-radius:50%;border:1px solid var(--line2,rgba(255,255,255,.16));background:none;color:var(--text,#f4f4f5);font-size:20px;line-height:1;cursor:pointer}
.rew-hint{font-size:13px;color:var(--muted,#8d8d93);line-height:1.6}
.rew-hint b{color:var(--text,#f4f4f5);font-weight:600}
.rew-warn{font-size:13px;color:#f7c46c;margin-top:8px;line-height:1.5}
.rew-warn[hidden]{display:none}
.rew-dlg .room{margin:14px auto 0;max-width:420px;aspect-ratio:1/1}
.rew-dlg .node{cursor:pointer}
.rew-dlg .node.rew-off .dot{opacity:.35}
.rew-mix{display:flex;justify-content:center;gap:6px;flex-wrap:wrap;margin-top:8px;min-height:4px}
.rew-sel{text-align:center;font-size:15px;margin-top:12px;min-height:20px}
.rew-sel span{color:var(--muted,#8d8d93)}
.rew-ways{display:flex;justify-content:center;gap:2px;padding:3px;border-radius:12px;background:var(--panel2,#17171a);border:1px solid var(--line,rgba(255,255,255,.08));margin:12px auto 0;width:max-content;max-width:100%}
.rew-ways[hidden]{display:none}
.rew-ways button{border:0;border-radius:9px;padding:8px 16px;min-height:36px;font:inherit;font-size:14px;font-weight:600;color:var(--muted,#8d8d93);background:transparent;cursor:pointer}
.rew-ways button.on{background:var(--accent-soft,rgba(139,123,255,.14));color:var(--text,#f4f4f5)}
.rew-sws{display:flex;flex-direction:column;gap:10px;margin-top:16px;border-top:1px solid var(--line,rgba(255,255,255,.08));padding-top:14px}
.rew-sw{display:flex;align-items:center;gap:10px;font-size:14px;cursor:pointer}
.rew-sw[hidden]{display:none}
.rew-sw input{width:18px;height:18px;accent-color:var(--accent,#8b7bff);flex:none}
.rew-sw small{color:var(--muted,#8d8d93);font-size:12px}
.rew-act{display:flex;align-items:center;gap:10px;flex-wrap:wrap;margin-top:18px}
.rew-act .rew-msg{flex:1;min-width:120px;font-size:12px;color:var(--muted,#8d8d93)}
.rew-act .rew-msg.err{color:#ff8a80}
.rew-chip{font-size:12px;padding:5px 11px;border-radius:999px;border:1px solid var(--line2,rgba(255,255,255,.16));background:none;color:var(--text,#f4f4f5);cursor:pointer;font:inherit}
.rew-chip.on{background:var(--accent-soft,rgba(139,123,255,.14));border-color:var(--accent,#8b7bff)}
button.eq-btn{background:none;font-family:inherit;cursor:pointer}
.np-h .rew-open-btn{margin-left:10px}
.rew-imp{margin-top:16px;border-top:1px solid var(--line,rgba(255,255,255,.08));padding-top:14px}
.rew-imp[hidden]{display:none}
.rew-imp-h{font-size:15px;font-weight:700;margin-bottom:4px}
.rew-imp-h small{font-weight:500;color:var(--muted,#8d8d93);font-size:13px}
.rew-imp p{font-size:12px;color:var(--muted,#8d8d93);line-height:1.5;margin-bottom:8px}
.rew-imp textarea{width:100%;min-height:96px;resize:vertical;padding:10px 12px;background:#0a0a0b;border:1px solid var(--line2,rgba(255,255,255,.16));border-radius:10px;color:var(--text,#f4f4f5);font:12px/1.5 ui-monospace,Menlo,monospace}
.rew-imp textarea:focus{outline:none;border-color:var(--accent,#8b7bff)}
.rew-imp-row{display:flex;align-items:center;gap:10px;flex-wrap:wrap;margin-top:8px;font-size:13px}
.rew-imp-row[hidden],.rew-imp-row label[hidden]{display:none}
.rew-imp-row select{width:auto;padding:7px 10px;background:#0a0a0b;border:1px solid var(--line2,rgba(255,255,255,.16));border-radius:10px;color:var(--text,#f4f4f5);font:inherit}
.rew-imp-err{font-size:12px;color:#ff8a80;line-height:1.6;margin-top:8px;white-space:pre-line}
.rew-imp-err:empty{display:none}
.rew-tab{width:100%;border-collapse:collapse;margin-top:10px;font-size:13px;font-variant-numeric:tabular-nums}
.rew-tab th{font-size:10px;color:var(--muted,#8d8d93);text-transform:uppercase;letter-spacing:1.2px;font-weight:500;text-align:right;padding:4px 6px}
.rew-tab td{border-top:1px solid var(--line,rgba(255,255,255,.08));padding:6px;text-align:right;white-space:nowrap}
.rew-tab th:first-child,.rew-tab td:first-child{text-align:left}
.rew-tab td small{color:var(--muted,#8d8d93)}
.rew-curve{width:100%;height:auto;margin-top:10px;display:block}
.rew-curve text{fill:var(--muted,#8d8d93);font-size:11px}
.rew-curve .g{stroke:var(--line,rgba(255,255,255,.08));stroke-width:1}
.rew-curve .z{stroke:var(--line2,rgba(255,255,255,.16));stroke-width:1}
.rew-curve .c{fill:none;stroke:var(--accent,#8b7bff);stroke-width:2}
.rew-imp-act{display:flex;align-items:center;gap:10px;flex-wrap:wrap;margin-top:12px}
.rew-imp-act .rew-imp-msg{flex:1;min-width:140px;font-size:12px;color:var(--muted,#8d8d93)}
.rew-imp-act .rew-imp-msg.ok{color:#7ee2a0}.rew-imp-act .rew-imp-msg.err{color:#ff8a80}
.rew-b:disabled{opacity:.4;cursor:not-allowed}
@media(max-width:560px){.rew-dlg{padding:16px;border-radius:18px}.rew-h h2{font-size:19px}.rew-ways button{padding:8px 12px}}
`;
const st=document.createElement('style');st.textContent=css;document.head.appendChild(st);

const WAYS={full:'whole speaker',woofer:'woofer',tweeter:'tweeter'};
const SEAT="<svg viewBox='0 0 24 24' role='img' aria-label='Your seat'><path d='M6 10V7a3 3 0 0 1 3-3h6a3 3 0 0 1 3 3v3'/><path d='M4 13a2 2 0 0 1 4 0v1h8v-1a2 2 0 0 1 4 0v4H4z'/><path d='M6 17v2M18 17v2'/></svg>";
const e=t=>String(t).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
let state={active:false},speakers=[],biamp={},timer=null,busy=false;
const opens=document.querySelectorAll('.rew-open-btn'),page=opens.length>0; /* /speakers only */

/* ---- REW filters (phase 3; docs/rew-alignment-design.md, points 3 and 4)
 * Two text formats, one parser: REW "Export filter settings as text" with
 * Equaliser Generic, and Equalizer APO ("Preamp: -6 dB", "Filter 1: ON PK Fc
 * 100 Hz Gain -3 dB Q 2"). A line is a filter only when it starts with
 * "Filter"; its number is not used (REW's restart at 1), "ON None" is an
 * empty slot. REW computes like the RBJ Cookbook, the speaker's biquads are
 * Zölzer's: a peak's Q becomes Q * 10^(|gain|/40) (exact), a 12 dB/octave
 * shelf at the centre frequency f0 becomes the speaker's corner shelf at
 * f0 * 10^(-|gain|/80) (low) or f0 * 10^(+|gain|/80) (high), within 0.02 dB. */
const RBJ_SHELF_Q=Math.SQRT1_2;
function rewFilter(type,fc,g,q,bw){
 const T=type.toUpperCase().replace(/\s+/g,' ').replace(/(\d) ?DB\b/,'$1dB');
 const need=(v,n)=>{if(v===null)throw new Error(n+' missing');return v;};
 if(/^(PK|PEQ|MODAL)$/.test(T)){
  need(g,'Gain');
  if(q===null&&bw!==null)q=1/(2*Math.sinh(Math.LN2/2*bw));
  need(q,'Q');
  return {type:'PK',frequency_hz:fc,gain_db:g,q:q*Math.pow(10,Math.abs(g)/40)};
 }
 if(T==='LP'||T==='HP')return {type:T,frequency_hz:fc,gain_db:0,q:Math.SQRT1_2};
 if(T==='LPQ'||T==='HPQ')return {type:T.slice(0,2),frequency_hz:fc,gain_db:0,q:q===null?Math.SQRT1_2:q};
 if(T==='NO')return {type:'NOTCH',frequency_hz:fc,gain_db:0,q:need(q,'Q')};
 if(T==='BP')return {type:'BP',frequency_hz:fc,gain_db:0,q:need(q,'Q')};
 let m=T.match(/^(LS|HS)(C)?(?: ([\d.]+)dB)?$/);
 if(m){
  need(g,'Gain');
  const low=m[1]==='LS',slope=m[3]!==undefined?parseFloat(m[3]):null;
  /* "LS 12dB": 12 dB/octave at the corner, the speaker's own shelf. */
  if(!m[2]&&slope===12)return {type:m[1],frequency_hz:fc,gain_db:g,q:0.71};
  /* "LS", "LSC", "LSC 12 dB" or Q 0.707: 12 dB/octave at the centre. */
  if((slope===null||slope===12)&&(q===null||Math.abs(q-RBJ_SHELF_Q)<0.01))
   return {type:m[1],frequency_hz:fc*Math.pow(10,(low?-1:1)*Math.abs(g)/80),gain_db:g,q:0.71};
  throw new Error('a shelf of '+(slope!==null?slope+' dB/octave':'Q '+q)+' is not supported: the speaker\'s shelves are 12 dB/octave (Q 0.71)');
 }
 throw new Error('type '+type.trim()+' is not supported');
}
function parseRew(text){
 const out={filters:[],preamp:null,errors:[]};
 String(text||'').split(/\r?\n/).forEach((raw,i)=>{
  const line=raw.trim(),n=i+1;if(!line)return;
  const pre=line.match(/^Preamp\s*:\s*([-+]?[\d.]+)\s*dB/i);
  if(pre){const v=parseFloat(pre[1]);
   if(!(v>=-24&&v<=0))out.errors.push({n,line,msg:'Preamp '+v+' dB: the speaker takes -24 to 0 dB'});
   else out.preamp=(out.preamp||0)+v;return;}
  const f=line.match(/^Filter\s*\d*\s*:\s*(ON|OFF)\b\s*(.*)$/i);
  if(!f)return; /* headers, notes, other EAPO commands */
  if(f[1].toUpperCase()==='OFF'||/^None\b/i.test(f[2])||f[2]==='')return;
  const body=f[2],t=body.match(/^(.*?)\s+Fc\s+([\d.]+)\s*(k?)Hz/i);
  if(!t){out.errors.push({n,line,msg:'no "Fc … Hz"'});return;}
  const num=re=>{const m=body.match(re);return m?parseFloat(m[1]):null;};
  const fc=parseFloat(t[2])*(t[3]?1000:1),g=num(/\bGain\s+([-+]?[\d.]+)\s*dB/i),q=num(/\bQ\s+([\d.]+)/i),bw=num(/\bBW\s+Oct\s+([\d.]+)/i);
  try{
   const o=rewFilter(t[1],fc,g,q,bw);
   if(!(o.frequency_hz>=10&&o.frequency_hz<=20000))throw new Error('Fc '+o.frequency_hz.toFixed(1)+' Hz is outside 10 Hz – 20 kHz');
   if(!(o.gain_db>=-24&&o.gain_db<=24))throw new Error('Gain '+o.gain_db+' dB is outside ±24 dB');
   if(!(o.q>=0.05&&o.q<=50))throw new Error('Q '+o.q.toFixed(3)+' (as the speaker computes it) is outside 0.05 – 50');
   out.filters.push({n,src:{type:t[1].trim(),fc,g,q,bw},f:{enabled:true,type:o.type,frequency_hz:Math.round(o.frequency_hz*100)/100,gain_db:o.gain_db,q:Math.round(o.q*10000)/10000}});
  }catch(x){out.errors.push({n,line,msg:x.message});}
 });
 return out;
}
/* The speaker's biquads (audio_eq.c calc_biquad), as roomeq.html bq(). */
function spkBq(t,f,g,q,fs){const K=Math.tan(Math.PI*f/fs),V=Math.pow(10,Math.abs(g)/20),r2=Math.SQRT2,sv=Math.sqrt(2*V),KK=K*K;let n,b0,b1,b2,a1,a2;
switch(t){case'LP':n=1/(1+K/q+KK);b0=KK*n;b1=2*b0;b2=b0;a1=2*(KK-1)*n;a2=(1-K/q+KK)*n;break;
case'HP':n=1/(1+K/q+KK);b0=n;b1=-2*b0;b2=b0;a1=2*(KK-1)*n;a2=(1-K/q+KK)*n;break;
case'BP':n=1/(1+K/q+KK);b0=K/q*n;b1=0;b2=-b0;a1=2*(KK-1)*n;a2=(1-K/q+KK)*n;break;
case'NOTCH':n=1/(1+K/q+KK);b0=(1+KK)*n;b1=2*(KK-1)*n;b2=b0;a1=b1;a2=(1-K/q+KK)*n;break;
case'LS':if(g>=0){n=1/(1+r2*K+KK);b0=(1+sv*K+V*KK)*n;b1=2*(V*KK-1)*n;b2=(1-sv*K+V*KK)*n;a1=2*(KK-1)*n;a2=(1-r2*K+KK)*n;}
 else{n=1/(1+sv*K+V*KK);b0=(1+r2*K+KK)*n;b1=2*(KK-1)*n;b2=(1-r2*K+KK)*n;a1=2*(V*KK-1)*n;a2=(1-sv*K+V*KK)*n;}break;
case'HS':if(g>=0){n=1/(1+r2*K+KK);b0=(V+sv*K+KK)*n;b1=2*(KK-V)*n;b2=(V-sv*K+KK)*n;a1=2*(KK-1)*n;a2=(1-r2*K+KK)*n;}
 else{n=1/(V+sv*K+KK);b0=(1+r2*K+KK)*n;b1=2*(KK-1)*n;b2=(1-r2*K+KK)*n;a1=2*(KK-V)*n;a2=(V-sv*K+KK)*n;}break;
default:if(g>=0){n=1/(1+K/q+KK);b0=(1+V*K/q+KK)*n;b1=2*(KK-1)*n;b2=(1-V*K/q+KK)*n;a1=b1;a2=(1-K/q+KK)*n;}else{n=1/(1+V*K/q+KK);b0=(1+K/q+KK)*n;b1=2*(KK-1)*n;b2=(1-K/q+KK)*n;a1=b1;a2=(1-V*K/q+KK)*n;}}
return[b0,b1,b2,a1,a2];}
function bqDb(k,f,fs){const w=2*Math.PI*f/fs,c1=Math.cos(w),s1=Math.sin(w),c2=Math.cos(2*w),s2=Math.sin(2*w);
 const nr=k[0]+k[1]*c1+k[2]*c2,ni=-(k[1]*s1+k[2]*s2),dr=1+k[3]*c1+k[4]*c2,di=-(k[3]*s1+k[4]*s2);
 return 10*Math.log10((nr*nr+ni*ni)/(dr*dr+di*di));}
window.rewParse=parseRew; /* tests */

/* ---- the bar ---- */
const bar=document.createElement('div');bar.className='rew-bar';bar.hidden=true;bar.setAttribute('role','status');
bar.innerHTML="<i class='rew-dot'></i><div class='rew-txt'></div>"+(page?"<button type='button' class='rew-o'>Open</button>":'')+"<button type='button' class='rew-main rew-e'>Exit</button>";
const wrap=document.querySelector('.wrap')||document.body;wrap.insertBefore(bar,wrap.firstChild);
bar.querySelector('.rew-e').onclick=()=>post({action:'stop'}).then(()=>{if(ov)ov.hidden=true;});
if(page)bar.querySelector('.rew-o').onclick=()=>openWin();

function who(){return state.name?state.name:'';}
function drawBar(){
 bar.hidden=!state.active;if(!state.active)return;
 const t=bar.querySelector('.rew-txt');
 t.innerHTML=state.mac?'<b>REW:</b> only <b>'+e(who())+'</b> plays'+(state.way&&state.way!=='full'?' <span>('+WAYS[state.way]+')</span>':'')+(state.eq_on?'':' <span>· EQ off</span>')+(state.invert_tweeter?' <span>· tweeter inverted</span>':''):'<b>REW:</b> no speaker plays';
}

async function get(){
 try{const r=await fetch('/api/rew',{cache:'no-store'});if(r.ok)state=await r.json();}catch(x){}
 drawBar();if(ov&&!ov.hidden)drawWin();
}
async function post(body){
 busy=true;
 try{
  const r=await fetch('/api/rew',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
  const t=await r.text();let d=null;try{d=JSON.parse(t);}catch(x){}
  if(!d)throw new Error(t||('HTTP '+r.status));
  state=d;if(!d.success)msg(d.error||'Error',true);else msg('');
 }catch(x){msg(x.message||'The Gateway did not answer',true);}
 busy=false;drawBar();if(ov&&!ov.hidden)drawWin();return state;
}
function poll(){clearTimeout(timer);timer=setTimeout(async()=>{if(!busy)await get();poll();},ov&&!ov.hidden?1500:3000);}

/* ---- the window (/speakers) ---- */
let ov=null,nodes={},gwName='the Gateway';
function msg(t,err){if(!ov)return;const m=ov.querySelector('.rew-msg');m.textContent=t;m.className='rew-msg'+(err?' err':'');}
function buildWin(){
 ov=document.createElement('div');ov.className='rew-ov';ov.hidden=true;
 ov.innerHTML="<div class='rew-dlg' role='dialog' aria-modal='true' aria-labelledby='rew-t'>"+
  "<div class='rew-h'><h2 id='rew-t'>Set up with REW</h2><button type='button' class='rew-x' aria-label='Close'>&times;</button></div>"+
  "<p class='rew-hint'>In REW: output <b>AirPlay to <span class='rew-gw'></span></b>. Channel L, R or L+R, any of them. No timing reference is needed. Only the speaker you click plays; the others stay silent.</p>"+
  "<div class='rew-warn rew-mc' hidden>A 5.1 or 7.1 stream is playing: the marked speaker plays its own channel, not L+R. Play stereo from REW.</div>"+
  "<div class='rew-warn rew-clip' hidden></div>"+
  "<div class='room rew-room'><div class='floor'></div><div class='ring'></div><div class='screen'></div><div class='seat' title='You'>"+SEAT+"</div></div>"+
  "<div class='rew-mix'></div>"+
  "<div class='rew-sel'></div>"+
  "<div class='rew-ways' role='group' aria-label='Driver' hidden><button type='button' data-w='full'>Whole</button><button type='button' data-w='woofer'>Woofer</button><button type='button' data-w='tweeter'>Tweeter</button></div>"+
  "<div class='rew-sws'><label class='rew-sw'><input type='checkbox' class='rew-eq'><span>EQ while measuring <small>(off: no filters and preamp; the crossover, polarity and delay stay)</small></span></label>"+
  "<label class='rew-sw rew-inv-l' hidden><input type='checkbox' class='rew-inv'><span>Invert the tweeter <small>(only while this mode is on)</small></span></label></div>"+
  "<div class='rew-imp' hidden><div class='rew-imp-h'>Filters from REW <small class='rew-imp-to'></small></div>"+
  "<p>Paste REW's <b>Export filter settings as text</b> (Equaliser <b>Generic</b>) or an Equalizer APO file. They replace this output's EQ bands; its high- and low-pass bands, the crossover, polarity and delay stay. A Preamp line sets the output's level.</p>"+
  "<textarea class='rew-in' spellcheck='false' placeholder='Filter  1: ON  PK       Fc   180.5 Hz  Gain  -3.40 dB  Q  4.905'></textarea>"+
  "<div class='rew-imp-row'><input type='file' class='rew-file' accept='.txt,text/plain' hidden><button type='button' class='rew-b rew-load'>Load .txt</button>"+
  "<label class='rew-to-l' hidden>Whole speaker, bi-amp: to <select class='rew-to'><option value='both'>both drivers</option><option value='woofer'>the woofer</option><option value='tweeter'>the tweeter</option></select></label></div>"+
  "<div class='rew-imp-err'></div><table class='rew-tab' hidden></table><svg class='rew-curve' viewBox='0 0 600 190' hidden role='img' aria-label='The filters as the speaker computes them'></svg>"+
  "<div class='rew-imp-act'><span class='rew-imp-msg'></span><button type='button' class='rew-b rew-apply' disabled>Apply</button><button type='button' class='rew-b rew-main rew-save' disabled>Apply &amp; Save</button></div></div>"+
  "<div class='rew-act'><span class='rew-msg'></span><button type='button' class='rew-b rew-c'>Close</button><button type='button' class='rew-b rew-main rew-stop'>Exit REW mode</button></div></div>";
 document.body.appendChild(ov);
 const room=ov.querySelector('.rew-room');
 SPOTS.forEach(([ch,x,y])=>{const n=document.createElement('div');n.className='node';n.style.left=x+'%';n.style.top=y+'%';
  n.innerHTML="<div class='dot'>"+short(ch)+"</div><div class='who'></div>";room.appendChild(n);
  nodes[ch]={el:n,who:n.querySelector('.who'),dot:n.querySelector('.dot'),macs:[]};
  n.onclick=()=>clickRole(ch);});
 ov.querySelector('.rew-x').onclick=closeWin;ov.querySelector('.rew-c').onclick=closeWin;
 ov.addEventListener('click',ev=>{if(ev.target===ov)closeWin();});
 document.addEventListener('keydown',ev=>{if(ev.key==='Escape'&&!ov.hidden)closeWin();});
 ov.querySelector('.rew-stop').onclick=()=>post({action:'stop'}).then(()=>{if(!state.active)closeWin();});
 ov.querySelectorAll('.rew-ways button').forEach(b=>b.onclick=()=>{if(state.mac)post({action:'select',mac:state.mac,way:b.dataset.w});});
 ov.querySelector('.rew-eq').onchange=ev=>post({action:'set',eq_on:ev.target.checked});
 ov.querySelector('.rew-inv').onchange=ev=>post({action:'set',invert_tweeter:ev.target.checked});
 const tin=ov.querySelector('.rew-in'),file=ov.querySelector('.rew-file');
 tin.addEventListener('input',()=>drawImport());
 ov.querySelector('.rew-load').onclick=()=>file.click();
 file.onchange=()=>{const f=file.files[0];if(!f)return;f.text().then(t=>{tin.value=t;file.value='';drawImport();});};
 ov.querySelector('.rew-to').onchange=()=>drawImport();
 ov.querySelector('.rew-apply').onclick=()=>applyImport('apply');
 ov.querySelector('.rew-save').onclick=()=>applyImport('both');
 fetch('/api/system/info').then(r=>r.json()).then(d=>{gwName=(d.info&&d.info.device_name)||gwName;ov.querySelector('.rew-gw').textContent=gwName;}).catch(()=>{});
 ov.querySelector('.rew-gw').textContent=gwName;
}
function select(mac){
 if(busy)return;
 if(mac&&mac===state.mac)mac=''; /* the marked one again: none plays */
 post({action:'select',mac,way:'full'});
 if(mac&&biamp[mac]===undefined)learnBiamp(mac);
}
function clickRole(ch){const n=nodes[ch];if(!n||!n.macs.length)return;
 const i=n.macs.indexOf(state.mac);
 /* Two speakers in one role: the next one, then none. */
 select(i<0?n.macs[0]:i+1<n.macs.length?n.macs[i+1]:state.mac);}
async function learnBiamp(mac){
 biamp[mac]=null; /* asking */
 try{const r=await fetch('/api/speakers/eq?mac='+mac+'&action=get',{method:'POST',headers:{'Content-Type':'application/json'},body:'',cache:'no-store'});
  const d=await r.json();biamp[mac]=d.output_mode==='biamp';}
 catch(x){delete biamp[mac];}
 drawWin();
}
function drawWin(){
 const byRole={},mix=[];
 for(const ch in nodes)nodes[ch].macs=[];
 speakers.forEach(s=>{if(s.channel===-1){mix.push(s);return;}const n=nodes[s.channel];if(n){n.macs.push(s.mac);(byRole[s.channel]=byRole[s.channel]||[]).push(s.name||s.ip);}});
 for(const ch in nodes){const n=nodes[ch],on=n.macs.length>0,sel=on&&n.macs.includes(state.mac);
  n.el.className='node'+(on?' on':'')+(sel?' live':on&&state.mac?' rew-off':'');
  n.who.textContent=sel?(speakers.find(s=>s.mac===state.mac)||{}).name||'':'';
  n.el.title=on?byRole[ch].join(', '):'No speaker';n.el.style.cursor=on?'pointer':'default';}
 const mx=ov.querySelector('.rew-mix');
 mx.innerHTML=mix.map(s=>"<button type='button' class='rew-chip"+(s.mac===state.mac?' on':'')+"' data-m='"+e(s.mac)+"'>Mono mix: "+e(s.name||s.ip)+"</button>").join('');
 mx.querySelectorAll('button').forEach(b=>b.onclick=()=>select(b.dataset.m));
 const sp=speakers.find(s=>s.mac===state.mac);
 ov.querySelector('.rew-sel').innerHTML=!state.active?'<span>The REW mode is off.</span>':state.mac?'Only <b>'+e(sp?(TITLE[sp.channel]||'')+' · '+(sp.name||sp.ip):state.name)+'</b> plays'+(state.way!=='full'?' <span>('+WAYS[state.way]+')</span>':''):'<span>Click a speaker: only it plays.</span>';
 const bi=!!(state.mac&&biamp[state.mac]);
 if(state.mac&&biamp[state.mac]===undefined)learnBiamp(state.mac);
 const ways=ov.querySelector('.rew-ways');ways.hidden=!bi;
 ways.querySelectorAll('button').forEach(b=>{const on=b.dataset.w===state.way;b.classList.toggle('on',on);b.setAttribute('aria-pressed',on);});
 ov.querySelector('.rew-inv-l').hidden=!bi;
 ov.querySelector('.rew-eq').checked=!!state.eq_on;ov.querySelector('.rew-inv').checked=!!state.invert_tweeter;
 ov.querySelector('.rew-mc').hidden=!state.multichannel;
 const cl=ov.querySelector('.rew-clip');cl.hidden=!state.clips;
 if(state.clips)cl.textContent='Clipping: '+state.clips+' samples above full scale'+(state.clip_peak_db?' (peak +'+state.clip_peak_db.toFixed(1)+' dB)':'')+'. Turn the level down in REW or on the Mac.';
 drawImport();
}
/* Which outputs the pasted filters go to: the marked driver; with "Whole"
 * on a bi-amp speaker the choice under the text (both drivers: the same
 * filters on both outputs, before their crossover, so on the whole
 * speaker); a speaker without bi-amp: both outputs. */
function importTarget(){
 const sp=speakers.find(s=>s.mac===state.mac);if(!sp)return null;
 const bi=!!biamp[state.mac],way=bi?(state.way==='full'?ov.querySelector('.rew-to').value:state.way):'both';
 return {mac:state.mac,name:(TITLE[sp.channel]?TITLE[sp.channel]+' · ':'')+(sp.name||sp.ip),bi,way};
}
function targetText(tg){return tg.way==='both'?(tg.bi?'both drivers':'both outputs'):tg.way+' output';}
let parsed=null,impKey='';
function drawImport(){
 const box=ov.querySelector('.rew-imp'),tg=state.active&&state.mac?importTarget():null;
 /* Another speaker or driver: its own file, nothing of the last one. */
 const key=tg?tg.mac+'|'+state.way:'';
 if(key!==impKey){impKey=key;ov.querySelector('.rew-in').value='';ov.querySelector('.rew-to').value='both';impMsg('');}
 box.hidden=!tg;if(!tg)return;
 ov.querySelector('.rew-to-l').hidden=!(tg.bi&&state.way==='full');
 ov.querySelector('.rew-imp-to').textContent='→ '+tg.name+' · '+targetText(tg);
 const text=ov.querySelector('.rew-in').value;parsed=text.trim()?parseRew(text):null;
 const err=ov.querySelector('.rew-imp-err'),tab=ov.querySelector('.rew-tab'),svg=ov.querySelector('.rew-curve');
 const errs=parsed?parsed.errors.map(x=>'Line '+x.n+': '+x.msg):[];
 if(parsed&&!parsed.errors.length&&!parsed.filters.length)errs.push('No filter in the text: a filter line starts with "Filter".');
 if(parsed&&parsed.filters.length>24)errs.push(parsed.filters.length+' filters: an output takes at most 24 (less the high- and low-pass bands it keeps).');
 err.textContent=errs.join('\n');
 const ok=!!(parsed&&parsed.filters.length&&!errs.length);
 ov.querySelector('.rew-apply').disabled=!ok;ov.querySelector('.rew-save').disabled=!ok;
 tab.hidden=!(parsed&&parsed.filters.length);svg.hidden=tab.hidden;
 if(tab.hidden)return;
 const fmt=(v,d)=>v===null?'—':(+v).toFixed(d);
 tab.innerHTML='<tr><th>Type</th><th>Fc (Hz)</th><th>Gain (dB)</th><th>Q</th><th>Speaker</th></tr>'+parsed.filters.map(x=>'<tr><td>'+e(x.src.type)+'</td><td>'+fmt(x.src.fc,x.src.fc<1000?1:0)+'</td><td>'+fmt(x.src.g,2)+'</td><td>'+(x.src.q!==null?fmt(x.src.q,3):x.src.bw!==null?fmt(x.src.bw,3)+' oct':'—')+'</td><td><small>'+e(x.f.type)+' '+fmt(x.f.frequency_hz,x.f.frequency_hz<1000?1:0)+' Hz'+(x.f.type==='PK'||x.f.type==='LP'||x.f.type==='HP'||x.f.type==='BP'||x.f.type==='NOTCH'?' Q '+fmt(x.f.q,3):'')+'</small></td></tr>').join('')+
  (parsed.preamp!==null?'<tr><td>Preamp</td><td></td><td>'+fmt(parsed.preamp,1)+'</td><td></td><td></td></tr>':'');
 /* The curve: the filters (and the preamp) as the speaker computes them. */
 const FS=48000,W=600,H=190,L=34,R=8,T=8,B=22,N=240,pts=[];let lo=-6,hi=6;
 const ks=parsed.filters.map(x=>spkBq(x.f.type,x.f.frequency_hz,x.f.gain_db,x.f.q,FS));
 for(let i=0;i<=N;i++){const f=20*Math.pow(1000,i/N);let d=parsed.preamp||0;ks.forEach(k=>d+=bqDb(k,f,FS));pts.push([f,d]);lo=Math.min(lo,d);hi=Math.max(hi,d);}
 lo=Math.floor(lo/6)*6;hi=Math.ceil(hi/6)*6;
 const X=f=>L+(W-L-R)*Math.log10(f/20)/3,Y=d=>T+(H-T-B)*(hi-d)/(hi-lo);
 let s='';
 [20,50,100,200,500,1000,2000,5000,10000,20000].forEach(f=>{s+="<line class='g' x1='"+X(f)+"' x2='"+X(f)+"' y1='"+T+"' y2='"+(H-B)+"'/>";if([100,1000,10000].includes(f)||f===20)s+="<text x='"+X(f)+"' y='"+(H-6)+"' text-anchor='middle'>"+(f>=1000?f/1000+'k':f)+"</text>";});
 for(let d=lo;d<=hi;d+=6){s+="<line class='"+(d===0?'z':'g')+"' x1='"+L+"' x2='"+(W-R)+"' y1='"+Y(d)+"' y2='"+Y(d)+"'/><text x='"+(L-4)+"' y='"+(Y(d)+4)+"' text-anchor='end'>"+(d>0?'+':'')+d+"</text>";}
 s+="<path class='c' d='"+pts.map((p,i)=>(i?'L':'M')+X(p[0]).toFixed(1)+' '+Y(p[1]).toFixed(1)).join('')+"'/>";
 svg.innerHTML=s;
}
function impMsg(t,c){const m=ov.querySelector('.rew-imp-msg');m.textContent=t;m.className='rew-imp-msg'+(c?' '+c:'');}
async function applyImport(action){
 const tg=importTarget();if(!tg||!parsed||!parsed.filters.length)return;
 if(action==='both'&&!confirm('Save these filters on '+tg.name+'? They replace the EQ bands'+(parsed.preamp!==null?' and the level':'')+' of '+(tg.way==='both'?'both outputs':'its '+tg.way+' output')+'; the high- and low-pass bands, the crossover, polarity and delay stay.'))return;
 impMsg('Sending…');
 try{
  const q=(a,b)=>fetch('/api/speakers/eq?mac='+tg.mac+'&action='+a,{method:'POST',headers:{'Content-Type':'application/json'},body:b?JSON.stringify(b):'',cache:'no-store'}).then(async r=>{const t=await r.text();if(!r.ok)throw new Error(t||('HTTP '+r.status));return JSON.parse(t);});
  const eq=await q('get');
  const bi=eq.output_mode==='biamp',tw=eq.tweeter_output==='right'?'right':'left';
  const outs=!bi||tg.way==='both'?['left','right']:[tg.way==='tweeter'?tw:(tw==='left'?'right':'left')];
  if(bi!==tg.bi)throw new Error('the speaker\'s outputs changed: pick it again');
  const body={enabled:true,channel_mode:eq.channel_mode||'stereo',left_preamp_db:eq.left_preamp_db||0,right_preamp_db:eq.right_preamp_db||0,left_filters:eq.left_filters||[],right_filters:eq.right_filters||[]};
  outs.forEach(o=>{
   const keep=(eq[o+'_filters']||[]).filter(f=>f.enabled&&(f.type==='HP'||f.type==='LP'));
   if(keep.length+parsed.filters.length>24)throw new Error(parsed.filters.length+' filters and '+keep.length+' high/low-pass bands kept: more than the 24 of an output');
   body[o+'_filters']=keep.concat(parsed.filters.map(x=>x.f));
   if(parsed.preamp!==null)body[o+'_preamp_db']=parsed.preamp;
  });
  await q(action,body);
  impMsg(action==='both'?'Applied and saved on the speaker.':'Applied (not saved: a restart brings back the saved EQ).','ok');
  /* Measure the result with REW: the EQ has to play. */
  if(!state.eq_on){await post({action:'set',eq_on:true});impMsg(ov.querySelector('.rew-imp-msg').textContent+' EQ while measuring is on now.','ok');}
 }catch(x){impMsg('Not applied: '+(x.message||x),'err');}
}
async function openWin(){
 if(!ov)buildWin();
 ov.hidden=false;msg('');drawWin();poll();
 if(!state.active)await post({action:'start'});
 ov.querySelector('.rew-x').focus();
}
function closeWin(){if(ov)ov.hidden=true;poll();}

if(page){
 opens.forEach(b=>b.onclick=openWin);
 window.addEventListener('gw-speakers',ev=>{speakers=ev.detail||[];if(ov&&!ov.hidden)drawWin();});
}
get();poll();
})();
