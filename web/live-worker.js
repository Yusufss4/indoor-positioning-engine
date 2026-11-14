"use strict";

// The engine for live.js, off the page's thread: it is fed the phone's sensor samples as they arrive and
// answers with the estimate. Everything it is fed is kept inside the module as a trace (bindings.cpp, Live).
importScripts("ipe_wasm.js");

const IMU_STRIDE = 10; // t, ax, ay, az, gx, gy, gz, mx, my, mz (as in live.js)
const CELLS_EVERY_MS = 200; // snapshots of the filter's cells

const modulePromise = createIpeModule();
let live = null;
let busyMs = 0; // time spent inside the engine
let lastCells = -Infinity;

/// The engine's state after what it has been fed so far.
function state(extra = {}) {
  const msg = { type: "state", t: live.time(), steps: live.steps(), est: live.estimate(), busyMs, cells: null, ...extra };
  const transfer = [];
  const now = performance.now();
  if (now - lastCells >= CELLS_EVERY_MS || extra.cellsNow) {
    lastCells = now;
    msg.cells = new Float32Array(live.gridCells());
    transfer.push(msg.cells.buffer);
  }
  postMessage(msg, transfer);
}

onmessage = async ({ data }) => {
  try {
    if (data.type === "start") {
      const Module = await modulePromise;
      const t0 = performance.now();
      live = new Module.Live(data.venue, data.config || "");
      for (const [key, value] of Object.entries(data.metadata || {})) live.setMetadata(key, String(value));
      busyMs = 0;
      postMessage({
        type: "info", cellSize: live.gridCellSize(), cellCount: live.gridCellCount(), levels: live.levelCount(),
        setupMs: performance.now() - t0,
      });
      state({ cellsNow: true });
      return;
    }
    if (!live) return;
    if (data.type === "imu") {
      const s = data.samples;
      const t0 = performance.now();
      for (let k = 0; k + IMU_STRIDE <= s.length; k += IMU_STRIDE) {
        live.onImu(s[k], s[k + 1], s[k + 2], s[k + 3], s[k + 4], s[k + 5], s[k + 6], s[k + 7], s[k + 8], s[k + 9]);
      }
      busyMs += performance.now() - t0;
      state();
    } else if (data.type === "fix") {
      const t0 = performance.now();
      live.onPositionFix(data.t, data.x, data.y, data.level, data.sigma);
      busyMs += performance.now() - t0;
      state({ cellsNow: true });
    } else if (data.type === "mark") {
      const error = live.mark(data.t, data.x, data.y, data.level);
      postMessage({ type: "mark", id: data.id, error, est: live.estimate() });
    } else if (data.type === "metadata") {
      live.setMetadata(data.key, String(data.value));
    } else if (data.type === "trace") {
      postMessage({ type: "trace", csv: live.trace() });
    }
  } catch (err) {
    postMessage({ type: "error", message: String(err && err.message ? err.message : err) });
  }
};
