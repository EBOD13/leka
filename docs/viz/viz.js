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
  real:  { file: 'data/real_aapl.jsonl',    name: 'Real — Nasdaq BX AAPL' },
  synth: { file: 'data/synth_hawkes.jsonl', name: 'Synthetic — Hawkes' },
};

/** Prices are integers in 1/10000 units, matching lob::Price's own encoding. */
const px = raw => (raw / 10000).toFixed(2);
const commas = n => n.toLocaleString('en-US');

async function loadDataset(key) {
  const res = await fetch(DATASETS[key].file);
  if (!res.ok) throw new Error(`cannot load ${DATASETS[key].file} (${res.status})`);
  const lines = (await res.text()).split('\n').filter(Boolean);
  const meta = JSON.parse(lines[0]);
  const frames = lines.slice(1).map(JSON.parse);
  return { meta, frames };
}

/** One dataset rendered into one panel: ladder, latency, flow, tape. */
class Panel {
  constructor(root, data) {
    this.root = root;
    this.meta = data.meta;
    this.frames = data.frames;
    this.tape = [];

    root.querySelector('.title').textContent = this.meta.label;
    this.el = {
      bb: root.querySelector('.bb'),
      ba: root.querySelector('.ba'),
      sp: root.querySelector('.sp'),
      levels: root.querySelector('.levels'),
      ladder: root.querySelector('.ladder'),
      latency: root.querySelector('canvas.latency'),
      latNow: root.querySelector('.lat-now'),
      flow: root.querySelector('canvas.flow'),
      flowStats: root.querySelector('.flowstats'),
      tape: root.querySelector('.tape'),
    };
    this.fitCanvas(this.el.latency);
    this.fitCanvas(this.el.flow);
  }

  /** Scales the backing store for devicePixelRatio so lines stay crisp. */
  fitCanvas(canvas) {
    const dpr = window.devicePixelRatio || 1;
    const rect = canvas.getBoundingClientRect();
    const w = rect.width || canvas.width;
    const h = canvas.height;
    canvas.width = Math.round(w * dpr);
    canvas.height = Math.round(h * dpr);
    canvas.style.height = h + 'px';
    const ctx = canvas.getContext('2d');
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    canvas._w = w;
    canvas._h = h;
  }

  render(i) {
    const f = this.frames[Math.min(i, this.frames.length - 1)];
    if (!f) return;
    this.renderQuotes(f);
    this.renderLadder(f);
    this.renderLatency(i);
    this.renderFlow(i, f);
    this.renderTape(f, i);
  }

  renderQuotes(f) {
    this.el.bb.textContent = f.bb ? px(f.bb) : '—';
    this.el.ba.textContent = f.ba ? px(f.ba) : '—';
    this.el.sp.textContent = (f.bb && f.ba) ? px(f.ba - f.bb) : '—';
    this.el.levels.textContent = `${f.nb}×${f.na}`;
  }

  renderLadder(f) {
    // Asks descend toward the spread, bids descend away from it, so the two
    // best quotes meet in the middle -- the conventional trading-terminal
    // layout rather than two separate lists.
    const maxQty = Math.max(
      1,
      ...f.bids.map(l => l[1]),
      ...f.asks.map(l => l[1]),
    );
    const row = (lvl, side) => {
      const [price, qty, orders] = lvl;
      const w = (qty / maxQty) * 100;
      return `<div class="lrow ${side}">
          <span class="px">${px(price)}</span>
          <span class="bar"><span class="fill" style="width:${w.toFixed(1)}%"></span></span>
          <span class="qty">${commas(qty)}</span>
        </div>`;
    };

    const asks = f.asks.slice().reverse().map(l => row(l, 'a')).join('');
    const bids = f.bids.map(l => row(l, 'b')).join('');
    const spread = (f.bb && f.ba)
      ? `<div class="spreadrow">${px(f.ba - f.bb)} spread</div>`
      : `<div class="spreadrow">one-sided</div>`;

    this.el.ladder.innerHTML =
      (asks || '<div class="empty">no resting asks</div>') +
      spread +
      (bids || '<div class="empty">no resting bids</div>');
  }

  /** Reads a CSS custom property so canvas drawing follows the active theme. */
  token(name) {
    return getComputedStyle(document.documentElement).getPropertyValue(name).trim();
  }

  renderLatency(idx) {
    const c = this.el.latency;
    this.fitCanvas(c);
    const ctx = c.getContext('2d');
    const w = c._w, h = c._h, pad = 2;
    ctx.clearRect(0, 0, w, h);

    const window = 240;
    const from = Math.max(0, idx - window + 1);
    const slice = this.frames.slice(from, idx + 1);
    if (!slice.length) return;

    // Log scale: the whole point is that p50 and the tail differ by orders
    // of magnitude, which a linear axis would flatten into one line.
    const vals = [];
    for (const f of slice) {
      for (const k of Object.keys(f.lat || {})) vals.push(...f.lat[k]);
    }
    const finite = vals.filter(v => v > 0);
    if (!finite.length) return;
    const lo = Math.log10(Math.max(10, Math.min(...finite)));
    const hi = Math.log10(Math.max(...finite) * 1.2);
    const y = v => h - pad - ((Math.log10(Math.max(v, 10)) - lo) / (hi - lo || 1)) * (h - pad * 2);
    const x = i => pad + (i / Math.max(1, slice.length - 1)) * (w - pad * 2);

    // gridlines at decade boundaries
    ctx.strokeStyle = this.token('--rail');
    ctx.lineWidth = 1;
    ctx.fillStyle = this.token('--subtle-text');
    ctx.font = "8.5px 'JetBrains Mono', ui-monospace, monospace";
    for (let d = Math.ceil(lo); d <= Math.floor(hi); d++) {
      const yy = y(Math.pow(10, d));
      ctx.beginPath(); ctx.moveTo(pad, yy); ctx.lineTo(w - pad, yy); ctx.stroke();
      const v = Math.pow(10, d);
      ctx.fillText(v >= 1000 ? `${v / 1000}µs` : `${v}ns`, pad + 1, yy - 3);
    }

    // NEW_REST is the representative resting-order path; drawing every event
    // type at every percentile would be twelve overlapping lines.
    const series = [
      { i: 0, color: this.token('--antique-gold') },
      { i: 1, color: this.token('--warm-amber') },
      { i: 2, color: this.token('--sell') },
    ];
    for (const s of series) {
      ctx.strokeStyle = s.color;
      ctx.lineWidth = 1.4;
      ctx.beginPath();
      let started = false;
      slice.forEach((f, i) => {
        const arr = f.lat && f.lat.NEW_REST;
        if (!arr || !arr[s.i]) return;
        const yy = y(arr[s.i]);
        if (!started) { ctx.moveTo(x(i), yy); started = true; }
        else ctx.lineTo(x(i), yy);
      });
      ctx.stroke();
    }

    const cur = slice[slice.length - 1].lat && slice[slice.length - 1].lat.NEW_REST;
    this.el.latNow.textContent = cur
      ? `NEW_REST  p50 ${cur[0]}  p99 ${cur[1]}  p99.9 ${cur[2]} ns`
      : '';
  }

  renderFlow(idx, f) {
    const c = this.el.flow;
    this.fitCanvas(c);
    const ctx = c.getContext('2d');
    const w = c._w, h = c._h, pad = 2;
    ctx.clearRect(0, 0, w, h);

    const slice = this.frames.slice(0, idx + 1);
    const maxV = Math.max(1, ...slice.map(s => Math.max(s.flow.buyQty, s.flow.sellQty)));
    const x = i => pad + (i / Math.max(1, slice.length - 1)) * (w - pad * 2);
    const y = v => h - pad - (v / maxV) * (h - pad * 2);

    const line = (get, color) => {
      ctx.strokeStyle = color; ctx.lineWidth = 1.4; ctx.beginPath();
      slice.forEach((s, i) => i ? ctx.lineTo(x(i), y(get(s))) : ctx.moveTo(x(i), y(get(s))));
      ctx.stroke();
    };
    line(s => s.flow.buyQty, this.token('--antique-gold'));
    line(s => s.flow.sellQty, this.token('--sell'));

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
    // Rebuild from scratch on a backwards scrub, otherwise append.
    if (idx < this._lastIdx) this.tape = [];
    this._lastIdx = idx;
    if (f.tape && f.tape.length) {
      for (const t of f.tape) this.tape.unshift(t);
      this.tape = this.tape.slice(0, 40);
    }
    this.el.tape.innerHTML = this.tape.length
      ? this.tape.map(([ts, price, qty, side]) =>
          `<div class="trow ${side === 'B' ? 'B' : 'A'}">
             <span class="tt">${side === 'B' ? 'buy' : 'sell'}</span>
             <span class="tpx">${px(price)}</span>
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
let panels = [];
let frameCount = 0;
let cursor = 0;
let playing = false;
let timer = null;

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
      `<div class="loading">Could not load snapshot data.<br><code>${err.message}</code>` +
      `<br><br>Regenerate with <code>snapshot_recorder --input &lt;interchange.csv&gt;</code>, ` +
      `and serve this directory over HTTP (fetch() will not read file:// URLs).</div>`;
    return;
  }

  panelsEl.innerHTML = '';
  const tpl = document.getElementById('panelTemplate');
  panels = datasets.map(d => {
    const node = tpl.content.firstElementChild.cloneNode(true);
    panelsEl.appendChild(node);
    return new Panel(node, d);
  });

  // Side by side runs on a shared cursor, so the two books advance together
  // even though the synthetic run is far shorter than the real session.
  frameCount = Math.max(...panels.map(p => p.frames.length));
  scrub.max = String(frameCount - 1);
  seek(0);
}

function seek(i) {
  cursor = Math.max(0, Math.min(i, frameCount - 1));
  scrub.value = String(cursor);
  for (const p of panels) {
    const scaled = p.frames.length === frameCount
      ? cursor
      : Math.floor(cursor * (p.frames.length - 1) / Math.max(1, frameCount - 1));
    p.render(scaled);
  }
  const f = panels[0] && panels[0].frames[Math.min(cursor, panels[0].frames.length - 1)];
  positionEl.textContent = f
    ? `${commas(f.seq)} events · frame ${commas(cursor + 1)}/${commas(frameCount)}`
    : `${commas(cursor + 1)} / ${commas(frameCount)}`;
}

function tick() {
  if (cursor >= frameCount - 1) { stop(); return; }
  seek(cursor + 1);
}

function play() {
  if (playing) return;
  playing = true;
  playBtn.textContent = '❚❚';
  timer = setInterval(tick, Math.max(8, 100 / Number(speedEl.value)));
}

function stop() {
  playing = false;
  playBtn.textContent = '▶';
  if (timer) { clearInterval(timer); timer = null; }
}

playBtn.addEventListener('click', () => (playing ? stop() : play()));
scrub.addEventListener('input', e => { stop(); seek(Number(e.target.value)); });
speedEl.addEventListener('change', () => { if (playing) { stop(); play(); } });

document.querySelectorAll('.seg').forEach(btn => {
  btn.addEventListener('click', () => {
    document.querySelectorAll('.seg').forEach(b => b.classList.remove('active'));
    btn.classList.add('active');
    setMode(btn.dataset.mode);
  });
});

// Theme toggle. Persisted so a reload keeps the reader's choice; the CSS
// token pairs do all the actual work, so this only flips one attribute and
// then asks the panels to redraw (canvas colors are read from the tokens).
const themeToggle = document.getElementById('themeToggle');
const savedTheme = localStorage.getItem('leka-theme');
if (savedTheme === 'light') document.documentElement.setAttribute('data-theme', 'light');
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
