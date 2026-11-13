"use strict";

const COLORS = {
  truth: "#222", grid: "#2ca02c", beacon: "#7f3fbf", beaconUnmodelled: "#c9b3e3",
  wall: "rgba(29, 78, 137, 0.45)", ink: "#1d4e89", paper: "#fbfcfe",
};
const FILTER_TITLES = { grid: "Grid filter" };
const LABEL_FONT = '500 11px "Barlow Condensed", "Arial Narrow", sans-serif';
const FRAME_DT = 0.05; // simulated seconds per recorded frame (as in worker.js)
const TRAIL_START_S = 3;
const PING_S = 0.6;
const TRAIL_STEP_3D = 0.4; // metres between trail points in 3D
const INFO_REFRESH_MS = 200;

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

// The engine runs in a Web Worker (worker.js) and streams its frames here. run: {worker, duration, finished,
// filters (["grid"]), cellSize, cellCount}; null while a run starts.
let run = null;
let startSeq = 0; // the newest start() call; older ones stop at their next await
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
let realDemos = new Map(); // id -> entry of a recordings index (data/real*/demos.json), with its base URL
let real = null; // entry being replayed, null for simulated scenarios
let backgrounds = null; // floor plan image of a recording per level index (null: none)
let waypoints = []; // [t, x, y, level] surveyor waypoints of a recording
let zoom = { k: 1, x: 0, y: 0 }; // 2D view: canvas px = k * fitted px + (x, y)
let roomLog = []; // room enter/exit events recorded so far, oldest first
let selected = null; // clicked map item {kind, id}, shown in the info card
let infoRefresh = 0;
let lastFix = {}; // last valid estimate per filter, to break its trail where it jumps a wall
let roomLogKey = null; // what the rooms panel currently shows

/// Recording indexes, in menu order. Each lists venues, traces and floor plans relative to its folder.
const RECORDING_SOURCES = [
  { dir: "data/real/", label: "Real recordings (Microsoft Indoor Location)" },
  { dir: "data/real_ipin/", label: "Real recordings (IPIN competition, several floors)" },
  { dir: "data/real_uji/", label: "Real recordings (UJI BLE, standing still)" },
  // Written by datasets/kaggle/export_demo.py on a machine that has the competition data; never deployed.
  { dir: "data/real_kaggle/", label: "Real recordings (Kaggle indoor location, local only)" },
];

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

/// Adds the recordings of every index in RECORDING_SOURCES that exists to the scenario menu.
async function loadRealDemos() {
  for (const src of RECORDING_SOURCES) {
    const base = new URL(src.dir, window.location.href);
    let index;
    try {
      index = JSON.parse(await loadJson(new URL("demos.json", base)));
    } catch {
      continue;
    }
    // One menu group per index, or one per `group` of its entries (a building, where an index has many).
    const groups = new Map();
    for (const d of index.demos) {
      const label = d.group || index.menu || src.label;
      if (!groups.has(label)) {
        const group = document.createElement("optgroup");
        group.label = label;
        groups.set(label, group);
        el("scenario").append(group);
      }
      realDemos.set(d.id, { ...d, base });
      groups.get(label).append(new Option(d.title, `real:${d.id}`));
    }
  }
}

/// Ground-truth rows as [t, x, y, level index]; single-floor traces have no level column.
function parseWaypoints(traceText) {
  return traceText.split("\n").filter((l) => l.startsWith("gt,")).map((l) => {
    const f = l.split(",");
    return [Number(f[1]), Number(f[2]), Number(f[3]), f.length > 5 ? Number(f[5]) : 0];
  });
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

/// Loads a simulated scenario; returns the worker's start message.
async function loadSimulated(name) {
  const scenarioUrl = new URL(`data/scenarios/${name}.json`, window.location.href);
  const scenarioText = await loadJson(scenarioUrl);
  const scenario = JSON.parse(scenarioText);
  const venueUrl = new URL(scenario.venue, scenarioUrl);
  const venueText = await loadJson(venueUrl);
  const configText = scenario.demo_config ? await loadJson(new URL(scenario.demo_config, scenarioUrl)) : "";
  venue = normalizeVenue(JSON.parse(venueText));
  geojsonUrl = new URL(venueUrl.href.replace(/\/venues\/([^/]+)\.json$/, "/geojson/$1.geojson"));
  backgrounds = null;
  waypoints = [];
  // The real-building tour explains itself on the map; the synthetic scenarios name the effect they inject.
  el("description").textContent = scenario.demo_config ? "" : scenario.description || "";
  return { venue: venueText, scenario: scenarioText, config: configText };
}

/// Loads a recording; returns the worker's start message.
async function loadRecording(entry) {
  const at = (path) => new URL(path, entry.base);
  const [venueText, traceText, configText] = await Promise.all([
    loadJson(at(entry.venue)), loadJson(at(entry.trace)), entry.config ? loadJson(at(entry.config)) : "",
  ]);
  venue = normalizeVenue(JSON.parse(venueText));
  // One plan per floor (`images`, by ordinal), one for a single floor (`image`), or none.
  const byOrdinal = entry.images || (entry.image ? { [venue.levels[0].ordinal]: entry.image } : {});
  const images = await Promise.all(venue.levels.map((l) => (byOrdinal[l.ordinal] ? loadImage(at(byOrdinal[l.ordinal])) : null)));
  backgrounds = images.some(Boolean) ? images : null;
  geojsonUrl = entry.geojson ? at(entry.geojson) : null;
  waypoints = parseWaypoints(traceText);
  el("description").textContent = entry.description ||
    `${entry.title} with a real Android phone (IMU + Wi-Fi), held out from fitting and tuning. Squares are ` +
    `the surveyor's waypoints, the only ground truth; the black path is interpolated between them, and the ` +
    `live error is measured against it. Purple dots are Wi-Fi access points located from other walks.`;
  return { venue: venueText, trace: traceText, config: configText };
}

/// Starts a worker on `job` (the start message) and resolves once its first frames have arrived.
function startRun(job) {
  return new Promise((resolve, reject) => {
    const worker = new Worker("worker.js");
    const r = { worker, duration: 0, finished: false, filters: ["grid"], cellSize: 0, cellCount: 0 };
    run = r;
    const fail = (message) => {
      if (run !== r) return;
      el("clock").textContent = `engine failed: ${message}`;
      reject(new Error(message));
    };
    worker.onmessage = ({ data }) => {
      if (run !== r) return;
      if (data.type === "info") {
        Object.assign(r, { duration: data.duration, filters: data.filters, cellSize: data.gridCellSize,
                           cellCount: data.gridCells });
      } else if (data.type === "frames") {
        addFrames(data);
        r.finished = data.finished;
        resolve();
      } else if (data.type === "error") {
        fail(data.message);
      }
    };
    worker.onerror = (ev) => fail(ev.message || "worker error");
    worker.postMessage({ type: "start", ...job });
  });
}

async function start() {
  const seq = ++startSeq;
  // Drop the old run and its frames first: tick() must not draw them over the new venue while it loads.
  if (run) run.worker.terminate();
  run = null;
  recorded = [];
  el("view3d-status").textContent = "";
  const name = el("scenario").value;
  real = name.startsWith("real:") ? realDemos.get(name.slice(5)) : null;
  const job = real ? await loadRecording(real) : await loadSimulated(name);
  if (seq !== startSeq) return;
  el("clock").textContent = "starting the grid filter...";
  closeInfo();
  roomLog = [];
  lastFix = {};
  roomLogKey = null;
  viewT = 0;
  lastFrame = null;
  await startRun(job);
  if (seq !== startSeq) return;
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

/// Appends a batch of frames from the worker to `recorded`: {t, steps, truth, est: {filter: estimate},
/// rooms: {truth, filter: [room index]}, ble: [{id, rssi}], cells}. The cell snapshots come at 5 Hz; a frame
/// without one keeps the previous frame's.
function addFrames({ frames, events }) {
  for (const fr of frames) {
    for (const f of run.filters) {
      const e = fr.est[f];
      const prev = lastFix[f];
      // A jump through a wall (a mode switch, a re-initialisation) leaves a gap in the trail, not a line through it.
      e.jump = !!(e.valid && prev && prev.level === e.level && crossesWall(prev, e, e.level));
      if (e.valid) lastFix[f] = e;
    }
    const prev = recorded[recorded.length - 1];
    if (!fr.cells) fr.cells = prev ? prev.cells : null;
    recorded.push(fr);
  }
  roomLog.push(...events);
}

const frameIndex = (t) => Math.max(0, Math.floor(t / FRAME_DT + 1e-6));
const lastRecorded = () => recorded[recorded.length - 1].t;
/// Latest time the timeline can reach: the end of the recording once the engine has finished.
const maxTime = () => (run.finished ? lastRecorded() : timelineEnd());
const timelineEnd = () => (Math.floor(run.duration / FRAME_DT) + 1) * FRAME_DT;
const computing = () => recorded.length <= frameIndex(viewT) && !run.finished;

/// The recorded frame shown at viewT (the newest one while the engine is still catching up).
function shownFrame() {
  return Math.min(recorded.length - 1, frameIndex(viewT));
}

function shownLevel(fr) {
  const v = el("floor").value;
  return v === "auto" ? fr.truth.level : Number(v);
}

function transform() {
  const [x0, y0, x1, y1] = real && real.view ? real.view : venue.bounds;
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
/// Same rule as the engine's segments_intersect: touching counts.
function segmentsIntersect(p1x, p1y, p2x, p2y, q1x, q1y, q2x, q2y) {
  const orient = (ax, ay, bx, by, cx, cy) => {
    const v = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    return v > 1e-12 ? 1 : v < -1e-12 ? -1 : 0;
  };
  const within = (ax, ay, bx, by, px, py) =>
    Math.min(ax, bx) <= px && px <= Math.max(ax, bx) && Math.min(ay, by) <= py && py <= Math.max(ay, by);
  const o1 = orient(p1x, p1y, p2x, p2y, q1x, q1y);
  const o2 = orient(p1x, p1y, p2x, p2y, q2x, q2y);
  const o3 = orient(q1x, q1y, q2x, q2y, p1x, p1y);
  const o4 = orient(q1x, q1y, q2x, q2y, p2x, p2y);
  if (o1 !== o2 && o3 !== o4) return true;
  return (
    (o1 === 0 && within(p1x, p1y, p2x, p2y, q1x, q1y)) ||
    (o2 === 0 && within(p1x, p1y, p2x, p2y, q2x, q2y)) ||
    (o3 === 0 && within(q1x, q1y, q2x, q2y, p1x, p1y)) ||
    (o4 === 0 && within(q1x, q1y, q2x, q2y, p2x, p2y))
  );
}

function crossesWall(a, b, level) {
  const walls = venue.levels[level]?.walls || [];
  return walls.some(([ax, ay, bx, by]) => segmentsIntersect(a.x, a.y, b.x, b.y, ax, ay, bx, by));
}

/// Polyline through at(frame) for the frames up to `last` on `level`; the pen lifts where at() is null,
/// on another level, or marked as a jump.
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
    if (pen && !p.jump) ctx.lineTo(T.toX(p.x), T.toY(p.y));
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

/// Room outlines on `level`: a faint fill, the grid filter's current rooms tinted green, the true ones outlined,
/// and a name wherever it fits inside the room's on-screen box.
function drawRooms(T, level, fr) {
  if (!venue.rooms.length) return;
  const gridIn = new Set(visibleFilters().includes("grid") ? fr.rooms.grid : []);
  const truthIn = new Set(fr.rooms.truth);
  ctx.font = LABEL_FONT;
  venue.rooms.forEach((r, i) => {
    if (r.level !== level) return;
    ctx.beginPath();
    r.area.forEach(([x, y], k) => (k ? ctx.lineTo(T.toX(x), T.toY(y)) : ctx.moveTo(T.toX(x), T.toY(y))));
    ctx.closePath();
    ctx.fillStyle = gridIn.has(i) ? "rgba(44, 160, 44, 0.16)" : "rgba(29, 78, 137, 0.035)";
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
    for (const { id } of recorded[k].ble) if (!pings.has(id)) pings.set(id, recorded[k].t);
  }
  return pings;
}

function draw(level, idx) {
  const fr = recorded[idx];
  const T = transform();
  ctx.clearRect(0, 0, canvas.width, canvas.height);
  drawGrid(T);

  const background = backgrounds && backgrounds[level];
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
    // Pale: heard in the fingerprints only, with no fitted position or radio model ("model": false).
    ctx.fillStyle = b.model === false ? COLORS.beaconUnmodelled : COLORS.beacon;
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

  const shown = visibleFilters();
  // The grid filter's heaviest cells as squares, darker with more mass.
  const c = el("show-cells").checked ? fr.cells : null;
  if (c) {
    const h = Math.max(2, run.cellSize * T.s);
    ctx.fillStyle = COLORS.grid;
    for (let k = 0; k < c.length; k += 4) {
      if (c[k + 3] !== level) continue;
      ctx.globalAlpha = 0.06 + 0.5 * Math.sqrt(c[k + 2]);
      ctx.fillRect(T.toX(c[k]) - h / 2, T.toY(c[k + 1]) - h / 2, h, h);
    }
    ctx.globalAlpha = 1;
  }

  const trails = el("show-trails").checked;
  if (trails) drawPath(idx, (h) => (h.truth.valid ? h.truth : null), T, COLORS.truth, 2, level);
  ctx.fillStyle = COLORS.truth;
  for (const [t, x, y, l] of waypoints) {
    if (t > now) break;
    if (l === level) ctx.fillRect(T.toX(x) - 4, T.toY(y) - 4, 8, 8);
  }
  for (const f of shown) {
    const fix = (h) => (h.t > TRAIL_START_S && h.est[f].valid ? h.est[f] : null);
    if (trails) drawPath(idx, fix, T, COLORS[f], 1.3, level);
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
  if (t.valid) dot(t.x, t.y, T, COLORS.truth, 6, true, t.level !== level);
}

function visibleFilters() {
  return run.filters.filter((f) => el(`show-${f}`).checked);
}

function updateStats(fr) {
  const t = fr.truth;
  const multi = venue.levels.length > 1;
  const levelText = multi && t.valid ? `   floor: ${venue.levels[t.level].name}` : "";
  // Shown whenever the view waits at the newest frame (playback is held there while the engine catches up).
  const waiting = !run.finished && frameIndex(viewT) >= recorded.length - 1;
  const busy = waiting ? `   computing... (engine at ${lastRecorded().toFixed(1)} s)` : "";
  el("clock").textContent = `t = ${fr.t.toFixed(1)} / ${run.duration.toFixed(0)} s   steps: ${fr.steps}` +
    `${levelText}${busy}`;
  const fmt = (v) => (v < 0 ? "-" : `${v.toFixed(2)} m`);
  const pct = (v) => (v < 0 ? "-" : `${(100 * v).toFixed(1)}%`);
  el("stats").innerHTML = run.filters.map((f) => {
    const e = fr.est[f];
    const floor = multi ? `<td>${e.valid ? venue.levels[e.level].name : "-"} (${pct(e.floorAcc)})</td>` : "";
    return `<tr><td><span class="swatch" style="background:${COLORS[f]}"></span>${f}</td>` +
      `<td>${fmt(e.error)}</td><td>${fmt(e.mean)}</td><td>${e.cep95 > 0 ? fmt(e.cep95) : "-"}</td>${floor}</tr>`;
  }).join("");
}

/// Trail through recorded[0..last] as runs of points on one level, thinned to TRAIL_STEP_3D; a run ends where
/// at() is null, the level changes or the point is marked as a jump.
function trailRuns(last, at) {
  const runs = [];
  let run = null;
  for (let k = 0; k <= last; ++k) {
    const p = at(recorded[k]);
    if (!p || !run || p.level !== run.level || p.jump) {
      run = p ? { level: p.level, pts: [[p.x, p.y]] } : null;
      if (run) runs.push(run);
      continue;
    }
    const [lx, ly] = run.pts[run.pts.length - 1];
    if (Math.hypot(p.x - lx, p.y - ly) >= TRAIL_STEP_3D || k === last) run.pts.push([p.x, p.y]);
  }
  return runs;
}

function currentFrame(fr, level, idx) {
  const filters = visibleFilters();
  const trails = el("show-trails").checked;
  return {
    t: fr.t,
    truth: fr.truth,
    estimates: filters.map((f) => ({ name: f, color: COLORS[f], e: fr.est[f] })),
    trailKey: trails ? `${idx}|${filters}` : "off",
    trails: () => !trails ? [] : [
      { color: COLORS.truth, width: 0.4, runs: trailRuns(idx, (h) => (h.truth.valid ? h.truth : null)) },
      ...filters.map((f) => ({
        color: COLORS[f], width: 0.3,
        runs: trailRuns(idx, (h) => (h.t > TRAIL_START_S && h.est[f].valid ? h.est[f] : null)),
      })),
    ],
    cells: el("show-cells").checked ? fr.cells : null,
    cellSize: run.cellSize,
    rooms: filters.flatMap((f, stack) => fr.rooms[f].map((i) => ({ id: venue.rooms[i].id, color: COLORS[f], stack }))),
    focusLevel: level,
    levels: venue.levels,
  };
}

function tick(ts) {
  if (run && venue && recorded.length) {
    if (playing) {
      const dt = lastFrame === null ? 0 : Math.min((ts - lastFrame) / 1000, 0.1);
      viewT += direction * dt * Number(el("speed").value);
    }
    viewT = Math.min(Math.max(viewT, 0), maxTime());
    // Playing forwards never runs ahead of the engine; a seek waits for it instead.
    if (playing && computing()) viewT = lastRecorded();
    if (playing && direction < 0 && viewT <= 0) setPlaying(false);
    if (playing && direction > 0 && run.finished && viewT >= lastRecorded()) setPlaying(false);
    lastFrame = ts;
    const idx = shownFrame();
    const fr = recorded[idx];
    const level = shownLevel(fr);
    if (view === "2d") draw(level, idx);
    if (view === "3d" && map3d) map3d.update(currentFrame(fr, level, idx));
    updateStats(fr);
    if (selected && ts - infoRefresh > INFO_REFRESH_MS) {
      infoRefresh = ts;
      renderInfo(idx);
    }
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
  el("time-label").textContent = `${fr.t.toFixed(1)} / ${run.duration.toFixed(1)} s`;
}

/// Starts or stops playback; `dir` (+1 or -1) changes the direction. Playing forwards from the end restarts.
function setPlaying(on, dir = direction) {
  direction = dir;
  if (on && dir > 0 && run.finished && viewT >= lastRecorded()) viewT = 0;
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
  if (!geojsonUrl) {
    status.textContent = "3D view unavailable: this recording has local coordinates only (no georeference).";
    setView("2d");
    return;
  }
  try {
    status.textContent = "loading 3D view...";
    if (!map3dReady) {
      const m = new Map3D(el("map3d"));
      m.onPick = (pick, point) => {
        const r = el("map3d").getBoundingClientRect();
        openInfo(pick, r.left + point.x, r.top + point.y);
      };
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
  closeInfo();
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
let dragMoved = 0;
canvas.addEventListener("pointerdown", (ev) => {
  drag = canvasPoint(ev);
  dragMoved = 0;
  canvas.setPointerCapture(ev.pointerId);
  canvas.classList.add("dragging");
});
canvas.addEventListener("pointermove", (ev) => {
  if (!drag) return;
  const [px, py] = canvasPoint(ev);
  zoom = { ...zoom, x: zoom.x + px - drag[0], y: zoom.y + py - drag[1] };
  dragMoved += Math.hypot(px - drag[0], py - drag[1]);
  drag = [px, py];
});
const endDrag = () => {
  drag = null;
  canvas.classList.remove("dragging");
};
canvas.addEventListener("pointerup", (ev) => {
  const click = drag && dragMoved < 4;
  endDrag();
  if (click && recorded.length) openInfo(pick2d(...canvasPoint(ev)), ev.clientX, ev.clientY);
});
el("info-close").addEventListener("click", closeInfo);
canvas.addEventListener("pointercancel", endDrag);
el("view2d").addEventListener("click", () => setView("2d"));
el("view3d").addEventListener("click", async () => {
  setView("3d");
  await show3d();
});
el("exploded").addEventListener("input", () => map3d && map3d.setExploded(Number(el("exploded").value)));
el("follow").addEventListener("change", () => map3d && (map3d.follow = el("follow").checked));
el("underlay").addEventListener("change", () => map3d && map3d.setUnderlay(el("underlay").checked));

// ---- Clicking map items: beacons, rooms, stairs and lifts, the walker and the estimate ----

function pointInPolygon(px, py, poly) {
  let inside = false;
  for (let i = 0, j = poly.length - 1; i < poly.length; j = i++) {
    const [xi, yi] = poly[i];
    const [xj, yj] = poly[j];
    if ((yi > py) !== (yj > py) && px < ((xj - xi) * (py - yi)) / (yj - yi) + xi) inside = !inside;
  }
  return inside;
}

function polygonArea(poly) {
  let a = 0;
  for (let i = 0, j = poly.length - 1; i < poly.length; j = i++) a += poly[j][0] * poly[i][1] - poly[i][0] * poly[j][1];
  return Math.abs(a) / 2;
}

/// The map item under canvas point (px, py) in the 2D view, or null.
function pick2d(px, py) {
  const fr = recorded[shownFrame()];
  const level = shownLevel(fr);
  const T = transform();
  const near = (x, y, r) => Math.hypot(T.toX(x) - px, T.toY(y) - py) <= r;
  if (fr.truth.valid && fr.truth.level === level && near(fr.truth.x, fr.truth.y, 9)) return { kind: "truth" };
  for (const f of visibleFilters()) {
    const e = fr.est[f];
    if (e.valid && e.level === level && near(e.x, e.y, 9)) return { kind: f };
  }
  const beacon = venue.beacons.find((b) => b.level === level && near(b.x, b.y, 9));
  if (beacon) return { kind: "beacon", id: beacon.id };
  const onScreen = (area) => area.map(([x, y]) => [T.toX(x), T.toY(y)]);
  const conn = venue.connectors.find((c) => c.levels.includes(level) && pointInPolygon(px, py, onScreen(c.area)));
  if (conn) return { kind: "connector", id: conn.id };
  // Nested rooms: the smallest one under the cursor.
  const rooms = venue.rooms.filter((r) => r.level === level && pointInPolygon(px, py, onScreen(r.area)));
  rooms.sort((a, b) => polygonArea(a.area) - polygonArea(b.area));
  return rooms.length ? { kind: "room", id: rooms[0].id } : null;
}

function openInfo(pick, clientX, clientY) {
  if (!pick || pick.kind === "level") {
    closeInfo();
    return;
  }
  selected = pick;
  const card = el("info");
  card.hidden = false;
  renderInfo(shownFrame());
  // Next to the click, kept inside the view.
  const box = card.parentElement.getBoundingClientRect();
  const x = clientX - box.left + 14;
  const y = clientY - box.top + 14;
  card.style.left = `${Math.max(0, Math.min(x, box.width - card.offsetWidth - 4))}px`;
  card.style.top = `${Math.max(0, Math.min(y, box.height - card.offsetHeight - 4))}px`;
}

function closeInfo() {
  selected = null;
  el("info").hidden = true;
}

const fmtM = (v) => `${v.toFixed(2)} m`;
const levelName = (i) => (venue.levels[i] ? venue.levels[i].name : "-");

/// Card rows for the beacon `b` as of frame idx: its model, and what the phone has heard from it so far.
function beaconRows(b, idx) {
  const fr = recorded[idx];
  let count = 0;
  let sum = 0;
  let min = Infinity;
  let max = -Infinity;
  let last = null;
  for (let k = 0; k <= idx; ++k) {
    for (const m of recorded[k].ble) {
      if (m.id !== b.id) continue;
      ++count;
      sum += m.rssi;
      min = Math.min(min, m.rssi);
      max = Math.max(max, m.rssi);
      last = { t: recorded[k].t, rssi: m.rssi };
    }
  }
  const t = fr.truth;
  const d = Math.hypot(t.x - b.x, t.y - b.y);
  const floors = Math.abs(t.level - b.level);
  const rows = [
    ["floor", levelName(b.level)],
    [b.model === false ? "heard strongest at" : "position", `${b.x.toFixed(1)}, ${b.y.toFixed(1)} m`],
  ];
  if (b.model === false) rows.push(["radio model", "none: used in fingerprints only"]);
  if (b.tx_power !== undefined) rows.push(["tx power (1 m)", `${b.tx_power.toFixed(1)} dBm`]);
  if (b.path_loss_exp !== undefined) rows.push(["path-loss exponent", b.path_loss_exp.toFixed(2)]);
  rows.push(["walker distance", `${fmtM(d)}${floors ? `, ${floors} floor${floors > 1 ? "s" : ""} away` : ""}`]);
  if (b.tx_power !== undefined && b.path_loss_exp !== undefined) {
    const model = b.tx_power - 10 * b.path_loss_exp * Math.log10(Math.max(d, 1));
    rows.push(["model RSSI there", `${model.toFixed(1)} dBm${floors ? " (before floor loss)" : ""}`]);
  }
  rows.push(["heard", `${count} time${count === 1 ? "" : "s"} so far`]);
  if (count) {
    rows.push(["last heard", `${last.t.toFixed(1)} s, ${last.rssi.toFixed(0)} dBm`]);
    rows.push(["RSSI mean (min / max)", `${(sum / count).toFixed(1)} dBm (${min.toFixed(0)} / ${max.toFixed(0)})`]);
  }
  return rows;
}

function roomRows(r, i, fr) {
  const visits = (f) => roomLog.slice(0, roomEventsUntil(fr.t)).filter((e) => e.filter === f && e.room === i &&
    e.type === "enter").length;
  const rows = [
    ["floor", levelName(r.level)],
    ["id", r.id],
    ["area", `${polygonArea(r.area).toFixed(1)} m\u00b2`],
    ["walker inside", fr.rooms.truth.includes(i) ? "yes" : "no"],
    ["walker visits", String(visits("truth"))],
  ];
  for (const f of visibleFilters()) {
    rows.push([`${f} inside`, fr.rooms[f].includes(i) ? "yes" : "no"], [`${f} visits`, String(visits(f))]);
  }
  return rows;
}

/// Fills the info card for `selected` as of frame idx.
function renderInfo(idx) {
  const fr = recorded[idx];
  let title = "";
  let rows = [];
  const { kind, id } = selected;
  if (kind === "beacon") {
    const b = venue.beacons.find((x) => x.id === id);
    if (!b) return closeInfo();
    title = `${b.id.startsWith("w:") ? "Access point" : "Beacon"} ${b.id}`;
    rows = beaconRows(b, idx);
  } else if (kind === "room") {
    const i = venue.rooms.findIndex((r) => r.id === id);
    if (i < 0) return closeInfo();
    title = venue.rooms[i].name;
    rows = roomRows(venue.rooms[i], i, fr);
  } else if (kind === "connector") {
    const c = venue.connectors.find((x) => x.id === id);
    if (!c) return closeInfo();
    title = c.kind === "elevator" ? "Lift" : c.kind === "escalator" ? "Escalator" : "Stairs";
    rows = [["id", c.id], ["floors", c.levels.map(levelName).join(", ")]];
  } else if (kind === "truth") {
    const t = fr.truth;
    title = "Walker (ground truth)";
    rows = [["position", `${t.x.toFixed(2)}, ${t.y.toFixed(2)} m`], ["floor", levelName(t.level)],
            ["steps", String(fr.steps)], ["time", `${fr.t.toFixed(1)} s`]];
    const inRooms = fr.rooms.truth.map((i) => venue.rooms[i].name).join(", ");
    if (venue.rooms.length) rows.push(["room", inRooms || "-"]);
  } else if (run.filters.includes(kind)) {
    const e = fr.est[kind];
    title = `${FILTER_TITLES[kind]} estimate`;
    if (!e.valid) rows = [["state", "no fix yet"]];
    else {
      rows = [["position", `${e.x.toFixed(2)}, ${e.y.toFixed(2)} m`], ["floor", levelName(e.level)],
              ["sigma", fmtM(e.sigma)], ["error now", e.error < 0 ? "-" : fmtM(e.error)],
              ["mean error", e.mean < 0 ? "-" : fmtM(e.mean)], ["CEP95", e.cep95 > 0 ? fmtM(e.cep95) : "-"]];
      const inRooms = fr.rooms[kind].map((i) => venue.rooms[i].name).join(", ");
      if (venue.rooms.length) rows.push(["room", inRooms || "-"]);
    }
  } else return closeInfo();
  el("info-title").textContent = title;
  el("info-body").innerHTML = rows.map(([k, v]) => `<tr><th>${escapeHtml(k)}</th><td>${escapeHtml(v)}</td></tr>`)
    .join("");
}

loadRealDemos().then(async () => {
  requestAnimationFrame(tick);
  await start();
}).catch((err) => {
  el("clock").textContent = `failed to load: ${err}`;
});
