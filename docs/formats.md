# File formats

## Venue (`data/venues/*.json`)

```json
{
  "name": "office_floor",
  "bounds": [min_x, min_y, max_x, max_y],
  "wall_attenuation_db": 3.5,
  "walls": [[x1, y1, x2, y2], ...],
  "beacons": [{"id": "C1", "x": 5, "y": 11.5, "tx_power": -59, "path_loss_exp": 2.0}]
}
```

- Units are metres in a local east-north frame (x east, y north).
- Walls are line segments; they block pedestrians (particle-filter motion constraint) and attenuate radio by
  `wall_attenuation_db` each in the map-aware RSSI model.
- `tx_power` is the expected RSSI at 1 m. `path_loss_exp` is the log-distance exponent. These are the
  *surveyed* values; scenarios can make the real ones differ.

## Scenario (`data/scenarios/*.json`)

| Section | Key | Meaning |
|---|---|---|
| top | `venue` | Venue file, relative to the scenario file |
| top | `seed` | Default simulation seed |
| `walk` | `waypoints` | Polyline followed by a pure-pursuit walker |
| `walk` | `step_frequency_hz`, `step_length_m`, `step_length_sigma`, `turn_rate_deg_s` | Gait |
| `imu` | `rate_hz`, `accel_noise`, `gyro_noise`, `gyro_bias` | Inertial sensor model |
| `imu` | `weinberg_k` | The walker's true Weinberg constant (the engine assumes 0.48) |
| `imu` | `mag_disturbances` | `[{center, radius, field}]` local magnetic anomalies (uT, world frame) |
| `ble` | `noise_db`, `shadowing_db`, `shadowing_tau_s` | Fast fading and AR(1) slow fading |
| `ble` | `wall_attenuation_db`, `path_loss_exp_delta` | True propagation vs. the surveyed model |
| `ble` | `device_offset_db` | Receiver RSSI bias (phone model differences) |
| `ble` | `dropout`, `rx_interval_s`, `sensitivity_dbm` | Packet reception |
| `ble` | `dead_beacons`, `moved_beacons` | Deployment faults: silent beacons, beacons not where the map says |

## Trace (`*.csv`)

Line-oriented, one sample per row, merged in timestamp order. Lines starting with `#` are comments;
`# key: value` comments are metadata.

```
# ipe-trace v1
# scenario: office_nominal
imu,t,ax,ay,az,gx,gy,gz,mx,my,mz
ble,t,beacon_id,rssi
gt,t,x,y,heading
```

- IMU in the phone frame (phone flat, x forward): m/s^2, rad/s, uT.
- `gt` rows are optional ground truth; replay scores the estimate at each one.
- A phone logger that writes this format can be replayed directly.

## Replay outputs

- `--out`: `t,truth_x,truth_y,est_x,est_y,sigma_m,error_m,has_fix`, one row per ground-truth sample.
- `--particles`: `t,x,y,w`, subsampled particle cloud every N ground-truth samples.
- stdout / `--metrics`: JSON with `mean_m`, `rmse_m`, `cep50_m`, `cep95_m`, `max_m`, `availability`,
  `time_to_first_fix_s`, `convergence_s`, `steps`, `compute_us_per_update`.

`cep50`/`cep95` are the 50th/95th percentiles of horizontal error after a 5 s warm-up.
