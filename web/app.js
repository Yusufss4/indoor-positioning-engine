"use strict";

const COLORS = {
  truth: "#222", pf: "#d62728", ekf: "#1f77b4", wcl: "#2ca02c", beacon: "#7f3fbf",
  wall: "rgba(29, 78, 137, 0.45)", ink: "#1d4e89", paper: "#fbfcfe",
};
const LABEL_FONT = '500 11px "Barlow Condensed", "Arial Narrow", sans-serif';
const FILTERS = ["pf", "ekf", "wcl"];
const FRAME_DT = 0.05; // simulated seconds per recorded frame
const PARTICLE_EVERY = 4; // frames per particle snapshot (5 Hz)
const MAX_RECORDED_PARTICLES = 1000;
const COMPUTE_BUDGET_MS = 25; // engine time per animation frame when running ahead
const TRAIL_START_S = 3;
const PING_S = 0.6;

const icon = (shapes) => `<svg viewBox="0 0 12 12" aria-hidden="true">${shapes}</svg>`;
const ICONS = {
  "jump-start": icon('<rect x="1" y="2" width="1.5" height="8"/><path d="M7 2 2.5 6 7 10z M11.5 2 7 6 11.5 10z"/>'),
  "step-back": icon('<rect x="1.5" y="2" width="1.5" height="8"/><path d="M10 2 3.5 6 10 10z"/>'),
  "play-rev": icon('<path d="M10 2 2 6 10 10z"/>'),
  play: icon('<path d="M2 2 10 6 2 10z"/>'),
  pause: icon('<rect x="2.5" y="2" width="2.5" height="8"/><rect x="7" y="2" width="2.5" height="8"/>'),
  "play-fwd": icon('<path d="M2 2 10 6 2 10z"/>'),
  "step-fwd": icon('<path d="M2 2 8.5 6 2 10z"/><rect x="9" y="2" width="1.5" height="8"/>'),
  "jump-end": icon('<path d="M0.5 2 5 6 0.5 10z M5 2 9.5 6 5 10z"/><rect x="9.5" y="2" width="1.5" height="8"/>'),
};

const el = (id) => document.getElementById(id);
const canvas = el("map");
const ctx = canvas.getContext("2d");

let Module = null;
let demo = null;
let venue = null;
let geojsonUrl = null;
let playing = true;
let direction = 1; // +1 forwards, -1 in reverse
let lastFrame = null;
let recorded = []; // engine frames so far; recorded[k] is at k * FRAME_DT
let viewT = 0; // time on screen; frames past the last recorded one are computed on demand
let view = "3d";
let map3d = null;
let map3dReady = null; // promise of map3d.init(), shared by overlapping show3d() calls
let show3dRequest = 0;
let realDemos = new Map(); // id -> entry of data/real/demos.json
let real = null; // entry being replayed, null for simulated scenarios
let background = null; // floor plan image of a recording
let waypoints = []; // [t, x, y] surveyor waypoints of a recording
let zoom = { k: 1, x: 0, y: 0 }; // 2D view: canvas px = k * fitted px + (x, y)
let roomLog = []; // room enter/exit events recorded so far, oldest first
let roomLogKey = null; // what the rooms panel currently shows

const REAL_BASE = new URL("data/real/", window.location.href);

async function loadJson(url) {
  const res = await fetch(url);
  if (!res.ok) throw new Error(`${url}: ${res.status}`);
  return res.text();
}

function loadImage(url) {
  return new Promise((resolve, reject) => {
    const img = new Image();
    img.onload = () => resolve(img);
    img.onerror = () => reject(new Error(`${url}: image failed to load`));
    img.src = url;
  });
}

/// Adds the recordings listed in data/real/demos.json to the scenario menu (if the file exists).
async function loadRealDemos() {
  let index;
  try {
    index = JSON.parse(await loadJson(new URL("demos.json", REAL_BASE)));
  } catch {
    return;
  }
  const group = document.createElement("optgroup");
  group.label = "Real recordings (Microsoft Indoor Location)";
  for (const d of index.demos) {
    realDemos.set(d.id, d);
    group.append(new Option(d.title, `real:${d.id}`));
  }
  el("scenario").append(group);
}

function parseWaypoints(traceText) {
  return traceText.split("\n").filter((l) => l.startsWith("gt,")).map((l) => l.split(",").slice(1, 4).map(Number));
}

/// Venue JSON in either layout (top-level walls, or levels) -> one shape.
function normalizeVenue(v) {
  const levels = (v.levels || [{ ordinal: 0, name: "", elevation_m: 0, height_m: 4, walls: v.walls || [] }])
    .map((l) => ({ ordinal: l.ordinal, name: l.name || `L${l.ordinal}`, walls: l.walls || [] }))
    .sort((a, b) => a.ordinal - b.ordinal);
  const index = new Map(levels.map((l, i) => [l.ordinal, i]));
  const levelOf = (ordinal) => index.get(ordinal ?? levels[0].ordinal);
  return {
    name: v.name,
    bounds: v.bounds,
    georef: v.georef || null,
    levels,
    beacons: v.beacons.map((b) => ({ ...b, level: levelOf(b.level) })),
    connectors: (v.connectors || []).map((c) => ({ ...c, levels: c.levels.map(levelOf) })),
    rooms: (v.rooms || []).map((r) => ({ ...r, name: r.name || r.id, level: levelOf(r.level) })),
  };
}

async function startSimulated(name) {
  const scenarioUrl = new URL(`data/scenarios/${name}.json`, window.location.href);
  const scenarioText = await loadJson(scenarioUrl);
  const scenario = JSON.parse(scenarioText);
  const venueUrl = new URL(scenario.venue, scenarioUrl);
  const venueText = await loadJson(venueUrl);
  const configText = scenario.demo_config ? await loadJson(new URL(scenario.demo_config, scenarioUrl)) : "";
  venue = normalizeVenue(JSON.parse(venueText));
  geojsonUrl = new URL(venueUrl.href.replace(/\/venues\/([^/]+)\.json$/, "/geojson/$1.geojson"));
  background = null;
  waypoints = [];
  el("description").textContent =
    (scenario.description || "") +
    (scenario.demo_config ? ` The particle filter uses ${scenario.demo_config.replace(/^(\.\.\/)+/, "data/")}.` : "");
  if (demo) demo.delete();
  demo = new Module.Demo(venueText, scenarioText, Number(el("seed").value) >>> 0, configText);
}

async function startRecording(entry) {
  const at = (path) => new URL(path, REAL_BASE);
  const [venueText, traceText, configText, image] = await Promise.all([
    loadJson(at(entry.venue)), loadJson(at(entry.trace)), loadJson(at(entry.config)), loadImage(at(entry.image)),
  ]);
  venue = normalizeVenue(JSON.parse(venueText));
  geojsonUrl = at(entry.geojson);
  background = image;
  waypoints = parseWaypoints(traceText);
  el("description").textContent =
    `${entry.title} with a real Android phone (IMU + Wi-Fi), held out from fitting and tuning. Squares are ` +
    `the surveyor's waypoints, the only ground truth; the black path is interpolated between them, and the ` +
    `live error is measured against it. Purple dots are Wi-Fi access points located from other walks. The ` +
    `particle filter uses data/configs/msil_wifi.json.`;
  if (demo) demo.delete();
  demo = Module.Demo.replay(venueText, traceText, configText);
}

async function start() {
  const name = el("scenario").value;
  real = name.startsWith("real:") ? realDemos.get(name.slice(5)) : null;
  if (real) await startRecording(real);
  else await startSimulated(name);
  el("seed").disabled = !!real;
  recorded = [];
  roomLog = [];
  roomLogKey = null;
  viewT = 0;
  lastFrame = null;
  demo.advance(0);
  record();
  el("timeline").max = timelineEnd().toFixed(2);
  setPlaying(true, 1);
  el("rooms-panel").hidden = venue.rooms.length === 0;

  const floorSel = el("floor");
  floorSel.innerHTML = `<option value="auto">follow walker</option>` +
    venue.levels.map((l, i) => `<option value="${i}">${l.name}</option>`).join("");
  el("floor-control").hidden = venue.levels.length < 2;
  el("floor-head").hidden = venue.levels.length < 2;

  if (view === "3d") await show3d();
}

/// Subsampled copy of the particle view (x, y, w, level per particle), which aliases WASM memory.
function sampleParticles(p) {
  const n = p.length / 4;
  const stride = Math.max(1, Math.ceil(n / MAX_RECORDED_PARTICLES));
  const out = new Float32Array(Math.ceil(n / stride) * 4);
  for (let k = 0, j = 0; k < n; k += stride, j += 4) {
    out[j] = p[4 * k];
    out[j + 1] = p[4 * k + 1];
    out[j + 2] = p[4 * k + 2];
    out[j + 3] = p[4 * k + 3];
  }
  return out;
}

/// Appends the engine state after the last advance() to `recorded`.
function record() {
  const k = recorded.length;
  const est = {};
  const rooms = { truth: demo.currentRooms("truth") };
  for (const f of FILTERS) {
    est[f] = demo.estimate(f);
    rooms[f] = demo.currentRooms(f);
  }
  recorded.push({
    t: demo.time(),
    steps: demo.steps(),
    truth: demo.truth(),
    est,
    rooms,
    ble: demo.recentBle().map((b) => b.id),
    particles: k % PARTICLE_EVERY === 0 ? sampleParticles(demo.particles()) : recorded[k - 1].particles,
  });
  roomLog.push(...demo.roomEvents());
}

const frameIndex = (t) => Math.max(0, Math.floor(t / FRAME_DT + 1e-6));
const lastRecorded = () => recorded[recorded.length - 1].t;
/// Latest time the timeline can reach: the end of the recording once the engine has finished.
const maxTime = () => (demo.finished() ? lastRecorded() : timelineEnd());
const timelineEnd = () => (Math.floor(demo.duration() / FRAME_DT) + 1) * FRAME_DT;
const computing = () => recorded.length <= frameIndex(viewT) && !demo.finished();

/// The recorded frame shown at viewT (the newest one while the engine is still catching up).
function shownFrame() {
  return Math.min(recorded.length - 1, frameIndex(viewT));
}

/// Advances the engine until frame `index` is recorded, the trace ends or the time budget runs out.
function computeUntil(index) {
  const deadline = performance.now() + COMPUTE_BUDGET_MS;
  while (recorded.length <= index && !demo.finished() && performance.now() < deadline) {
    demo.advance(FRAME_DT);
    record();
  }
}

function shownLevel(fr) {
  const v = el("floor").value;
  return v === "auto" ? fr.truth.level : Number(v);
}

function transform() {
  const [x0, y0, x1, y1] = real ? real.view : venue.bounds;
  const pad = 20;
  const s = Math.min((canvas.width - 2 * pad) / (x1 - x0), (canvas.height - 2 * pad) / (y1 - y0));
  const ox = pad + ((canvas.width - 2 * pad) - s * (x1 - x0)) / 2;
  const oy = pad + ((canvas.height - 2 * pad) - s * (y1 - y0)) / 2;
  const { k, x: zx, y: zy } = zoom;
  return {
    s: s * k,
    toX: (x) => k * (ox + (x - x0) * s) + zx,
    toY: (y) => k * (canvas.height - (oy + (y - y0) * s)) + zy,
  };
}

/// Zooms the 2D view by `factor` about canvas point (px, py).
function zoomAt(factor, px, py) {
  const k = Math.min(60, Math.max(0.5, zoom.k * factor));
  const f = k / zoom.k;
  zoom = { k, x: px - (px - zoom.x) * f, y: py - (py - zoom.y) * f };
}

function canvasPoint(ev) {
  const r = canvas.getBoundingClientRect();
  return [((ev.clientX - r.left) * canvas.width) / r.width, ((ev.clientY - r.top) * canvas.height) / r.height];
}

/// Draws the trail through recorded[0..last] that lies on `level`; `at(frame)` gives the {x, y, level} point
/// of a frame or null for a gap.
function drawPath(last, at, T, color, width, level) {
  ctx.strokeStyle = color;
  ctx.lineWidth = width;
  ctx.beginPath();
  let pen = false;
  for (let k = 0; k <= last; ++k) {
    const p = at(recorded[k]);
    if (!p || p.level !== level) {
      pen = false;
      continue;
    }
    if (pen) ctx.lineTo(T.toX(p.x), T.toY(p.y));
    else ctx.moveTo(T.toX(p.x), T.toY(p.y));
    pen = true;
  }
  ctx.stroke();
}

function dot(x, y, T, color, r, ring = false, hollow = false) {
  ctx.beginPath();
  ctx.arc(T.toX(x), T.toY(y), r, 0, 2 * Math.PI);
  if (hollow) {
    ctx.strokeStyle = color;
    ctx.lineWidth = 2;
    ctx.setLineDash([3, 3]);
    ctx.stroke();
    ctx.setLineDash([]);
    return;
  }
  ctx.fillStyle = color;
  ctx.fill();
  if (ring) {
    ctx.strokeStyle = COLORS.paper;
    ctx.lineWidth = 2;
    ctx.stroke();
  }
}

/// Metric grid that pans and zooms with the map; spacing picks the first step at least 32 px wide.
function drawGrid(T) {
  const metres = [1, 2, 5, 10, 20, 50, 100].find((m) => m * T.s >= 32) || 100;
  const step = metres * T.s;
  const x0 = T.toX(0) - Math.ceil(T.toX(0) / step) * step;
  const y0 = T.toY(0) - Math.ceil(T.toY(0) / step) * step;
  ctx.strokeStyle = "rgba(29, 78, 137, 0.07)";
  ctx.lineWidth = 1;
  ctx.beginPath();
  for (let x = x0; x <= canvas.width; x += step) {
    ctx.moveTo(Math.round(x) + 0.5, 0);
    ctx.lineTo(Math.round(x) + 0.5, canvas.height);
  }
  for (let y = y0; y <= canvas.height; y += step) {
    ctx.moveTo(0, Math.round(y) + 0.5);
    ctx.lineTo(canvas.width, Math.round(y) + 0.5);
  }
  ctx.stroke();
  ctx.fillStyle = COLORS.ink;
  ctx.font = LABEL_FONT;
  ctx.fillText(`GRID ${metres} M`, 8, canvas.height - 8);
}

/// Room outlines on `level`: a faint fill, the particle filter's current rooms tinted red, the true ones
/// outlined, and a name wherever it fits inside the room's on-screen box.
function drawRooms(T, level, fr) {
  if (!venue.rooms.length) return;
  const pfIn = new Set(el("show-pf").checked ? fr.rooms.pf : []);
  const truthIn = new Set(fr.rooms.truth);
  ctx.font = LABEL_FONT;
  venue.rooms.forEach((r, i) => {
    if (r.level !== level) return;
    ctx.beginPath();
    r.area.forEach(([x, y], k) => (k ? ctx.lineTo(T.toX(x), T.toY(y)) : ctx.moveTo(T.toX(x), T.toY(y))));
    ctx.closePath();
    ctx.fillStyle = pfIn.has(i) ? "rgba(214, 39, 40, 0.14)" : "rgba(29, 78, 137, 0.035)";
    ctx.fill();
    if (truthIn.has(i)) {
      ctx.strokeStyle = "rgba(34, 34, 34, 0.55)";
      ctx.lineWidth = 1.5;
      ctx.setLineDash([5, 4]);
      ctx.stroke();
      ctx.setLineDash([]);
    }
    const xs = r.area.map((p) => T.toX(p[0]));
    const ys = r.area.map((p) => T.toY(p[1]));
    const [x0, x1, y0, y1] = [Math.min(...xs), Math.max(...xs), Math.min(...ys), Math.max(...ys)];
    const w = ctx.measureText(r.name).width;
    if (x1 - x0 < w + 8 || y1 - y0 < 16) return;
    ctx.fillStyle = "rgba(29, 78, 137, 0.55)";
    ctx.fillText(r.name, (x0 + x1 - w) / 2, (y0 + y1) / 2 + 4);
  });
}

/// Number of room events at or before time t (roomLog is sorted by time).
function roomEventsUntil(t) {
  let lo = 0;
  let hi = roomLog.length;
  while (lo < hi) {
    const mid = (lo + hi) >> 1;
    if (roomLog[mid].t <= t) lo = mid + 1;
    else hi = mid;
  }
  return lo;
}

/// Rooms panel as of frame `fr`: events up to fr.t, newest first, and the current rooms.
function renderRoomLog(fr) {
  if (!venue.rooms.length) return;
  const count = roomEventsUntil(fr.t);
  const shown = new Set(["truth", ...visibleFilters()]);
  const key = `${count}|${[...shown]}|${JSON.stringify(fr.rooms)}`;
  if (key === roomLogKey) return;
  roomLogKey = key;
  const fmt = (r) => venue.levels.length > 1 ? `${r.name} (${venue.levels[r.level].name})` : r.name;
  const items = [];
  for (let k = count - 1; k >= 0 && items.length < 60; --k) {
    if (shown.has(roomLog[k].filter)) items.push(roomLog[k]);
  }
  el("room-log").innerHTML = items.map((e) =>
    `<li class="${e.type}"><span class="t">${e.t.toFixed(1)} s</span>` +
    `<span class="swatch" style="background:${COLORS[e.filter]}"></span>${e.filter} ` +
    `<b>${e.type}</b> ${escapeHtml(fmt(venue.rooms[e.room]))}</li>`).join("");
  const now = (f) => fr.rooms[f].map((i) => escapeHtml(fmt(venue.rooms[i]))).join(", ") || "-";
  el("rooms-now").innerHTML = [...shown].map((f) =>
    `<span class="swatch" style="background:${COLORS[f]}"></span>${f}: ${now(f)}`).join("<br>");
}

function escapeHtml(s) {
  return s.replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[c]);
}

/// Beacon id -> time it was last heard within PING_S before frame `idx`.
function recentPings(idx) {
  const pings = new Map();
  const now = recorded[idx].t;
  for (let k = idx; k >= 0 && now - recorded[k].t < PING_S; --k) {
    for (const id of recorded[k].ble) if (!pings.has(id)) pings.set(id, recorded[k].t);
  }
  return pings;
}

function draw(level, idx) {
  const fr = recorded[idx];
  const T = transform();
  ctx.clearRect(0, 0, canvas.width, canvas.height);
  drawGrid(T);

  if (background) {
    const [bx0, by0, bx1, by1] = venue.bounds;
    ctx.globalAlpha = 0.3;
    ctx.filter = "grayscale(1)";
    ctx.drawImage(background, T.toX(bx0), T.toY(by1), (bx1 - bx0) * T.s, (by1 - by0) * T.s);
    ctx.filter = "none";
    ctx.globalAlpha = 1;
  }

  drawRooms(T, level, fr);

  for (const c of venue.connectors) {
    if (!c.levels.includes(level)) continue;
    ctx.beginPath();
    c.area.forEach(([x, y], k) => (k ? ctx.lineTo(T.toX(x), T.toY(y)) : ctx.moveTo(T.toX(x), T.toY(y))));
    ctx.closePath();
    ctx.fillStyle = c.kind === "elevator" ? "rgba(255, 159, 28, 0.3)" : "rgba(46, 196, 182, 0.3)";
    ctx.fill();
    ctx.fillStyle = COLORS.ink;
    ctx.font = LABEL_FONT;
    ctx.fillText(c.kind.toUpperCase(), T.toX(c.area[0][0]) + 3, T.toY(c.area[0][1]) - 3);
  }

  ctx.strokeStyle = COLORS.wall;
  ctx.lineWidth = 1.5;
  ctx.lineCap = "round";
  for (const [ax, ay, bx, by] of venue.levels[level].walls) {
    ctx.beginPath();
    ctx.moveTo(T.toX(ax), T.toY(ay));
    ctx.lineTo(T.toX(bx), T.toY(by));
    ctx.stroke();
  }

  const now = fr.t;
  const pings = recentPings(idx);
  // Recorded venues have hundreds of Wi-Fi access points: small dots, no labels.
  const dense = venue.beacons.length > 60;
  for (const b of venue.beacons) {
    if (b.level !== level) continue;
    const ping = pings.get(b.id);
    if (ping !== undefined) {
      const age = (now - ping) / PING_S;
      ctx.beginPath();
      ctx.arc(T.toX(b.x), T.toY(b.y), dense ? 3 + 9 * age : 6 + 22 * age, 0, 2 * Math.PI);
      ctx.strokeStyle = `rgba(127, 63, 191, ${(dense ? 0.3 : 0.5) * (1 - age)})`;
      ctx.lineWidth = 2;
      ctx.stroke();
    }
    ctx.fillStyle = COLORS.beacon;
    if (dense) {
      ctx.fillRect(T.toX(b.x) - 1.5, T.toY(b.y) - 1.5, 3, 3);
      continue;
    }
    ctx.beginPath();
    ctx.moveTo(T.toX(b.x), T.toY(b.y) - 7);
    ctx.lineTo(T.toX(b.x) - 6, T.toY(b.y) + 5);
    ctx.lineTo(T.toX(b.x) + 6, T.toY(b.y) + 5);
    ctx.closePath();
    ctx.fill();
    ctx.fillStyle = COLORS.ink;
    ctx.font = LABEL_FONT;
    ctx.fillText(b.id, T.toX(b.x) + 7, T.toY(b.y) - 6);
  }

  if (el("show-particles").checked) {
    const p = fr.particles;
    ctx.fillStyle = "rgba(214, 39, 40, 0.25)";
    for (let k = 0; k < p.length; k += 4) {
      if (p[k + 3] === level) ctx.fillRect(T.toX(p[k]) - 1, T.toY(p[k + 1]) - 1, 2, 2);
    }
  }

  drawPath(idx, (h) => h.truth, T, COLORS.truth, 2, level);
  ctx.fillStyle = COLORS.truth;
  for (const [t, x, y] of waypoints) {
    if (t > now) break;
    ctx.fillRect(T.toX(x) - 4, T.toY(y) - 4, 8, 8);
  }
  for (const f of visibleFilters()) {
    drawPath(idx, (h) => (h.t > TRAIL_START_S && h.est[f].valid ? h.est[f] : null), T, COLORS[f], 1.3, level);
    const e = fr.est[f];
    if (!e.valid) continue;
    // An estimate on another floor is drawn dashed: you see the miss, not a phantom on this floor.
    const here = e.level === level;
    if (here && e.sigma > 0) {
      ctx.beginPath();
      ctx.arc(T.toX(e.x), T.toY(e.y), Math.max(4, e.sigma * T.s), 0, 2 * Math.PI);
      ctx.fillStyle = COLORS[f] + "22";
      ctx.fill();
    }
    dot(e.x, e.y, T, COLORS[f], 6, true, !here);
  }
  const t = fr.truth;
  dot(t.x, t.y, T, COLORS.truth, 6, true, t.level !== level);
}

function visibleFilters() {
  return FILTERS.filter((f) => el(`show-${f}`).checked);
}

function updateStats(fr) {
  const t = fr.truth;
  const multi = venue.levels.length > 1;
  const levelText = multi ? `   floor: ${venue.levels[t.level].name}` : "";
  el("clock").textContent = `t = ${fr.t.toFixed(1)} / ${demo.duration().toFixed(0)} s   steps: ${fr.steps}` +
    `${levelText}${computing() ? "   computing..." : ""}`;
  const fmt = (v) => (v < 0 ? "-" : `${v.toFixed(2)} m`);
  const pct = (v) => (v < 0 ? "-" : `${(100 * v).toFixed(1)}%`);
  el("stats").innerHTML = FILTERS.map((f) => {
    const e = fr.est[f];
    const floor = multi ? `<td>${e.valid ? venue.levels[e.level].name : "-"} (${pct(e.floorAcc)})</td>` : "";
    return `<tr><td><span class="swatch" style="background:${COLORS[f]}"></span>${f}</td>` +
      `<td>${fmt(e.error)}</td><td>${fmt(e.mean)}</td><td>${e.cep95 > 0 ? fmt(e.cep95) : "-"}</td>${floor}</tr>`;
  }).join("");
}

function currentFrame(fr, level) {
  return {
    t: fr.t,
    truth: fr.truth,
    estimates: visibleFilters().map((f) => ({ name: f, color: COLORS[f], e: fr.est[f] })),
    particles: el("show-particles").checked ? fr.particles : null,
    rooms: el("show-pf").checked ? fr.rooms.pf.map((i) => venue.rooms[i].id) : [],
    focusLevel: level,
    levels: venue.levels,
  };
}

function tick(ts) {
  if (demo && venue && recorded.length) {
    if (playing) {
      const dt = lastFrame === null ? 0 : Math.min((ts - lastFrame) / 1000, 0.1);
      viewT += direction * dt * Number(el("speed").value);
    }
    viewT = Math.min(Math.max(viewT, 0), maxTime());
    computeUntil(frameIndex(viewT));
    // Playing forwards never runs ahead of the engine; a seek waits for it instead.
    if (playing && computing()) viewT = lastRecorded();
    if (playing && direction < 0 && viewT <= 0) setPlaying(false);
    if (playing && direction > 0 && demo.finished() && viewT >= lastRecorded()) setPlaying(false);
    lastFrame = ts;
    const idx = shownFrame();
    const fr = recorded[idx];
    const level = shownLevel(fr);
    if (view === "2d") draw(level, idx);
    if (view === "3d" && map3d) map3d.update(currentFrame(fr, level));
    updateStats(fr);
    renderRoomLog(fr);
    updateTimeline(fr);
  }
  requestAnimationFrame(tick);
}

function updateTimeline(fr) {
  const bar = el("timeline");
  const end = Number(bar.max);
  if (!bar.matches(":active")) bar.value = String(Math.min(viewT, end));
  bar.style.setProperty("--done", `${(100 * Math.min(lastRecorded(), end)) / end}%`);
  el("time-label").textContent = `${fr.t.toFixed(1)} / ${demo.duration().toFixed(1)} s`;
}

/// Starts or stops playback; `dir` (+1 or -1) changes the direction. Playing forwards from the end restarts.
function setPlaying(on, dir = direction) {
  direction = dir;
  if (on && dir > 0 && demo.finished() && viewT >= lastRecorded()) viewT = 0;
  if (on && dir < 0 && viewT <= 0) on = false;
  playing = on;
  el("play").innerHTML = playing ? ICONS.pause : ICONS.play;
  el("play").title = playing ? "Pause (Space)" : "Play (Space)";
  el("play-fwd").classList.toggle("active", playing && direction > 0);
  el("play-rev").classList.toggle("active", playing && direction < 0);
}

/// Pauses and shows the frame `frames` frames away from the current one.
function stepFrames(frames) {
  setPlaying(false);
  viewT = Math.min(Math.max(0, (shownFrame() + frames) * FRAME_DT), maxTime());
}

function seek(t) {
  setPlaying(false);
  viewT = Math.min(Math.max(0, t), maxTime());
}

async function show3d() {
  const status = el("view3d-status");
  const request = ++show3dRequest;
  try {
    status.textContent = "loading 3D view...";
    if (!map3dReady) {
      const m = new Map3D(el("map3d"));
      map3dReady = m.init().then(() => {
        m.exploded = Number(el("exploded").value);
        m.follow = el("follow").checked;
        map3d = m;
      });
    }
    await map3dReady;
    const res = await fetch(geojsonUrl);
    if (!res.ok) throw new Error(`no GeoJSON export for this venue (${geojsonUrl.pathname}); run ipe-import export`);
    const geojson = await res.json();
    if (request !== show3dRequest) return;
    map3d.setVenue(geojson);
    map3d.setUnderlay(el("underlay").checked);
    map3d.resize();
    status.textContent = "";
  } catch (err) {
    if (request !== show3dRequest) return;
    if (!map3d) map3dReady = null;
    status.textContent = `3D view unavailable: ${err.message}`;
    setView("2d");
  }
}

function setView(v) {
  view = v;
  el("map").hidden = v !== "2d";
  el("map3d").hidden = v !== "3d";
  el("controls3d").hidden = v !== "3d";
  el("zoom2d").hidden = v !== "2d";
  el("hint2d").hidden = v !== "2d";
  el("view2d").classList.toggle("active", v === "2d");
  el("view3d").classList.toggle("active", v === "3d");
}

const TRANSPORT = {
  "jump-start": () => seek(0),
  "step-back": () => stepFrames(-1),
  "play-rev": () => setPlaying(!(playing && direction < 0), -1),
  play: () => setPlaying(!playing),
  "play-fwd": () => setPlaying(!(playing && direction > 0), 1),
  "step-fwd": () => stepFrames(1),
  "jump-end": () => seek(lastRecorded()),
};
for (const [id, action] of Object.entries(TRANSPORT)) {
  el(id).innerHTML = ICONS[id];
  el(id).addEventListener("click", () => recorded.length && action());
}
el("timeline").addEventListener("input", () => recorded.length && seek(Number(el("timeline").value)));

const KEYS = {
  " ": () => setPlaying(!playing),
  ArrowLeft: (ev) => stepFrames(ev.shiftKey ? -1 : -Math.round(1 / FRAME_DT)),
  ArrowRight: (ev) => stepFrames(ev.shiftKey ? 1 : Math.round(1 / FRAME_DT)),
  j: () => setPlaying(true, -1),
  k: () => setPlaying(false),
  l: () => setPlaying(true, 1),
};
const handledKey = (ev) => {
  const t = ev.target;
  if (ev.ctrlKey || ev.metaKey || ev.altKey || t.isContentEditable) return null;
  if (["INPUT", "SELECT", "TEXTAREA"].includes(t.tagName)) return null;
  return KEYS[ev.key.length === 1 ? ev.key.toLowerCase() : ev.key] || null;
};
document.addEventListener("keydown", (ev) => {
  const action = handledKey(ev);
  if (!action || !recorded.length) return;
  ev.preventDefault();
  action(ev);
});
// A focused button would otherwise also activate on the Space keyup.
document.addEventListener("keyup", (ev) => handledKey(ev) && ev.preventDefault());

el("restart").addEventListener("click", start);
el("scenario").addEventListener("change", () => {
  zoom = { k: 1, x: 0, y: 0 };
  start();
});

const zoomCenter = (factor) => zoomAt(factor, canvas.width / 2, canvas.height / 2);
el("zoom-in").addEventListener("click", () => zoomCenter(1.5));
el("zoom-out").addEventListener("click", () => zoomCenter(1 / 1.5));
el("zoom-fit").addEventListener("click", () => (zoom = { k: 1, x: 0, y: 0 }));
canvas.addEventListener("wheel", (ev) => {
  ev.preventDefault();
  zoomAt(Math.exp(-ev.deltaY * (ev.deltaMode === 1 ? 0.05 : 0.0015)), ...canvasPoint(ev));
}, { passive: false });
canvas.addEventListener("dblclick", () => (zoom = { k: 1, x: 0, y: 0 }));
let drag = null;
canvas.addEventListener("pointerdown", (ev) => {
  drag = canvasPoint(ev);
  canvas.setPointerCapture(ev.pointerId);
  canvas.classList.add("dragging");
});
canvas.addEventListener("pointermove", (ev) => {
  if (!drag) return;
  const [px, py] = canvasPoint(ev);
  zoom = { ...zoom, x: zoom.x + px - drag[0], y: zoom.y + py - drag[1] };
  drag = [px, py];
});
const endDrag = () => {
  drag = null;
  canvas.classList.remove("dragging");
};
canvas.addEventListener("pointerup", endDrag);
canvas.addEventListener("pointercancel", endDrag);
el("view2d").addEventListener("click", () => setView("2d"));
el("view3d").addEventListener("click", async () => {
  setView("3d");
  await show3d();
});
el("exploded").addEventListener("input", () => map3d && map3d.setExploded(Number(el("exploded").value)));
el("follow").addEventListener("change", () => map3d && (map3d.follow = el("follow").checked));
el("underlay").addEventListener("change", () => map3d && map3d.setUnderlay(el("underlay").checked));

createIpeModule().then(async (m) => {
  Module = m;
  await loadRealDemos();
  await start();
  requestAnimationFrame(tick);
}).catch((err) => {
  el("clock").textContent = `failed to load: ${err}`;
});
