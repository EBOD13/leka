/*
 * Leka replay viewer.
 *
 * Reads the JSONL produced by tools/viz/snapshot_recorder.cpp. Each line is
 * one book snapshot captured between events, so nothing here corresponds to
 * work done on the engine's hot path -- this file only draws what the
 * recorder already measured.
 *
 * The record shape is intentionally the same one a live WebSocket server
 * would publish, so swapping fetch() for a socket is the only change needed
 * to drive this from a running engine.
 */
'use strict';

const DATASETS = {
  real:  { file: 'data/real_aapl.jsonl' },
  synth: { file: 'data/synth_hawkes.jsonl' },
};

/** Prices are integers in 1/10000 units, matching lob::Price's own encoding. */
const px = raw => raw / 10000;
const fmt = raw => px(raw).toFixed(2);
const commas = n => n.toLocaleString('en-US');
const compact = n =>
  n >= 1e6 ? (n / 1e6).toFixed(1) + 'M' :
  n >= 1e3 ? (n / 1e3).toFixed(1) + 'k' : String(n);

async function loadDataset(key) {
  const res = await fetch(DATASETS[key].file);
  if (!res.ok) throw new Error(`cannot load ${DATASETS[key].file} (${res.status})`);
  const lines = (await res.text()).split('\n').filter(Boolean);
  return { meta: JSON.parse(lines[0]), frames: lines.slice(1).map(JSON.parse) };
}

/** One dataset rendered into one panel. */
class Panel {
  constructor(root, data) {
    this.meta = data.meta;
    this.frames = data.frames;
    this.tape = [];
    this._lastIdx = -1;

    root.querySelector('.title').textContent = this.meta.label;
    const q = s => root.querySelector(s);
    this.el = {
      bb: q('.bb'), ba: q('.ba'), sp: q('.sp'),
      last: q('.last'), chg: q('.chg'),
      levels: q('.levels'),
      price: q('canvas.price'),
      depth: q('canvas.depth'),
      latency: q('canvas.latency'),
      flow: q('canvas.flow'),
      ladder: q('.ladder'),
      latNow: q('.lat-now'),
      flowStats: q('.flowstats'),
      tape: q('.tape'),
    };

    // Fixed CSS height per canvas, captured once from the markup. fitCanvas
    // must never re-derive this from canvas.height: that is the *backing
    // store*, which fitCanvas itself writes, so reading it back would
    // multiply the height by devicePixelRatio on every single call.
    for (const c of [this.el.price, this.el.depth, this.el.latency, this.el.flow]) {
      c._cssH = c.height;
    }
  }

  /** Sizes the backing store for devicePixelRatio. Safe to call every frame. */
  fitCanvas(canvas) {
    const dpr = window.devicePixelRatio || 1;
    const w = Math.max(1, Math.round(canvas.getBoundingClientRect().width));
    const h = canvas._cssH;
    const bw = Math.round(w * dpr), bh = Math.round(h * dpr);
    if (canvas.width !== bw || canvas.height !== bh) {
      canvas.width = bw;
      canvas.height = bh;
      canvas.style.height = h + 'px';
    }
    const ctx = canvas.getContext('2d');
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, h);
    canvas._w = w;
    canvas._h = h;
    return ctx;
  }

  /** Reads a CSS custom property so canvas drawing follows the active theme. */
  token(name) {
    return getComputedStyle(document.documentElement).getPropertyValue(name).trim();
  }

  /** Translucent version of a hex or hsl token, for gradient fills. */
  alpha(colour, a) {
    if (colour.startsWith('#')) {
      const n = parseInt(colour.slice(1), 16);
      return `rgba(${(n >> 16) & 255},${(n >> 8) & 255},${n & 255},${a})`;
    }
    return colour.replace(/^hsl\(/, 'hsla(').replace(/\)$/, `,${a})`);
  }

  render(i) {
    const idx = Math.min(i, this.frames.length - 1);
    const f = this.frames[idx];
    if (!f) return;
    this.renderQuotes(f, idx);
    this.renderPrice(idx);
    this.renderDepth(f);
    this.renderLadder(f);
    this.renderLatency(idx);
    this.renderFlow(idx, f);
    this.renderTape(f, idx);
  }

  /** Mid-price at a frame, or null when the book is one-sided there. */
  midAt(i) {
    const f = this.frames[i];
    if (!f || !f.bb || !f.ba) return null;
    return px(f.bb + f.ba) / 2;
  }

  renderQuotes(f, idx) {
    this.el.bb.textContent = f.bb ? fmt(f.bb) : '—';
    this.el.ba.textContent = f.ba ? fmt(f.ba) : '—';
    this.el.sp.textContent = (f.bb && f.ba) ? px(f.ba - f.bb).toFixed(2) : '—';
    this.el.levels.textContent = `${f.nb}×${f.na}`;

    const mid = this.midAt(idx);
    let base = null;
    for (let k = 0; k < this.frames.length && base === null; k++) base = this.midAt(k);

    this.el.last.textContent = mid ? mid.toFixed(2) : '—';
    if (mid && base) {
      const d = mid - base;
      this.el.chg.textContent =
        `${d >= 0 ? '+' : ''}${d.toFixed(2)} (${d >= 0 ? '+' : ''}${((d / base) * 100).toFixed(2)}%)`;
      this.el.chg.className = 'chg ' + (d >= 0 ? 'up' : 'down');
    } else {
      this.el.chg.textContent = '';
      this.el.chg.className = 'chg';
    }
  }

  /**
   * Mid-price over time, with the bid-ask spread drawn as a band behind it.
   * The band is the point: on a thin venue the spread is a visible fraction
   * of the price move, and a bare mid line hides that entirely.
   */
  renderPrice(idx) {
    const c = this.el.price, ctx = this.fitCanvas(c);
    const w = c._w, h = c._h;
    const padL = 2, padR = 56, padT = 10, padB = 8;

    const from = Math.max(0, idx - 320 + 1);
    const pts = [];
    for (let i = from; i <= idx; i++) {
      const f = this.frames[i];
      if (f && f.bb && f.ba) pts.push({ i, bid: px(f.bb), ask: px(f.ba), mid: px(f.bb + f.ba) / 2 });
    }
    if (pts.length < 2) return;

    let lo = Infinity, hi = -Infinity;
    for (const p of pts) { lo = Math.min(lo, p.bid); hi = Math.max(hi, p.ask); }
    const span = (hi - lo) || 0.02;
    lo -= span * 0.2; hi += span * 0.2;

    const x = i => padL + ((i - from) / Math.max(1, idx - from)) * (w - padL - padR);
    const y = v => padT + (1 - (v - lo) / (hi - lo)) * (h - padT - padB);

    const accent = this.token('--antique-gold');

    ctx.strokeStyle = this.token('--rail'); ctx.lineWidth = 1;
    ctx.fillStyle = this.token('--subtle-text');
    ctx.font = "9.5px 'JetBrains Mono', ui-monospace, monospace";
    ctx.textAlign = 'left';
    for (let k = 0; k <= 3; k++) {
      const v = lo + ((hi - lo) * k) / 3;
      const yy = Math.round(y(v)) + 0.5;
      ctx.beginPath(); ctx.moveTo(padL, yy); ctx.lineTo(w - padR, yy); ctx.stroke();
      ctx.fillText(v.toFixed(2), w - padR + 8, yy + 3);
    }

    // spread band
    ctx.beginPath();
    pts.forEach((p, k) => k ? ctx.lineTo(x(p.i), y(p.ask)) : ctx.moveTo(x(p.i), y(p.ask)));
    for (let k = pts.length - 1; k >= 0; k--) ctx.lineTo(x(pts[k].i), y(pts[k].bid));
    ctx.closePath();
    ctx.fillStyle = this.alpha(accent, 0.14); ctx.fill();

    // area under mid
    const g = ctx.createLinearGradient(0, padT, 0, h - padB);
    g.addColorStop(0, this.alpha(accent, 0.24));
    g.addColorStop(1, this.alpha(accent, 0));
    ctx.beginPath();
    pts.forEach((p, k) => k ? ctx.lineTo(x(p.i), y(p.mid)) : ctx.moveTo(x(p.i), y(p.mid)));
    ctx.lineTo(x(pts[pts.length - 1].i), h - padB);
    ctx.lineTo(x(pts[0].i), h - padB);
    ctx.closePath();
    ctx.fillStyle = g; ctx.fill();

    // mid line
    ctx.beginPath();
    pts.forEach((p, k) => k ? ctx.lineTo(x(p.i), y(p.mid)) : ctx.moveTo(x(p.i), y(p.mid)));
    ctx.strokeStyle = accent; ctx.lineWidth = 1.6; ctx.lineJoin = 'round'; ctx.stroke();

    // last price marker and tag
    const last = pts[pts.length - 1];
    const ly = y(last.mid);
    ctx.setLineDash([2, 3]);
    ctx.beginPath(); ctx.moveTo(padL, ly); ctx.lineTo(w - padR, ly);
    ctx.strokeStyle = this.alpha(accent, 0.45); ctx.lineWidth = 1; ctx.stroke();
    ctx.setLineDash([]);
    ctx.beginPath(); ctx.arc(x(last.i), ly, 2.8, 0, Math.PI * 2);
    ctx.fillStyle = accent; ctx.fill();

    const tag = last.mid.toFixed(2);
    ctx.font = "600 10px 'JetBrains Mono', ui-monospace, monospace";
    const tw = ctx.measureText(tag).width + 11;
    ctx.fillStyle = accent;
    ctx.beginPath();
    if (ctx.roundRect) ctx.roundRect(w - padR + 4, ly - 8, tw, 16, 3);
    else ctx.rect(w - padR + 4, ly - 8, tw, 16);
    ctx.fill();
    ctx.fillStyle = this.token('--ink-strong');
    ctx.fillText(tag, w - padR + 9.5, ly + 3.5);
  }

  /**
   * Cumulative depth: the classic order-book "mountain". Quantity is summed
   * outward from the touch on each side, so the height at any price is what
   * an order sweeping to that price would have to consume.
   */
  renderDepth(f) {
    const c = this.el.depth, ctx = this.fitCanvas(c);
    const w = c._w, h = c._h, padT = 10, padB = 15;
    if (!f.bids.length && !f.asks.length) return;

    const cum = levels => {
      let total = 0;
      return levels.map(([p, q]) => ({ p: px(p), q: (total += q) }));
    };
    const bids = cum(f.bids), asks = cum(f.asks);

    const prices = [...bids.map(d => d.p), ...asks.map(d => d.p)];
    const lo = Math.min(...prices), hi = Math.max(...prices);
    const span = (hi - lo) || 0.02;
    const maxQ = Math.max(1, ...bids.map(d => d.q), ...asks.map(d => d.q));

    const x = p => ((p - (lo - span * 0.05)) / (span * 1.1)) * w;
    const y = q => padT + (1 - q / maxQ) * (h - padT - padB);

    // Stepped, not interpolated: depth genuinely is constant between ticks,
    // and a smooth curve would imply liquidity that is not there.
    const side = (data, colour, dir) => {
      if (!data.length) return;
      ctx.beginPath();
      ctx.moveTo(x(data[0].p), h - padB);
      let prevY = y(data[0].q);
      ctx.lineTo(x(data[0].p), prevY);
      for (let k = 1; k < data.length; k++) {
        ctx.lineTo(x(data[k].p), prevY);
        prevY = y(data[k].q);
        ctx.lineTo(x(data[k].p), prevY);
      }
      const edge = x(data[data.length - 1].p) + dir * 7;
      ctx.lineTo(edge, prevY);
      ctx.lineTo(edge, h - padB);
      ctx.closePath();
      const g = ctx.createLinearGradient(0, padT, 0, h - padB);
      g.addColorStop(0, this.alpha(colour, 0.36));
      g.addColorStop(1, this.alpha(colour, 0.03));
      ctx.fillStyle = g; ctx.fill();
      ctx.strokeStyle = colour; ctx.lineWidth = 1.4; ctx.stroke();
    };
    side(bids, this.token('--antique-gold'), -1);
    side(asks, this.token('--sell'), +1);

    if (f.bb && f.ba) {
      const mx = x(px(f.bb + f.ba) / 2);
      ctx.setLineDash([2, 3]);
      ctx.beginPath(); ctx.moveTo(mx, padT); ctx.lineTo(mx, h - padB);
      ctx.strokeStyle = this.alpha(this.token('--muted-text'), 0.6);
      ctx.lineWidth = 1; ctx.stroke();
      ctx.setLineDash([]);
    }

    ctx.font = "9.5px 'JetBrains Mono', ui-monospace, monospace";
    ctx.fillStyle = this.token('--subtle-text');
    ctx.textAlign = 'left';
    ctx.fillText(compact(maxQ), 2, padT + 7);
    ctx.textAlign = 'center';
    if (bids.length) ctx.fillText(bids[bids.length - 1].p.toFixed(2), Math.max(20, x(bids[bids.length - 1].p)), h - 3);
    if (asks.length) ctx.fillText(asks[asks.length - 1].p.toFixed(2), Math.min(w - 20, x(asks[asks.length - 1].p)), h - 3);
    ctx.textAlign = 'left';
  }

  renderLadder(f) {
    const maxQty = Math.max(1, ...f.bids.map(l => l[1]), ...f.asks.map(l => l[1]));
    const row = ([price, qty], side) =>
      `<div class="lrow ${side}">
         <span class="px">${fmt(price)}</span>
         <span class="bar"><span class="fill" style="width:${((qty / maxQty) * 100).toFixed(1)}%"></span></span>
         <span class="qty">${commas(qty)}</span>
       </div>`;
    const asks = f.asks.slice().reverse().map(l => row(l, 'a')).join('');
    const bids = f.bids.map(l => row(l, 'b')).join('');
    const spread = (f.bb && f.ba)
      ? `<div class="spreadrow">${px(f.ba - f.bb).toFixed(2)} spread</div>`
      : `<div class="spreadrow">one-sided</div>`;
    this.el.ladder.innerHTML =
      (asks || '<div class="empty">no resting asks</div>') + spread +
      (bids || '<div class="empty">no resting bids</div>');
  }

  renderLatency(idx) {
    const c = this.el.latency, ctx = this.fitCanvas(c);
    const w = c._w, h = c._h, padT = 4, padB = 4, padR = 36;

    const from = Math.max(0, idx - 240 + 1);
    const slice = this.frames.slice(from, idx + 1);
    if (!slice.length) return;

    const finite = [];
    for (const f of slice) for (const k of Object.keys(f.lat || {})) {
      for (const v of f.lat[k]) if (v > 0) finite.push(v);
    }
    if (!finite.length) return;

    // Log scale: p50 and the tail differ by orders of magnitude, which a
    // linear axis would flatten into a single line along the bottom.
    const lo = Math.log10(Math.max(10, Math.min(...finite)));
    const hi = Math.log10(Math.max(...finite) * 1.3);
    const y = v => padT + (1 - (Math.log10(Math.max(v, 10)) - lo) / (hi - lo || 1)) * (h - padT - padB);
    const x = i => (i / Math.max(1, slice.length - 1)) * (w - padR);

    ctx.strokeStyle = this.token('--rail'); ctx.lineWidth = 1;
    ctx.fillStyle = this.token('--subtle-text');
    ctx.font = "9px 'JetBrains Mono', ui-monospace, monospace";
    for (let d = Math.ceil(lo); d <= Math.floor(hi); d++) {
      const v = Math.pow(10, d), yy = Math.round(y(v)) + 0.5;
      ctx.beginPath(); ctx.moveTo(0, yy); ctx.lineTo(w - padR, yy); ctx.stroke();
      ctx.fillText(v >= 1000 ? `${v / 1000}µs` : `${v}ns`, w - padR + 5, yy + 3);
    }

    for (const s of [
      { i: 2, colour: this.token('--sell'), fill: false },
      { i: 1, colour: this.token('--warm-amber'), fill: false },
      { i: 0, colour: this.token('--antique-gold'), fill: true },
    ]) {
      const pts = [];
      slice.forEach((f, i) => {
        const a = f.lat && f.lat.NEW_REST;
        if (a && a[s.i]) pts.push([x(i), y(a[s.i])]);
      });
      if (pts.length < 2) continue;
      if (s.fill) {
        ctx.beginPath();
        pts.forEach(([a, b], k) => k ? ctx.lineTo(a, b) : ctx.moveTo(a, b));
        ctx.lineTo(pts[pts.length - 1][0], h - padB);
        ctx.lineTo(pts[0][0], h - padB);
        ctx.closePath();
        const g = ctx.createLinearGradient(0, padT, 0, h - padB);
        g.addColorStop(0, this.alpha(s.colour, 0.22));
        g.addColorStop(1, this.alpha(s.colour, 0));
        ctx.fillStyle = g; ctx.fill();
      }
      ctx.beginPath();
      pts.forEach(([a, b], k) => k ? ctx.lineTo(a, b) : ctx.moveTo(a, b));
      ctx.strokeStyle = s.colour; ctx.lineWidth = 1.3; ctx.stroke();
    }

    const cur = slice[slice.length - 1].lat && slice[slice.length - 1].lat.NEW_REST;
    this.el.latNow.textContent = cur ? `p50 ${cur[0]} · p99 ${cur[1]} · p99.9 ${cur[2]} ns` : '';
  }

  renderFlow(idx, f) {
    const c = this.el.flow, ctx = this.fitCanvas(c);
    const w = c._w, h = c._h, padT = 4, padB = 3;

    const slice = this.frames.slice(0, idx + 1);
    const maxV = Math.max(1, ...slice.map(s => Math.max(s.flow.buyQty, s.flow.sellQty)));
    // Never sample more points than the canvas has pixels.
    const step = Math.max(1, Math.ceil(slice.length / w));
    const x = i => (i / Math.max(1, slice.length - 1)) * w;
    const y = v => padT + (1 - v / maxV) * (h - padT - padB);

    const area = (get, colour) => {
      const trace = () => {
        ctx.beginPath();
        let started = false;
        for (let i = 0; i < slice.length; i += step) {
          const a = x(i), b = y(get(slice[i]));
          started ? ctx.lineTo(a, b) : (ctx.moveTo(a, b), started = true);
        }
        ctx.lineTo(x(slice.length - 1), y(get(slice[slice.length - 1])));
      };
      trace();
      ctx.lineTo(x(slice.length - 1), h - padB);
      ctx.lineTo(0, h - padB);
      ctx.closePath();
      const g = ctx.createLinearGradient(0, padT, 0, h - padB);
      g.addColorStop(0, this.alpha(colour, 0.24));
      g.addColorStop(1, this.alpha(colour, 0));
      ctx.fillStyle = g; ctx.fill();
      trace();
      ctx.strokeStyle = colour; ctx.lineWidth = 1.3; ctx.stroke();
    };
    area(s => s.flow.buyQty, this.token('--antique-gold'));
    area(s => s.flow.sellQty, this.token('--sell'));

    const fl = f.flow;
    const cell = (label, value, colour) =>
      `<div><b${colour ? ` style="color:${colour}"` : ''}>${commas(value)}</b>${label}</div>`;
    this.el.flowStats.innerHTML =
      cell('buy volume', fl.buyQty, 'var(--antique-gold)') +
      cell('sell volume', fl.sellQty, 'var(--sell)') +
      cell('trades', fl.trades) +
      cell('new', fl.new) +
      cell('cancel', fl.cancel) +
      cell('reduce', fl.reduce);
  }

  renderTape(f, idx) {
    if (idx < this._lastIdx) this.tape = [];   // scrubbed backwards: rebuild
    this._lastIdx = idx;
    if (f.tape && f.tape.length) {
      for (const t of f.tape) this.tape.unshift(t);
      this.tape = this.tape.slice(0, 40);
    }
    this.el.tape.innerHTML = this.tape.length
      ? this.tape.map(([, price, qty, side]) =>
          `<div class="trow ${side === 'B' ? 'B' : 'A'}">
             <span class="tt">${side === 'B' ? 'buy' : 'sell'}</span>
             <span class="tpx">${fmt(price)}</span>
             <span class="tq">${commas(qty)}</span>
           </div>`).join('')
      : '<div class="empty">no executions yet</div>';
  }
}

/* ------------------------------------------------------------------ */

const panelsEl = document.getElementById('panels');
const playBtn = document.getElementById('playPause');
const scrub = document.getElementById('scrub');
const positionEl = document.getElementById('position');
const speedEl = document.getElementById('speed');

const cache = {};
let panels = [], frameCount = 0, cursor = 0;
let playing = false, raf = null, acc = 0, lastT = 0;

async function get(key) {
  if (!cache[key]) cache[key] = await loadDataset(key);
  return cache[key];
}

async function setMode(mode) {
  stop();
  panelsEl.classList.toggle('dual', mode === 'both');
  panelsEl.innerHTML = '<div class="loading">loading snapshots…</div>';

  const keys = mode === 'both' ? ['real', 'synth'] : [mode];
  let datasets;
  try {
    datasets = await Promise.all(keys.map(get));
  } catch (err) {
    panelsEl.innerHTML =
      `<div class="loading">Could not load snapshot data.<br><code>${err.message}</code><br><br>` +
      `Generate it with <code>snapshot_recorder --input data/interchange/&lt;file&gt;.csv</code>, ` +
      `and serve this directory over HTTP — fetch() cannot read file:// URLs.</div>`;
    return;
  }

  panelsEl.innerHTML = '';
  const tpl = document.getElementById('panelTemplate');
  panels = datasets.map(d => {
    const node = tpl.content.firstElementChild.cloneNode(true);
    panelsEl.appendChild(node);
    return new Panel(node, d);
  });

  frameCount = Math.max(...panels.map(p => p.frames.length));
  scrub.max = String(frameCount - 1);
  seek(0);
}

function seek(i) {
  cursor = Math.max(0, Math.min(i, frameCount - 1));
  scrub.value = String(cursor);
  for (const p of panels) {
    // Compare mode runs both books on one cursor even though the synthetic
    // run is far shorter, so the shorter one is sampled proportionally.
    const scaled = p.frames.length === frameCount
      ? cursor
      : Math.floor((cursor * (p.frames.length - 1)) / Math.max(1, frameCount - 1));
    p.render(scaled);
  }
  const f = panels[0] && panels[0].frames[Math.min(cursor, panels[0].frames.length - 1)];
  positionEl.textContent = f
    ? `${commas(f.seq)} events · ${commas(cursor + 1)}/${commas(frameCount)}`
    : `${commas(cursor + 1)} / ${commas(frameCount)}`;
}

/*
 * requestAnimationFrame, not setInterval: the browser drives the cadence, so
 * playback cannot queue frames faster than it can paint them. Speed advances
 * more snapshots per painted frame rather than shortening a timer interval,
 * which is what keeps 16x smooth instead of stuttering.
 */
function loop(t) {
  if (!playing) return;
  if (!lastT) lastT = t;
  acc += ((t - lastT) / 1000) * 30 * Number(speedEl.value); // 30 frames/sec at 1x
  lastT = t;
  if (acc >= 1) {
    const advance = Math.floor(acc);
    acc -= advance;
    if (cursor + advance >= frameCount - 1) { seek(frameCount - 1); stop(); return; }
    seek(cursor + advance);
  }
  raf = requestAnimationFrame(loop);
}

function play() {
  if (playing || frameCount === 0) return;
  if (cursor >= frameCount - 1) seek(0);
  playing = true; acc = 0; lastT = 0;
  playBtn.textContent = '❚❚';
  raf = requestAnimationFrame(loop);
}

function stop() {
  playing = false;
  playBtn.textContent = '▶';
  if (raf) { cancelAnimationFrame(raf); raf = null; }
}

playBtn.addEventListener('click', () => (playing ? stop() : play()));
scrub.addEventListener('input', e => { stop(); seek(Number(e.target.value)); });

document.querySelectorAll('.seg').forEach(btn => {
  btn.addEventListener('click', () => {
    document.querySelectorAll('.seg').forEach(b => b.classList.remove('active'));
    btn.classList.add('active');
    setMode(btn.dataset.mode);
  });
});

const themeToggle = document.getElementById('themeToggle');
if (localStorage.getItem('leka-theme') === 'light') {
  document.documentElement.setAttribute('data-theme', 'light');
}
themeToggle.addEventListener('click', () => {
  const isLight = document.documentElement.getAttribute('data-theme') === 'light';
  if (isLight) {
    document.documentElement.removeAttribute('data-theme');
    localStorage.setItem('leka-theme', 'dark');
  } else {
    document.documentElement.setAttribute('data-theme', 'light');
    localStorage.setItem('leka-theme', 'light');
  }
  seek(cursor); // canvases sample the tokens at draw time
});

let resizeTimer = null;
window.addEventListener('resize', () => {
  clearTimeout(resizeTimer);
  resizeTimer = setTimeout(() => seek(cursor), 120);
});

setMode('real');
