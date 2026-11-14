"use strict";

// Live mode: this phone's motion sensors feed the engine (live-worker.js), which answers with the estimate.
// What a browser gives is an accelerometer, a gyroscope and a compass heading; it gives no Wi-Fi or BLE
// signal strength, so the position comes from steps, heading and the map, from a start the user taps.

const COLORS = { ink: "#1d4e89", wall: "rgba(29, 78, 137, 0.55)", grid: "#2ca02c", truth: "#222", paper: "#fbfcfe" };
const LABEL_FONT = '500 12px "Barlow Condensed", "Arial Narrow", sans-serif';
const IMU_STRIDE = 10; // t, ax, ay, az, gx, gy, gz, mx, my, mz (as in live-worker.js)
const FLUSH_MS = 100; // sensor samples go to the engine in batches
const FIELD_UT = 30; // strength of the horizontal field written for a compass heading
const COMPASS_STALE_MS = 1500;
const TAP_SIGMA_M = 1.0; // how well a tap places the user
const TAP_SLOP_PX = 10;
const OPEN_AREA = { name: "open area", bounds: [0, 0, 40, 40], walls: [], beacons: [] };

const el = (id) => document.getElementById(id);
const canvas = el("map");
const ctx = canvas.getContext("2d");
const rad = (deg) => (deg * Math.PI) / 180;

let venue = null; // normalised venue being shown
let venueText = ""; // its JSON, as given to the engine
let venueRef = { kind: "open" }; // where it came from: {kind: "open" | "repo" | "file", path, name}
let worker = null;
let session = null; // the run in progress or just stopped (newSession())
let zoom = { k: 1, x: 0, y: 0 }; // canvas px = k * fitted px + (x, y)
let wakeLock = null;

function newSession() {
  return {
    running: false, ready: false, t0Ms: null, clockMs: 0, pending: [], samples: 0, recent: [],
    gravity: null, compass: null, compassMs: -Infinity, compassSeen: false, gyroSeen: false,
    hasFix: false, est: null, cells: null, cellSize: 0.5, cellCount: 0, busyMs: 0, steps: 0,
    trail: [], fixes: [], marks: [], nextMark: 0,
  };
}

function setStatus(text, warn = false) {
  el("status").textContent = text;
  el("status").classList.toggle("warn", warn);
}

/// Venue JSON in either layout (top-level walls, or levels) -> one shape, levels by index.
function normalizeVenue(v) {
  const levels = (v.levels || [{ ordinal: 0, name: "", walls: v.walls || [] }])
    .map((l) => ({ ordinal: l.ordinal, name: l.name || `L${l.ordinal}`, walls: l.walls || [] }))
    .sort((a, b) => a.ordinal - b.ordinal);
  const index = new Map(levels.map((l, i) => [l.ordinal, i]));
  const levelOf = (ordinal) => index.get(ordinal ?? levels[0].ordinal);
  return {
    name: v.name || "venue",
    bounds: v.bounds,
    rotationDeg: v.georef ? v.georef.rotation_deg || 0 : 0,
    levels,
    connectors: (v.connectors || []).map((c) => ({ ...c, levels: c.levels.map(levelOf) })),
    rooms: (v.rooms || []).map((r) => ({ ...r, level: levelOf(r.level) })),
  };
}

function showVenue(text, ref) {
  venue = normalizeVenue(JSON.parse(text));
  venueText = text;
  venueRef = ref;
  zoom = { k: 1, x: 0, y: 0 };
  el("floor").innerHTML = `<option value="auto">follow the estimate</option>` +
    venue.levels.map((l, i) => `<option value="${i}">${l.name}</option>`).join("");
  el("floor-control").hidden = venue.levels.length < 2;
}

async function loadSelectedVenue() {
  const value = el("venue").value;
  if (value === "open") {
    showVenue(JSON.stringify(OPEN_AREA), { kind: "open", name: "open-area" });
  } else if (value !== "file") {
    const res = await fetch(value);
    if (!res.ok) throw new Error(`${value}: ${res.status}`);
    showVenue(await res.text(), { kind: "repo", path: value });
  }
}

// ---- Sensors ----------------------------------------------------------------------------------------

/// Heading on the map [rad, counter-clockwise from the map's +x] of a compass heading [degrees clockwise
/// from north]. The map's +x is `rotationDeg` counter-clockwise from east.
function mapHeading(compassDeg) {
  return rad(90 - compassDeg - venue.rotationDeg);
}

/// A devicemotion event (or an object shaped like one) at `tMs`. Turns it into a sample in the engine's
/// frame: the acceleration as measured (only its magnitude is used), the turn rate about the vertical, and
/// the compass heading as the horizontal magnetic field the engine expects.
function onMotion(ev, tMs = ev.timeStamp) {
  const s = session;
  if (!s || !s.running) return;
  const a = ev.accelerationIncludingGravity;
  if (!a || a.x === null || a.x === undefined) return;
  if (s.t0Ms === null) s.t0Ms = tMs;
  s.clockMs = tMs;

  // Which way is up, in the phone's axes: along gravity, smoothed over about a second. Browsers disagree
  // about the sign of accelerationIncludingGravity (Safari's is the opposite of the specification's), so
  // the sign comes from how the phone is held: screen up, which makes up point out of the screen (+z).
  const g = s.gravity || [a.x, a.y, a.z];
  s.gravity = [g[0] + 0.03 * (a.x - g[0]), g[1] + 0.03 * (a.y - g[1]), g[2] + 0.03 * (a.z - g[2])];
  const norm = Math.hypot(...s.gravity) || 1;
  const sign = s.gravity[2] >= 0 ? 1 : -1;
  const up = s.gravity.map((v) => (sign * v) / norm);

  // rotationRate is in degrees per second about the phone's x (beta), y (gamma) and z (alpha) axes.
  const r = ev.rotationRate;
  let yawRate = 0;
  if (r && r.alpha !== null && r.alpha !== undefined) {
    s.gyroSeen = true;
    yawRate = rad((r.beta || 0) * up[0] + (r.gamma || 0) * up[1] + r.alpha * up[2]);
  }

  let mx = 0;
  let my = 0;
  if (s.compass !== null && tMs - s.compassMs < COMPASS_STALE_MS) {
    const psi = mapHeading(s.compass);
    mx = FIELD_UT * Math.sin(psi);
    my = FIELD_UT * Math.cos(psi);
  }

  s.pending.push((tMs - s.t0Ms) / 1000, a.x, a.y, a.z, 0, 0, yawRate, mx, my, 0);
  s.samples += 1;
  s.recent.push(tMs);
  while (s.recent.length && tMs - s.recent[0] > 2000) s.recent.shift();
  // The open area has nothing to tap at: the walk starts in its middle.
  if (venueRef.kind === "open" && !s.hasFix && s.ready) {
    const [x0, y0, x1, y1] = venue.bounds;
    sendFix((x0 + x1) / 2, (y0 + y1) / 2, 0, 0.5);
  }
}

/// A deviceorientation event (or an object shaped like one): the compass heading of the phone's top edge.
function onOrientation(ev, tMs = ev.timeStamp) {
  const s = session;
  if (!s || !s.running) return;
  let compass = null;
  if (typeof ev.webkitCompassHeading === "number" && ev.webkitCompassHeading >= 0) {
    compass = ev.webkitCompassHeading; // Safari: degrees clockwise from north
  } else if (ev.absolute && ev.alpha !== null && ev.alpha !== undefined) {
    compass = 360 - ev.alpha; // alpha grows counter-clockwise from north
  }
  if (compass === null) return;
  s.compass = ((compass % 360) + 360) % 360;
  s.compassMs = tMs;
  s.compassSeen = true;
}

function flush() {
  const s = session;
  if (!s || !worker || !s.ready || s.pending.length === 0) return;
  const samples = new Float64Array(s.pending);
  s.pending = [];
  worker.postMessage({ type: "imu", samples }, [samples.buffer]);
}

/// Time on the session's sensor clock [s].
const sessionTime = () => (session && session.t0Ms !== null ? (session.clockMs - session.t0Ms) / 1000 : 0);

function sendFix(x, y, level, sigma = TAP_SIGMA_M) {
  flush();
  session.hasFix = true;
  session.fixes.push({ x, y, level });
  worker.postMessage({ type: "fix", t: sessionTime(), x, y, level, sigma });
}

function sendMark(x, y, level) {
  flush();
  const id = session.nextMark++;
  session.marks.push({ id, x, y, level, error: null });
  worker.postMessage({ type: "mark", id, t: sessionTime(), x, y, level });
}

/// A tap on the map at (x, y) in metres: the start, a correction, or a mark of the true position.
function tap(x, y) {
  const s = session;
  if (!s || !s.running || !s.ready) return;
  const level = shownLevel();
  if (!s.hasFix || el("tap-fixes").checked) {
    sendFix(x, y, level);
    setStatus("Walk. Tap the map where you really are to measure the error.");
  } else {
    sendMark(x, y, level);
  }
}

// ---- Session ----------------------------------------------------------------------------------------

async function requestSensorPermission() {
  // Safari asks the user, and only from inside a tap; other browsers have nothing to ask.
  const ask = async (type) => {
    if (typeof type === "undefined" || typeof type.requestPermission !== "function") return true;
    try {
      return (await type.requestPermission()) === "granted";
    } catch {
      return false;
    }
  };
  const motion = await ask(window.DeviceMotionEvent);
  await ask(window.DeviceOrientationEvent);
  return motion;
}

function onWorkerMessage({ data }) {
  const s = session;
  if (!s) return;
  if (data.type === "info") {
    s.ready = true;
    s.cellSize = data.cellSize;
    s.cellCount = data.cellCount;
    if (venueRef.kind !== "open") setStatus("Tap the map where you are standing.");
    else setStatus("Walk. You start in the middle of the area; north is up.");
  } else if (data.type === "state") {
    s.est = data.est;
    s.steps = data.steps;
    s.busyMs = data.busyMs;
    s.engineT = data.t;
    if (data.cells) s.cells = data.cells;
    const e = data.est;
    const last = s.trail[s.trail.length - 1];
    if (e.valid && (!last || last.level !== e.level || Math.hypot(e.x - last.x, e.y - last.y) > 0.15)) {
      s.trail.push({ x: e.x, y: e.y, level: e.level });
    }
    updateReadout();
  } else if (data.type === "mark") {
    const mark = s.marks.find((m) => m.id === data.id);
    if (mark) mark.error = data.error;
    updateReadout();
  } else if (data.type === "trace") {
    saveFiles(data.csv);
  } else if (data.type === "error") {
    setStatus(`Engine failed: ${data.message}`, true);
  }
}

async function start() {
  const permitted = await requestSensorPermission(); // first: it must run inside the tap
  if (!venue) await loadSelectedVenue();
  stop();
  session = newSession();
  session.running = true;
  el("start").textContent = "Stop";
  el("save").disabled = true;
  el("saved").hidden = true;
  if (!permitted) {
    setStatus("Motion sensors were not allowed. Allow motion and orientation access for this site and try again.", true);
  } else {
    setStatus("Starting the engine...");
  }
  worker = new Worker("live-worker.js");
  worker.onmessage = onWorkerMessage;
  worker.onerror = (ev) => setStatus(`Engine failed: ${ev.message || "worker error"}`, true);
  worker.postMessage({ type: "start", venue: venueText });
  window.addEventListener("devicemotion", onMotion);
  window.addEventListener("deviceorientationabsolute", onOrientation);
  window.addEventListener("deviceorientation", onOrientation);
  const s = session;
  setTimeout(() => {
    if (session === s && s.running && s.samples === 0) {
      setStatus("This device sends no motion data. Open this page on a phone, over https.", true);
    }
  }, 3000);
  try {
    wakeLock = await navigator.wakeLock.request("screen"); // keep the screen on while walking
  } catch {
    wakeLock = null;
  }
}

/// Stops feeding the engine. The session and its recording stay until the next start.
function stop() {
  window.removeEventListener("devicemotion", onMotion);
  window.removeEventListener("deviceorientationabsolute", onOrientation);
  window.removeEventListener("deviceorientation", onOrientation);
  if (wakeLock) wakeLock.release().catch(() => {});
  wakeLock = null;
  if (session && session.running) {
    flush();
    session.running = false;
    el("save").disabled = session.samples === 0;
    setStatus(session.samples ? "Stopped. Save the recording to replay it with ipe-replay." : "Stopped.");
  } else if (worker) {
    worker.terminate();
    worker = null;
  }
  el("start").textContent = "Start";
}

function download(name, text, type) {
  const url = URL.createObjectURL(new Blob([text], { type }));
  const a = document.createElement("a");
  a.href = url;
  a.download = name;
  document.body.append(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 10000);
}

function requestSave() {
  if (!worker || !session) return;
  flush();
  const s = session;
  const seconds = sessionTime();
  worker.postMessage({ type: "metadata", key: "sensor_hz", value: seconds > 0 ? (s.samples / seconds).toFixed(1) : "0" });
  worker.postMessage({ type: "metadata", key: "compass", value: s.compassSeen ? "yes" : "no" });
  worker.postMessage({ type: "metadata", key: "gyroscope", value: s.gyroSeen ? "yes" : "no" });
  worker.postMessage({
    type: "metadata", key: "frame",
    value: "accel as measured; gyro z is the turn rate about the vertical; mag is the compass heading on the map",
  });
  worker.postMessage({ type: "trace" });
}

function saveFiles(csv) {
  const d = new Date();
  const p = (n) => String(n).padStart(2, "0");
  const stamp = `${d.getFullYear()}${p(d.getMonth() + 1)}${p(d.getDate())}-${p(d.getHours())}${p(d.getMinutes())}${p(d.getSeconds())}`;
  const trace = `ipe-live-${stamp}.csv`;
  download(trace, csv, "text/csv");
  let venuePath = venueRef.path;
  if (venueRef.kind !== "repo") {
    venuePath = `ipe-live-${stamp}-venue.json`;
    download(venuePath, venueText, "application/json");
  }
  const marks = session.marks.length;
  el("saved").hidden = false;
  el("saved").innerHTML = `Saved. Replay it with <code>ipe-replay --venue ${venuePath} --trace ${trace}</code>` +
    (marks ? "" : " (it is scored at the marks: tap the map during the walk to have some).");
}

// ---- Drawing ----------------------------------------------------------------------------------------

function shownLevel() {
  const v = el("floor").value;
  if (v !== "auto") return Number(v);
  return session && session.est && session.est.valid ? session.est.level : 0;
}

function resizeCanvas() {
  const dpr = window.devicePixelRatio || 1;
  const w = Math.round(canvas.clientWidth * dpr);
  const h = Math.round(canvas.clientHeight * dpr);
  if (canvas.width !== w || canvas.height !== h) {
    canvas.width = w;
    canvas.height = h;
  }
}

function transform() {
  const [x0, y0, x1, y1] = venue.bounds;
  const pad = 16 * (window.devicePixelRatio || 1);
  const s = Math.min((canvas.width - 2 * pad) / (x1 - x0), (canvas.height - 2 * pad) / (y1 - y0));
  const ox = pad + ((canvas.width - 2 * pad) - s * (x1 - x0)) / 2;
  const oy = pad + ((canvas.height - 2 * pad) - s * (y1 - y0)) / 2;
  const { k, x: zx, y: zy } = zoom;
  return {
    s: s * k,
    toX: (x) => k * (ox + (x - x0) * s) + zx,
    toY: (y) => k * (canvas.height - (oy + (y - y0) * s)) + zy,
    fromX: (px) => ((px - zx) / k - ox) / s + x0,
    fromY: (py) => (canvas.height - (py - zy) / k - oy) / s + y0,
  };
}

function zoomAt(factor, px, py) {
  const k = Math.min(80, Math.max(0.5, zoom.k * factor));
  const f = k / zoom.k;
  zoom = { k, x: px - (px - zoom.x) * f, y: py - (py - zoom.y) * f };
}

function drawGrid(T) {
  const dpr = window.devicePixelRatio || 1;
  const metres = [1, 2, 5, 10, 20, 50, 100].find((m) => m * T.s >= 36 * dpr) || 100;
  const step = metres * T.s;
  const x0 = T.toX(0) - Math.ceil(T.toX(0) / step) * step;
  const y0 = T.toY(0) - Math.ceil(T.toY(0) / step) * step;
  ctx.strokeStyle = "rgba(29, 78, 137, 0.08)";
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
  ctx.font = LABEL_FONT.replace("12px", `${12 * dpr}px`);
  ctx.fillText(`GRID ${metres} M`, 8 * dpr, canvas.height - 8 * dpr);
}

/// An arrow to north in the top left corner: the map's +y is north when the venue has no rotation.
function drawNorth() {
  const dpr = window.devicePixelRatio || 1;
  const a = rad(90 - venue.rotationDeg); // direction of north on the map, counter-clockwise from +x
  const cx = 26 * dpr;
  const cy = 30 * dpr;
  const len = 14 * dpr;
  const dx = Math.cos(a);
  const dy = -Math.sin(a);
  ctx.strokeStyle = COLORS.ink;
  ctx.fillStyle = COLORS.ink;
  ctx.lineWidth = 1.5 * dpr;
  ctx.beginPath();
  ctx.moveTo(cx - dx * len, cy - dy * len);
  ctx.lineTo(cx + dx * len, cy + dy * len);
  ctx.stroke();
  ctx.beginPath();
  ctx.moveTo(cx + dx * len, cy + dy * len);
  ctx.lineTo(cx + dx * (len - 6 * dpr) - dy * 4 * dpr, cy + dy * (len - 6 * dpr) + dx * 4 * dpr);
  ctx.lineTo(cx + dx * (len - 6 * dpr) + dy * 4 * dpr, cy + dy * (len - 6 * dpr) - dx * 4 * dpr);
  ctx.closePath();
  ctx.fill();
  ctx.font = LABEL_FONT.replace("12px", `${12 * dpr}px`);
  ctx.fillText("N", cx + dx * (len + 8 * dpr) - 4 * dpr, cy + dy * (len + 8 * dpr) + 4 * dpr);
}

function draw() {
  requestAnimationFrame(draw);
  if (!venue) return;
  resizeCanvas();
  const dpr = window.devicePixelRatio || 1;
  const s = session;
  const level = shownLevel();
  const e = s && s.est && s.est.valid ? s.est : null;
  if (e && el("follow").checked && !pointers.size) {
    // Keep the estimate in the middle of the view.
    const T0 = transform();
    zoom.x += canvas.width / 2 - T0.toX(e.x);
    zoom.y += canvas.height / 2 - T0.toY(e.y);
  }
  const T = transform();
  ctx.clearRect(0, 0, canvas.width, canvas.height);
  drawGrid(T);

  const [bx0, by0, bx1, by1] = venue.bounds;
  ctx.strokeStyle = "rgba(29, 78, 137, 0.25)";
  ctx.lineWidth = 1;
  ctx.strokeRect(T.toX(bx0), T.toY(by1), (bx1 - bx0) * T.s, (by1 - by0) * T.s);

  for (const c of venue.connectors) {
    if (!c.levels.includes(level)) continue;
    ctx.beginPath();
    c.area.forEach(([x, y], k) => (k ? ctx.lineTo(T.toX(x), T.toY(y)) : ctx.moveTo(T.toX(x), T.toY(y))));
    ctx.closePath();
    ctx.fillStyle = c.kind === "elevator" ? "rgba(255, 159, 28, 0.3)" : "rgba(46, 196, 182, 0.3)";
    ctx.fill();
  }

  ctx.strokeStyle = COLORS.wall;
  ctx.lineWidth = 1.5 * dpr;
  ctx.lineCap = "round";
  ctx.beginPath();
  for (const [ax, ay, bx, by] of venue.levels[level].walls) {
    ctx.moveTo(T.toX(ax), T.toY(ay));
    ctx.lineTo(T.toX(bx), T.toY(by));
  }
  ctx.stroke();
  drawNorth();
  if (!s) return;

  // The filter's heaviest cells as squares, darker with more mass.
  if (s.cells && s.hasFix) {
    const h = Math.max(2, s.cellSize * T.s);
    ctx.fillStyle = COLORS.grid;
    for (let k = 0; k < s.cells.length; k += 4) {
      if (s.cells[k + 3] !== level) continue;
      ctx.globalAlpha = 0.06 + 0.5 * Math.sqrt(s.cells[k + 2]);
      ctx.fillRect(T.toX(s.cells[k]) - h / 2, T.toY(s.cells[k + 1]) - h / 2, h, h);
    }
    ctx.globalAlpha = 1;
  }

  // The estimate's trail; the pen lifts across floors.
  ctx.strokeStyle = COLORS.grid;
  ctx.lineWidth = 1.5 * dpr;
  ctx.beginPath();
  let pen = false;
  for (const p of s.trail) {
    if (p.level !== level) {
      pen = false;
      continue;
    }
    if (pen) ctx.lineTo(T.toX(p.x), T.toY(p.y));
    else ctx.moveTo(T.toX(p.x), T.toY(p.y));
    pen = true;
  }
  ctx.stroke();

  // Where the user said "start here" (rings) and "I am here" (squares, with the error at that moment).
  ctx.strokeStyle = COLORS.truth;
  ctx.lineWidth = 1.5 * dpr;
  for (const f of s.fixes) {
    if (f.level !== level) continue;
    ctx.beginPath();
    ctx.arc(T.toX(f.x), T.toY(f.y), 7 * dpr, 0, 2 * Math.PI);
    ctx.stroke();
  }
  ctx.fillStyle = COLORS.truth;
  ctx.font = LABEL_FONT.replace("12px", `${12 * dpr}px`);
  for (const m of s.marks) {
    if (m.level !== level) continue;
    ctx.fillRect(T.toX(m.x) - 4 * dpr, T.toY(m.y) - 4 * dpr, 8 * dpr, 8 * dpr);
    if (m.error !== null && m.error >= 0) ctx.fillText(`${m.error.toFixed(1)} m`, T.toX(m.x) + 7 * dpr, T.toY(m.y) - 6 * dpr);
  }

  if (!e) return;
  const here = e.level === level;
  const px = T.toX(e.x);
  const py = T.toY(e.y);
  if (here && e.sigma > 0) {
    ctx.beginPath();
    ctx.arc(px, py, Math.max(4 * dpr, e.sigma * T.s), 0, 2 * Math.PI);
    ctx.fillStyle = COLORS.grid + "22";
    ctx.fill();
  }
  // The heading the engine believes in, as a tick out of the dot.
  ctx.strokeStyle = COLORS.grid;
  ctx.lineWidth = 2.5 * dpr;
  ctx.beginPath();
  ctx.moveTo(px, py);
  ctx.lineTo(px + 16 * dpr * Math.cos(e.heading), py - 16 * dpr * Math.sin(e.heading));
  ctx.stroke();
  ctx.beginPath();
  ctx.arc(px, py, 6 * dpr, 0, 2 * Math.PI);
  if (here) {
    ctx.fillStyle = COLORS.grid;
    ctx.fill();
    ctx.strokeStyle = COLORS.paper;
    ctx.lineWidth = 2 * dpr;
    ctx.stroke();
  } else {
    // On another floor than the one shown: hollow.
    ctx.setLineDash([3 * dpr, 3 * dpr]);
    ctx.stroke();
    ctx.setLineDash([]);
  }
}

function updateReadout() {
  const s = session;
  if (!s) return;
  const seconds = sessionTime();
  el("r-time").textContent = `${seconds.toFixed(0)} s`;
  el("r-steps").textContent = String(s.steps);
  const e = s.est;
  if (e && e.valid) {
    const floor = venue.levels.length > 1 ? `, ${venue.levels[e.level].name}` : "";
    el("r-pos").textContent = `${e.x.toFixed(1)}, ${e.y.toFixed(1)} m${floor}`;
    el("r-sigma").textContent = `${e.sigma.toFixed(1)} m`;
  } else {
    el("r-pos").textContent = "-";
    el("r-sigma").textContent = "-";
  }
  const fresh = s.compass !== null && s.clockMs - s.compassMs < COMPASS_STALE_MS;
  el("r-compass").textContent = fresh ? `${s.compass.toFixed(0)} deg` : s.samples ? "none" : "-";
  const span = s.recent.length > 1 ? (s.recent[s.recent.length - 1] - s.recent[0]) / 1000 : 0;
  el("r-rate").textContent = span > 0 ? `${((s.recent.length - 1) / span).toFixed(0)} Hz` : "-";
  // Engine time against the sensor time it has been fed.
  const fed = s.engineT || 0;
  el("r-load").textContent = fed > 1 ? `${(s.busyMs / fed).toFixed(1)} ms/s (${((s.busyMs / fed) / 10).toFixed(1)}%)` : "-";
  el("r-cells").textContent = s.cellCount ? s.cellCount.toLocaleString("en") : "-";
  const scored = s.marks.filter((m) => m.error !== null && m.error >= 0);
  el("r-marks").textContent = String(s.marks.length);
  el("r-error").textContent = scored.length
    ? `${(scored.reduce((sum, m) => sum + m.error, 0) / scored.length).toFixed(1)} m` : "-";
}

// ---- Input ------------------------------------------------------------------------------------------

const pointers = new Map(); // pointerId -> {x, y} in canvas px
let gesture = null; // {moved, pinch: distance between two pointers}

function canvasPoint(ev) {
  const r = canvas.getBoundingClientRect();
  return { x: ((ev.clientX - r.left) * canvas.width) / r.width, y: ((ev.clientY - r.top) * canvas.height) / r.height };
}

canvas.addEventListener("pointerdown", (ev) => {
  canvas.setPointerCapture(ev.pointerId);
  pointers.set(ev.pointerId, canvasPoint(ev));
  if (pointers.size === 1) gesture = { moved: 0, pinch: 0, multi: false };
  else gesture.multi = true;
});

canvas.addEventListener("pointermove", (ev) => {
  const before = pointers.get(ev.pointerId);
  if (!before || !gesture) return;
  const now = canvasPoint(ev);
  pointers.set(ev.pointerId, now);
  const pts = [...pointers.values()];
  if (pts.length === 1) {
    gesture.moved += Math.hypot(now.x - before.x, now.y - before.y);
    if (gesture.moved > TAP_SLOP_PX * (window.devicePixelRatio || 1)) {
      zoom.x += now.x - before.x;
      zoom.y += now.y - before.y;
      el("follow").checked = false;
    }
  } else if (pts.length === 2) {
    const d = Math.hypot(pts[0].x - pts[1].x, pts[0].y - pts[1].y);
    if (gesture.pinch > 0 && d > 0) zoomAt(d / gesture.pinch, (pts[0].x + pts[1].x) / 2, (pts[0].y + pts[1].y) / 2);
    gesture.pinch = d;
  }
});

const endPointer = (ev, cancelled) => {
  const at = pointers.get(ev.pointerId);
  pointers.delete(ev.pointerId);
  if (!gesture || pointers.size > 0) {
    if (gesture) gesture.pinch = 0;
    return;
  }
  const wasTap = !cancelled && !gesture.multi && gesture.moved <= TAP_SLOP_PX * (window.devicePixelRatio || 1);
  gesture = null;
  if (wasTap && at && venue) {
    const T = transform();
    tap(T.fromX(at.x), T.fromY(at.y));
  }
};
canvas.addEventListener("pointerup", (ev) => endPointer(ev, false));
canvas.addEventListener("pointercancel", (ev) => endPointer(ev, true));
canvas.addEventListener("wheel", (ev) => {
  ev.preventDefault();
  const p = canvasPoint(ev);
  zoomAt(ev.deltaY < 0 ? 1.2 : 1 / 1.2, p.x, p.y);
}, { passive: false });

el("zoom-in").addEventListener("click", () => zoomAt(1.5, canvas.width / 2, canvas.height / 2));
el("zoom-out").addEventListener("click", () => zoomAt(1 / 1.5, canvas.width / 2, canvas.height / 2));
el("zoom-fit").addEventListener("click", () => {
  zoom = { k: 1, x: 0, y: 0 };
  el("follow").checked = false;
});

el("start").addEventListener("click", () => {
  if (session && session.running) stop();
  else start().catch((err) => setStatus(String(err.message || err), true));
});
el("save").addEventListener("click", requestSave);
el("venue").addEventListener("change", () => {
  stop();
  session = null;
  el("save").disabled = true;
  if (el("venue").value === "file") {
    el("venue-file").click();
    return;
  }
  loadSelectedVenue().then(() => setStatus("Press Start.")).catch((err) => setStatus(String(err.message || err), true));
});
el("venue-file").addEventListener("change", async () => {
  const file = el("venue-file").files[0];
  if (!file) return;
  try {
    showVenue(await file.text(), { kind: "file", name: file.name });
    setStatus(`${file.name} loaded. Press Start.`);
  } catch (err) {
    setStatus(`${file.name} is not a venue file: ${err.message || err}`, true);
  }
});
setInterval(flush, FLUSH_MS);

loadSelectedVenue().then(() => requestAnimationFrame(draw));
