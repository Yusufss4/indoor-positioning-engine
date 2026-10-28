"use strict";

const COLORS = { truth: "#222", pf: "#d62728", ekf: "#1f77b4", wcl: "#2ca02c", wall: "#555", beacon: "#7f3fbf" };
const FILTERS = ["pf", "ekf", "wcl"];

const el = (id) => document.getElementById(id);
const canvas = el("map");
const ctx = canvas.getContext("2d");

let Module = null;
let demo = null;
let venue = null;
let playing = true;
let lastFrame = null;
let truthTrail = [];
let trails = {};
let pings = new Map();

async function loadJson(url) {
  const res = await fetch(url);
  if (!res.ok) throw new Error(`${url}: ${res.status}`);
  return res.text();
}

async function start() {
  const name = el("scenario").value;
  const scenarioUrl = new URL(`data/scenarios/${name}.json`, window.location.href);
  const scenarioText = await loadJson(scenarioUrl);
  const scenario = JSON.parse(scenarioText);
  const venueText = await loadJson(new URL(scenario.venue, scenarioUrl));
  venue = JSON.parse(venueText);
  el("description").textContent = scenario.description || "";

  if (demo) demo.delete();
  demo = new Module.Demo(venueText, scenarioText, Number(el("seed").value) >>> 0);
  truthTrail = [];
  trails = Object.fromEntries(FILTERS.map((f) => [f, []]));
  pings = new Map();
  lastFrame = null;
}

function transform() {
  const [x0, y0, x1, y1] = venue.bounds;
  const pad = 20;
  const s = Math.min((canvas.width - 2 * pad) / (x1 - x0), (canvas.height - 2 * pad) / (y1 - y0));
  const ox = pad + ((canvas.width - 2 * pad) - s * (x1 - x0)) / 2;
  const oy = pad + ((canvas.height - 2 * pad) - s * (y1 - y0)) / 2;
  return { s, toX: (x) => ox + (x - x0) * s, toY: (y) => canvas.height - (oy + (y - y0) * s) };
}

function drawPath(points, T, color, width) {
  if (points.length < 2) return;
  ctx.strokeStyle = color;
  ctx.lineWidth = width;
  ctx.beginPath();
  ctx.moveTo(T.toX(points[0][0]), T.toY(points[0][1]));
  for (const p of points) ctx.lineTo(T.toX(p[0]), T.toY(p[1]));
  ctx.stroke();
}

function dot(x, y, T, color, r, ring = false) {
  ctx.beginPath();
  ctx.arc(T.toX(x), T.toY(y), r, 0, 2 * Math.PI);
  ctx.fillStyle = color;
  ctx.fill();
  if (ring) {
    ctx.strokeStyle = "#fff";
    ctx.lineWidth = 2;
    ctx.stroke();
  }
}

function draw() {
  const T = transform();
  ctx.clearRect(0, 0, canvas.width, canvas.height);

  ctx.strokeStyle = COLORS.wall;
  ctx.lineWidth = 3;
  ctx.lineCap = "round";
  for (const [ax, ay, bx, by] of venue.walls) {
    ctx.beginPath();
    ctx.moveTo(T.toX(ax), T.toY(ay));
    ctx.lineTo(T.toX(bx), T.toY(by));
    ctx.stroke();
  }

  const now = demo.time();
  for (const b of venue.beacons) {
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
    for (let k = 0; k < p.length; k += 3) ctx.fillRect(T.toX(p[k]) - 1, T.toY(p[k + 1]) - 1, 2, 2);
  }

  drawPath(truthTrail, T, COLORS.truth, 2);
  for (const f of FILTERS) {
    if ((f === "ekf" && !el("show-ekf").checked) || (f === "wcl" && !el("show-wcl").checked)) continue;
    drawPath(trails[f], T, COLORS[f], 1.3);
    const e = demo.estimate(f);
    if (e.valid) {
      if (e.sigma > 0) {
        ctx.beginPath();
        ctx.arc(T.toX(e.x), T.toY(e.y), Math.max(4, e.sigma * T.s), 0, 2 * Math.PI);
        ctx.fillStyle = COLORS[f] + "22";
        ctx.fill();
      }
      dot(e.x, e.y, T, COLORS[f], 6, true);
    }
  }
  const t = demo.truth();
  dot(t.x, t.y, T, COLORS.truth, 6, true);
}

function updateStats() {
  el("clock").textContent = `t = ${demo.time().toFixed(1)} / ${demo.duration().toFixed(0)} s   steps: ${demo.steps()}`;
  const fmt = (v) => (v < 0 ? "-" : `${v.toFixed(2)} m`);
  el("stats").innerHTML = FILTERS.map((f) => {
    const e = demo.estimate(f);
    return `<tr><td><span class="swatch" style="background:${COLORS[f]}"></span>${f}</td>` +
      `<td>${fmt(e.error)}</td><td>${fmt(e.mean)}</td><td>${e.cep95 > 0 ? fmt(e.cep95) : "-"}</td></tr>`;
  }).join("");
}

function tick(ts) {
  if (demo && venue) {
    if (playing && !demo.finished()) {
      const dt = lastFrame === null ? 0 : Math.min((ts - lastFrame) / 1000, 0.1);
      demo.advance(dt * Number(el("speed").value));
      const t = demo.truth();
      truthTrail.push([t.x, t.y]);
      for (const f of FILTERS) {
        const e = demo.estimate(f);
        if (e.valid && demo.time() > 3) trails[f].push([e.x, e.y]);
      }
      for (const b of demo.recentBle()) pings.set(b.id, { t: demo.time(), rssi: b.rssi });
    }
    lastFrame = ts;
    draw();
    updateStats();
  }
  requestAnimationFrame(tick);
}

el("play").addEventListener("click", () => {
  playing = !playing;
  el("play").textContent = playing ? "Pause" : "Play";
});
el("restart").addEventListener("click", start);
el("scenario").addEventListener("change", start);

createIpeModule().then(async (m) => {
  Module = m;
  await start();
  requestAnimationFrame(tick);
}).catch((err) => {
  el("clock").textContent = `failed to load: ${err}`;
});
