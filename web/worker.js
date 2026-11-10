"use strict";

// Runs the engine off the page's thread and streams the frames it records to app.js. The grid filter is
// expensive: the first time it hears a Wi-Fi access point on a mall floor it counts the walls between every
// cell and the access point, about a second each, and the page must stay responsive meanwhile. app.js starts
// a new worker for every run and terminates the old one, so nothing here needs to be reset.
importScripts("ipe_wasm.js");

const FRAME_DT = 0.05; // simulated seconds per recorded frame (as in app.js)
const PARTICLE_EVERY = 4; // frames per particle and cell snapshot (5 Hz)
const MAX_RECORDED_PARTICLES = 1000;
const POST_EVERY_MS = 50; // frames are sent in batches

const modulePromise = createIpeModule();

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

/// The engine state after the last advance(). Snapshots of the clouds are taken every PARTICLE_EVERY frames;
/// in between they are null and the page keeps the previous ones.
function frame(demo, k, filters, transfer) {
  const est = {};
  const rooms = { truth: demo.currentRooms("truth") };
  for (const f of filters) {
    est[f] = demo.estimate(f);
    rooms[f] = demo.currentRooms(f);
  }
  const fr = { t: demo.time(), steps: demo.steps(), truth: demo.truth(), est, rooms, ble: demo.recentBle(),
               particles: null, cells: null };
  if (k % PARTICLE_EVERY === 0) {
    fr.particles = sampleParticles(demo.particles());
    transfer.push(fr.particles.buffer);
    if (demo.hasGrid()) {
      fr.cells = new Float32Array(demo.gridCells());
      transfer.push(fr.cells.buffer);
    }
  }
  return fr;
}

function run(demo, filters) {
  let k = 0;
  const loop = () => {
    const frames = [];
    const events = [];
    const transfer = [];
    const deadline = performance.now() + POST_EVERY_MS;
    do {
      if (k > 0) demo.advance(FRAME_DT);
      else demo.advance(0);
      frames.push(frame(demo, k++, filters, transfer));
      events.push(...demo.roomEvents());
    } while (!demo.finished() && performance.now() < deadline);
    const finished = demo.finished();
    postMessage({ type: "frames", frames, events, finished }, transfer);
    if (!finished) setTimeout(loop, 0);
  };
  loop();
}

onmessage = async ({ data }) => {
  if (data.type !== "start") return;
  try {
    const Module = await modulePromise;
    const t0 = performance.now();
    const demo = data.trace !== undefined
      ? Module.Demo.replay(data.venue, data.trace, data.config, data.grid)
      : new Module.Demo(data.venue, data.scenario, 1, data.config, data.grid);
    const filters = demo.hasGrid() ? ["pf", "grid"] : ["pf"];
    postMessage({
      type: "info", duration: demo.duration(), filters, gridCellSize: demo.gridCellSize(),
      gridCells: demo.gridCellCount(), setupMs: performance.now() - t0,
    });
    run(demo, filters);
  } catch (err) {
    postMessage({ type: "error", message: String(err && err.message ? err.message : err) });
  }
};
