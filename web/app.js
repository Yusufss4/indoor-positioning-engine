"use strict";

const COLORS = { truth: "#222", pf: "#d62728", ekf: "#1f77b4", wcl: "#2ca02c", wall: "#555", beacon: "#7f3fbf" };
const FILTERS = ["pf", "ekf", "wcl"];

const el = (id) => document.getElementById(id);
const canvas = el("map");
const ctx = canvas.getContext("2d");

let Module = null;
let demo = null;
let venue = null;
let geojsonUrl = null;
let playing = true;
let lastFrame = null;
let truthTrail = [];
let trails = {};
let pings = new Map();
let view = "2d";
let map3d = null;

async function loadJson(url) {
  const res = await fetch(url);
  if (!res.ok) throw new Error(`${url}: ${res.status}`);
  return res.text();
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
  };
}

async function start() {
  const name = el("scenario").value;
  const scenarioUrl = new URL(`data/scenarios/${name}.json`, window.location.href);
  const scenarioText = await loadJson(scenarioUrl);
  const scenario = JSON.parse(scenarioText);
  const venueUrl = new URL(scenario.venue, scenarioUrl);
  const venueText = await loadJson(venueUrl);
  venue = normalizeVenue(JSON.parse(venueText));
  geojsonUrl = new URL(venueUrl.href.replace(/\/venues\/([^/]+)\.json$/, "/geojson/$1.geojson"));
  el("description").textContent = scenario.description || "";

  if (demo) demo.delete();
  demo = new Module.Demo(venueText, scenarioText, Number(el("seed").value) >>> 0);
  truthTrail = [];
  trails = Object.fromEntries(FILTERS.map((f) => [f, []]));
  pings = new Map();
  lastFrame = null;

  const floorSel = el("floor");
  floorSel.innerHTML = `<option value="auto">follow walker</option>` +
    venue.levels.map((l, i) => `<option value="${i}">${l.name}</option>`).join("");
  el("floor-control").hidden = venue.levels.length < 2;
  el("floor-head").hidden = venue.levels.length < 2;

  if (view === "3d") await show3d();
}

function shownLevel() {
  const v = el("floor").value;
  return v === "auto" ? demo.truth().level : Number(v);
}

function transform() {
  const [x0, y0, x1, y1] = venue.bounds;
  const pad = 20;
  const s = Math.min((canvas.width - 2 * pad) / (x1 - x0), (canvas.height - 2 * pad) / (y1 - y0));
  const ox = pad + ((canvas.width - 2 * pad) - s * (x1 - x0)) / 2;
  const oy = pad + ((canvas.height - 2 * pad) - s * (y1 - y0)) / 2;
  return { s, toX: (x) => ox + (x - x0) * s, toY: (y) => canvas.height - (oy + (y - y0) * s) };
}

/// Draws the parts of a [x, y, level] trail that lie on `level`.
function drawPath(points, T, color, width, level) {
  ctx.strokeStyle = color;
  ctx.lineWidth = width;
  ctx.beginPath();
  let pen = false;
  for (const [x, y, l] of points) {
    if (l !== level) {
      pen = false;
      continue;
    }
    if (pen) ctx.lineTo(T.toX(x), T.toY(y));
    else ctx.moveTo(T.toX(x), T.toY(y));
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
    ctx.strokeStyle = "#fff";
    ctx.lineWidth = 2;
    ctx.stroke();
  }
}

function draw(level) {
  const T = transform();
  ctx.clearRect(0, 0, canvas.width, canvas.height);

  for (const c of venue.connectors) {
    if (!c.levels.includes(level)) continue;
    ctx.beginPath();
    c.area.forEach(([x, y], k) => (k ? ctx.lineTo(T.toX(x), T.toY(y)) : ctx.moveTo(T.toX(x), T.toY(y))));
    ctx.closePath();
    ctx.fillStyle = c.kind === "elevator" ? "rgba(255, 159, 28, 0.3)" : "rgba(46, 196, 182, 0.3)";
    ctx.fill();
    ctx.fillStyle = "#555";
    ctx.font = "10px system-ui";
    ctx.fillText(c.kind, T.toX(c.area[0][0]) + 3, T.toY(c.area[0][1]) - 3);
  }

  ctx.strokeStyle = COLORS.wall;
  ctx.lineWidth = 3;
  ctx.lineCap = "round";
  for (const [ax, ay, bx, by] of venue.levels[level].walls) {
    ctx.beginPath();
    ctx.moveTo(T.toX(ax), T.toY(ay));
    ctx.lineTo(T.toX(bx), T.toY(by));
    ctx.stroke();
  }

  const now = demo.time();
  for (const b of venue.beacons) {
    if (b.level !== level) continue;
    const ping = pings.get(b.id);
    if (ping && now - ping.t < 0.6) {
      const age = (now - ping.t) / 0.6;
      ctx.beginPath();
      ctx.arc(T.toX(b.x), T.toY(b.y), 6 + 22 * age, 0, 2 * Math.PI);
      ctx.strokeStyle = `rgba(127, 63, 191, ${0.5 * (1 - age)})`;
      ctx.lineWidth = 2;
      ctx.stroke();
    }
    ctx.fillStyle = COLORS.beacon;
    ctx.beginPath();
    ctx.moveTo(T.toX(b.x), T.toY(b.y) - 7);
    ctx.lineTo(T.toX(b.x) - 6, T.toY(b.y) + 5);
    ctx.lineTo(T.toX(b.x) + 6, T.toY(b.y) + 5);
    ctx.closePath();
    ctx.fill();
    ctx.font = "10px system-ui";
    ctx.fillText(b.id, T.toX(b.x) + 7, T.toY(b.y) - 6);
  }

  if (el("show-particles").checked) {
    const p = demo.particles();
    ctx.fillStyle = "rgba(214, 39, 40, 0.25)";
    for (let k = 0; k < p.length; k += 4) {
      if (p[k + 3] === level) ctx.fillRect(T.toX(p[k]) - 1, T.toY(p[k + 1]) - 1, 2, 2);
    }
  }

  drawPath(truthTrail, T, COLORS.truth, 2, level);
  for (const f of visibleFilters()) {
    drawPath(trails[f], T, COLORS[f], 1.3, level);
    const e = demo.estimate(f);
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
  const t = demo.truth();
  dot(t.x, t.y, T, COLORS.truth, 6, true, t.level !== level);
}

function visibleFilters() {
  return FILTERS.filter((f) => !((f === "ekf" && !el("show-ekf").checked) || (f === "wcl" && !el("show-wcl").checked)));
}

function updateStats() {
  const t = demo.truth();
  const multi = venue.levels.length > 1;
  const levelText = multi ? `   floor: ${venue.levels[t.level].name}` : "";
  el("clock").textContent =
    `t = ${demo.time().toFixed(1)} / ${demo.duration().toFixed(0)} s   steps: ${demo.steps()}${levelText}`;
  const fmt = (v) => (v < 0 ? "-" : `${v.toFixed(2)} m`);
  const pct = (v) => (v < 0 ? "-" : `${(100 * v).toFixed(1)}%`);
  el("stats").innerHTML = FILTERS.map((f) => {
    const e = demo.estimate(f);
    const floor = multi ? `<td>${e.valid ? venue.levels[e.level].name : "-"} (${pct(e.floorAcc)})</td>` : "";
    return `<tr><td><span class="swatch" style="background:${COLORS[f]}"></span>${f}</td>` +
      `<td>${fmt(e.error)}</td><td>${fmt(e.mean)}</td><td>${e.cep95 > 0 ? fmt(e.cep95) : "-"}</td>${floor}</tr>`;
  }).join("");
}

function currentFrame(level) {
  return {
    t: demo.time(),
    truth: demo.truth(),
    estimates: visibleFilters().map((f) => ({ name: f, color: COLORS[f], e: demo.estimate(f) })),
    particles: el("show-particles").checked ? demo.particles().slice() : null,
    focusLevel: level,
    levels: venue.levels,
  };
}

function tick(ts) {
  if (demo && venue) {
    if (playing && !demo.finished()) {
      const dt = lastFrame === null ? 0 : Math.min((ts - lastFrame) / 1000, 0.1);
      demo.advance(dt * Number(el("speed").value));
      const t = demo.truth();
      truthTrail.push([t.x, t.y, t.level]);
      for (const f of FILTERS) {
        const e = demo.estimate(f);
        if (e.valid && demo.time() > 3) trails[f].push([e.x, e.y, e.level]);
      }
      for (const b of demo.recentBle()) pings.set(b.id, { t: demo.time(), rssi: b.rssi });
    }
    lastFrame = ts;
    const level = shownLevel();
    if (view === "2d") draw(level);
    if (view === "3d" && map3d) map3d.update(currentFrame(level));
    updateStats();
  }
  requestAnimationFrame(tick);
}

async function show3d() {
  const status = el("view3d-status");
  try {
    status.textContent = "loading 3D view...";
    if (!map3d) {
      map3d = new Map3D(el("map3d"));
      await map3d.init();
      map3d.exploded = Number(el("exploded").value);
      map3d.follow = el("follow").checked;
    }
    const res = await fetch(geojsonUrl);
    if (!res.ok) throw new Error(`no GeoJSON export for this venue (${geojsonUrl.pathname}); run ipe-import export`);
    map3d.setVenue(await res.json());
    map3d.setUnderlay(el("underlay").checked);
    map3d.resize();
    status.textContent = "";
  } catch (err) {
    status.textContent = `3D view unavailable: ${err.message}`;
    setView("2d");
  }
}

function setView(v) {
  view = v;
  el("map").hidden = v !== "2d";
  el("map3d").hidden = v !== "3d";
  el("controls3d").hidden = v !== "3d";
  el("view2d").classList.toggle("active", v === "2d");
  el("view3d").classList.toggle("active", v === "3d");
}

el("play").addEventListener("click", () => {
  playing = !playing;
  el("play").textContent = playing ? "Pause" : "Play";
});
el("restart").addEventListener("click", start);
el("scenario").addEventListener("change", start);
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
  await start();
  requestAnimationFrame(tick);
}).catch((err) => {
  el("clock").textContent = `failed to load: ${err}`;
});
