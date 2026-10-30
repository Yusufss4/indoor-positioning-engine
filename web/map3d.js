"use strict";

// 3D view of the venue and the live estimates, drawn with MapLibre GL JS on a
// blank style (no tile keys). MapLibre is fetched from a CDN the first time the
// view is opened, so the 2D demo keeps working offline.
//
// Input is the GeoJSON written by `ipe-import export` (data/geojson/*.geojson):
// walls are thin polygons extruded per floor, levels become slabs, connectors
// become shafts and spaces (rooms) are tinted on their floor. Everything dynamic (dots, uncertainty, particles) is drawn as
// small extruded polygons because circle layers cannot be lifted off the ground.

const MAPLIBRE = {
  js: "https://unpkg.com/maplibre-gl@4.7.1/dist/maplibre-gl.js",
  jsIntegrity: "sha384-SYKAG6cglRMN0RVvhNeBY0r3FYKNOJtznwA0v7B5Vp9tr31xAHsZC0DqkQ/pZDmj",
  css: "https://unpkg.com/maplibre-gl@4.7.1/dist/maplibre-gl.css",
  cssIntegrity: "sha384-MinO0mNliZ3vwppuPOUnGa+iq619pfMhLVUXfC4LHwSCvF9H+6P/KO4Q7qBOYV5V",
};

const LEVEL_COLORS = ["#4c78a8", "#f58518", "#54a24b", "#b279a2", "#9d755d", "#e45756"];
const MAX_PARTICLES_3D = 1200;

let maplibrePromise = null;

function loadMapLibre() {
  if (window.maplibregl) return Promise.resolve(window.maplibregl);
  if (maplibrePromise) return maplibrePromise;
  maplibrePromise = new Promise((resolve, reject) => {
    const css = document.createElement("link");
    css.rel = "stylesheet";
    css.href = MAPLIBRE.css;
    css.integrity = MAPLIBRE.cssIntegrity;
    css.crossOrigin = "anonymous";
    document.head.appendChild(css);
    const js = document.createElement("script");
    js.src = MAPLIBRE.js;
    js.integrity = MAPLIBRE.jsIntegrity;
    js.crossOrigin = "anonymous";
    js.onload = () => resolve(window.maplibregl);
    js.onerror = () => {
      maplibrePromise = null;
      reject(new Error("could not load MapLibre GL JS from the CDN"));
    };
    document.head.appendChild(js);
  });
  return maplibrePromise;
}

/// Local metric frame <-> WGS84; the same tangent-plane maths as core/src/georef.cpp.
class GeoFrame {
  constructor({ origin_lat, origin_lon, rotation_deg = 0 }) {
    const a = 6378137.0;
    const e2 = 6.69437999014e-3;
    const phi = (origin_lat * Math.PI) / 180;
    const w = 1 - e2 * Math.sin(phi) ** 2;
    this.lat0 = origin_lat;
    this.lon0 = origin_lon;
    this.mLat = (a * (1 - e2)) / (w * Math.sqrt(w));
    this.mLon = (a / Math.sqrt(w)) * Math.cos(phi);
    this.cos = Math.cos((rotation_deg * Math.PI) / 180);
    this.sin = Math.sin((rotation_deg * Math.PI) / 180);
  }

  toLngLat(x, y) {
    const east = x * this.cos - y * this.sin;
    const north = x * this.sin + y * this.cos;
    return [this.lon0 + (east / this.mLon) * (180 / Math.PI), this.lat0 + (north / this.mLat) * (180 / Math.PI)];
  }
}

function ring(frame, cx, cy, r, n) {
  const pts = [];
  for (let k = 0; k <= n; ++k) {
    const a = (2 * Math.PI * (k % n)) / n;
    pts.push(frame.toLngLat(cx + r * Math.cos(a), cy + r * Math.sin(a)));
  }
  return { type: "Polygon", coordinates: [pts] };
}

function extruded(geometry, base, top, color) {
  return { type: "Feature", geometry, properties: { base, top, color } };
}

const EMPTY = { type: "FeatureCollection", features: [] };

class Map3D {
  constructor(container) {
    this.container = container;
    this.map = null;
    this.levels = [];
    this.exploded = 0;
    this.follow = false;
    this.focus = 0;
    this.lastUpdate = 0;
    this.venue = null;
  }

  async init() {
    if (this.map) return;
    const maplibregl = await loadMapLibre();
    this.map = new maplibregl.Map({
      container: this.container,
      style: {
        version: 8,
        sources: {
          osm: {
            type: "raster",
            tiles: ["https://tile.openstreetmap.org/{z}/{x}/{y}.png"],
            tileSize: 256,
            maxzoom: 19,
            attribution: "© OpenStreetMap contributors",
          },
        },
        layers: [
          { id: "background", type: "background", paint: { "background-color": "#fbfcfe" } },
          { id: "osm", type: "raster", source: "osm", layout: { visibility: "none" }, paint: { "raster-opacity": 0.6 } },
        ],
      },
      center: [0, 0],
      zoom: 18,
      pitch: 55,
      bearing: -25,
      maxPitch: 85,
      attributionControl: { compact: true },
    });
    this.map.addControl(new maplibregl.NavigationControl({ visualizePitch: true }), "top-right");
    await new Promise((resolve) => this.map.on("load", resolve));
  }

  /// `geojson` is an `ipe-import export` FeatureCollection.
  setVenue(geojson) {
    const map = this.map;
    for (const id of this.layerIds || []) if (map.getLayer(id)) map.removeLayer(id);
    for (const id of this.sourceIds || []) if (map.getSource(id)) map.removeSource(id);
    this.layerIds = [];
    this.sourceIds = [];

    const byType = (t) => geojson.features.filter((f) => f.properties.feature_type === t);
    const venueFeature = byType("venue")[0];
    if (!venueFeature) throw new Error("GeoJSON has no venue feature (not written by ipe-import export?)");
    this.frame = new GeoFrame(venueFeature.properties);
    this.venue = venueFeature;
    this.levels = byType("level")
      .map((f) => ({ ordinal: f.properties.ordinal, name: f.properties.name, elevation: f.properties.elevation_m,
                     height: f.properties.height_m, outline: f.geometry }))
      .sort((a, b) => a.ordinal - b.ordinal);
    const indexOf = new Map(this.levels.map((l, i) => [l.ordinal, i]));
    this.static = {
      walls: byType("wall").map((f) => ({ level: indexOf.get(f.properties.level), geometry: f.geometry })),
      beacons: byType("beacon").map((f) => ({ level: indexOf.get(f.properties.level), id: f.properties.id,
                                               lngLat: f.geometry.coordinates })),
      connectors: geojson.features
        .filter((f) => ["stairs", "elevator", "escalator"].includes(f.properties.feature_type))
        .map((f) => ({ levels: f.properties.levels.map((o) => indexOf.get(o)), geometry: f.geometry,
                       kind: f.properties.feature_type })),
      spaces: byType("space").map((f) => ({ level: indexOf.get(f.properties.level), id: f.properties.id,
                                            geometry: f.geometry })),
    };
    this.spaceById = new Map(this.static.spaces.map((s) => [s.id, s]));
    this.activeRooms = "";

    const paint = (opacity) => ({
      "fill-extrusion-color": ["get", "color"],
      "fill-extrusion-base": ["get", "base"],
      "fill-extrusion-height": ["get", "top"],
      "fill-extrusion-opacity": opacity,
    });
    const add = (id, opacity) => {
      map.addSource(id, { type: "geojson", data: EMPTY });
      map.addLayer({ id, type: "fill-extrusion", source: id, paint: paint(opacity) });
      this.sourceIds.push(id);
      this.layerIds.push(id);
    };
    // Extrusion layers share one depth buffer, so a nearly transparent floor drawn first would still hide
    // everything behind it: the live layers go first and the building is blended over them.
    add("dots", 1.0);
    add("particles", 0.7);
    add("uncertainty", 0.25);
    add("rooms", 0.3);
    // fill-extrusion-opacity is per layer, hence one layer per floor.
    this.levels.forEach((_, i) => add(`level-${i}`, 0.85));
    add("connectors", 0.35);

    this.rebuildStatic();
    const [minLng, minLat, maxLng, maxLat] = this.bbox(venueFeature.geometry.coordinates[0]);
    map.fitBounds([[minLng, minLat], [maxLng, maxLat]], { padding: 40, pitch: 55, bearing: -25, duration: 0 });
  }

  bbox(coords) {
    const lngs = coords.map((c) => c[0]);
    const lats = coords.map((c) => c[1]);
    return [Math.min(...lngs), Math.min(...lats), Math.max(...lngs), Math.max(...lats)];
  }

  /// Display height of a point `dz` metres above the floor of level `i`.
  z(i, dz = 0) {
    const l = this.levels[i] || this.levels[0];
    return l.elevation + this.exploded * i * l.height + dz;
  }

  wallHeight(i) {
    return 0.65 * (this.levels[i] || this.levels[0]).height;
  }

  rebuildStatic() {
    if (!this.map || !this.levels.length) return;
    this.levels.forEach((l, i) => {
      const fs = [extruded(l.outline, this.z(i, -0.15), this.z(i), "#dfe6ef")];
      for (const s of this.static.spaces) {
        if (s.level === i) fs.push(extruded(s.geometry, this.z(i), this.z(i, 0.02), "#cfdcec"));
      }
      for (const w of this.static.walls) {
        if (w.level === i) fs.push(extruded(w.geometry, this.z(i), this.z(i, this.wallHeight(i)), "#88a2c2"));
      }
      for (const b of this.static.beacons) {
        if (b.level !== i) continue;
        const [x, y] = this.toLocal(b.lngLat);
        const h = this.wallHeight(i);
        fs.push(extruded(ring(this.frame, x, y, 0.2, 4), this.z(i, h - 0.25), this.z(i, h), "#7f3fbf"));
      }
      this.map.getSource(`level-${i}`).setData({ type: "FeatureCollection", features: fs });
    });
    const shafts = this.static.connectors.map((c) => {
      const lo = Math.min(...c.levels);
      const hi = Math.max(...c.levels);
      return extruded(c.geometry, this.z(lo, 0.01), this.z(hi, this.wallHeight(hi)),
                      c.kind === "elevator" ? "#ff9f1c" : "#2ec4b6");
    });
    this.map.getSource("connectors").setData({ type: "FeatureCollection", features: shafts });
    this.activeRooms = null;
    this.applyFocus();
  }

  /// Inverse of GeoFrame.toLngLat for beacon points (only needs to be approximate).
  toLocal([lng, lat]) {
    const f = this.frame;
    const east = ((lng - f.lon0) * Math.PI) / 180 * f.mLon;
    const north = ((lat - f.lat0) * Math.PI) / 180 * f.mLat;
    return [east * f.cos + north * f.sin, -east * f.sin + north * f.cos];
  }

  /// The focused floor is opaque, floors above it nearly vanish so you can see in.
  applyFocus() {
    this.levels.forEach((_, i) => {
      const opacity = i === this.focus ? 0.9 : i < this.focus ? 0.35 : 0.07;
      this.map.setPaintProperty(`level-${i}`, "fill-extrusion-opacity", opacity);
    });
  }

  setExploded(v) {
    this.exploded = v;
    this.rebuildStatic();
  }

  setUnderlay(on) {
    if (this.map) this.map.setLayoutProperty("osm", "visibility", on ? "visible" : "none");
  }

  resize() {
    if (this.map) this.map.resize();
  }

  /// frame: {truth, estimates: [{name, color, e}], particles: Float32Array (x, y, w, level), rooms: [room id],
  /// focusLevel}
  update(frame, force = false) {
    if (!this.map || !this.levels.length) return;
    const now = performance.now();
    if (!force && now - this.lastUpdate < 60) return;  // ~15 Hz is plenty and keeps setData cheap
    this.lastUpdate = now;

    if (frame.focusLevel !== this.focus) {
      this.focus = frame.focusLevel;
      this.applyFocus();
    }

    const roomKey = (frame.rooms || []).join("\n");
    if (roomKey !== this.activeRooms) {
      this.activeRooms = roomKey;
      const fs = (frame.rooms || []).map((id) => this.spaceById.get(id)).filter(Boolean)
        .map((s) => extruded(s.geometry, this.z(s.level, 0.02), this.z(s.level, 0.08), "#d62728"));
      this.map.getSource("rooms").setData({ type: "FeatureCollection", features: fs });
    }

    const dots = [];
    const halos = [];
    for (const { color, e } of frame.estimates) {
      if (!e.valid) continue;
      halos.push(extruded(ring(this.frame, e.x, e.y, Math.max(0.3, e.sigma), 32), this.z(e.level, 0.03),
                          this.z(e.level, 0.06), color));
      dots.push(extruded(ring(this.frame, e.x, e.y, 0.6, 16), this.z(e.level, 0.1), this.z(e.level, 0.6), color));
    }
    const t = frame.truth;
    dots.push(extruded(ring(this.frame, t.x, t.y, 0.3, 12), this.z(t.level), this.z(t.level, 1.8), "#222222"));

    const parts = [];
    const p = frame.particles;
    if (p && p.length) {
      const n = p.length / 4;
      const stride = Math.max(1, Math.ceil(n / MAX_PARTICLES_3D));
      for (let k = 0; k < n; k += stride) {
        const [x, y, level] = [p[4 * k], p[4 * k + 1], p[4 * k + 3]];
        parts.push(extruded(ring(this.frame, x, y, 0.09, 4), this.z(level, 0.05), this.z(level, 0.12),
                            LEVEL_COLORS[level % LEVEL_COLORS.length]));
      }
    }

    this.map.getSource("dots").setData({ type: "FeatureCollection", features: dots });
    this.map.getSource("uncertainty").setData({ type: "FeatureCollection", features: halos });
    this.map.getSource("particles").setData({ type: "FeatureCollection", features: parts });

    if (this.follow) {
      const pf = frame.estimates.find((s) => s.e.valid);
      if (pf) this.map.jumpTo({ center: this.frame.toLngLat(pf.e.x, pf.e.y) });
    }
  }
}

window.Map3D = Map3D;
window.GeoFrame = GeoFrame;
