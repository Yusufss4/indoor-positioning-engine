| Scenario | Filter | Mean [m] | CEP50 [m] | CEP95 [m] | Converged in [s] | Floor ok | Floor switch [s] | us/update |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| airport_hall_sparse | pf | 2.47 | 1.30 | 7.25 | 70.5 | - | - | 41.0 |
| airport_hall_sparse | pf-nomap | 2.59 | 1.41 | 7.58 | 76.0 | - | - | 22.5 |
| airport_hall_sparse | ekf | 3.55 | 1.61 | 10.47 | 89.2 | - | - | 0.1 |
| airport_hall_sparse | wcl | 5.54 | 4.73 | 12.85 | 156.1 (2/10) | - | - | 0.1 |
| baro_drift_long | pf | 0.83 | 0.63 | 1.99 | 13.3 | 99.5% | 0.4 | 90.2 |
| baro_drift_long | pf-nomap | 0.98 | 0.83 | 2.18 | 7.8 | 99.3% | 0.5 | 50.8 |
| baro_drift_long | ekf | 1.24 | 0.83 | 4.19 | 2.8 | 67.4% | 25.2 | 0.1 |
| baro_drift_long | wcl | 2.74 | 2.55 | 5.75 | 97.9 | 98.4% | 1.4 | 0.2 |
| office_device_offset | pf | 0.93 | 0.75 | 2.05 | 6.4 | - | - | 45.3 |
| office_device_offset | pf-nomap | 1.18 | 0.96 | 2.77 | 10.5 | - | - | 27.7 |
| office_device_offset | ekf | 7.06 | 7.00 | 9.45 | 29.1 (4/10) | - | - | 0.1 |
| office_device_offset | wcl | 2.60 | 2.48 | 5.31 | 93.1 (3/10) | - | - | 0.1 |
| office_elevator | pf | 0.79 | 0.60 | 1.76 | 1.0 | 99.5% | 0.4 | 82.4 |
| office_elevator | pf-nomap | 1.01 | 0.87 | 2.04 | 0.5 | 99.3% | 0.4 | 46.6 |
| office_elevator | ekf | 0.84 | 0.76 | 1.79 | 2.1 | 98.9% | 0.9 | 0.1 |
| office_elevator | wcl | 3.06 | 2.80 | 6.58 | 66.7 (7/10) | 98.3% | 1.2 | 0.2 |
| office_field_issue | pf | 0.89 | 0.72 | 2.00 | 2.4 | - | - | 63.0 |
| office_field_issue | pf-nomap | 1.33 | 1.01 | 3.67 | 5.2 | - | - | 34.0 |
| office_field_issue | ekf | 2.91 | 2.85 | 5.75 | 21.7 (9/10) | - | - | 0.1 |
| office_field_issue | wcl | 3.97 | 2.99 | 12.95 | 63.0 (7/10) | - | - | 0.1 |
| office_floor_bleed | pf | 1.33 | 0.89 | 4.02 | 11.4 | 96.3% | 1.0 | 103.7 |
| office_floor_bleed | pf-nomap | 2.19 | 1.53 | 6.17 | 22.6 | 94.2% | 0.6 | 65.5 |
| office_floor_bleed | ekf | 1.19 | 0.95 | 3.13 | 14.1 | 95.7% | 2.9 | 0.1 |
| office_floor_bleed | wcl | 2.65 | 2.55 | 5.30 | 64.3 (5/10) | 95.2% | 2.1 | 0.3 |
| office_mag_disturbance | pf | 1.24 | 1.09 | 2.63 | 3.7 | - | - | 78.8 |
| office_mag_disturbance | pf-nomap | 1.60 | 1.45 | 3.29 | 0.7 | - | - | 41.1 |
| office_mag_disturbance | ekf | 1.83 | 1.60 | 3.85 | 14.6 | - | - | 0.1 |
| office_mag_disturbance | wcl | 2.41 | 2.28 | 4.93 | 36.1 (7/10) | - | - | 0.1 |
| office_nlos_heavy | pf | 0.77 | 0.65 | 1.87 | 2.6 | - | - | 50.6 |
| office_nlos_heavy | pf-nomap | 1.01 | 0.86 | 2.22 | 5.6 | - | - | 31.9 |
| office_nlos_heavy | ekf | 1.16 | 0.98 | 2.72 | 11.0 | - | - | 0.1 |
| office_nlos_heavy | wcl | 2.43 | 2.31 | 5.14 | 50.9 (7/10) | - | - | 0.1 |
| office_nominal | pf | 0.75 | 0.66 | 1.59 | 1.4 | - | - | 76.2 |
| office_nominal | pf-nomap | 0.94 | 0.82 | 2.23 | 6.0 | - | - | 40.1 |
| office_nominal | ekf | 1.37 | 0.95 | 4.13 | 16.1 | - | - | 0.1 |
| office_nominal | wcl | 2.41 | 2.32 | 4.88 | 49.0 (7/10) | - | - | 0.2 |
| office_stairs_up_down | pf | 1.11 | 0.78 | 3.22 | 11.2 | 99.5% | 0.4 | 89.8 |
| office_stairs_up_down | pf-nomap | 1.26 | 1.12 | 2.61 | 10.6 | 99.3% | 0.3 | 50.4 |
| office_stairs_up_down | ekf | 1.07 | 0.97 | 2.26 | 3.0 | 98.7% | 1.0 | 0.1 |
| office_stairs_up_down | wcl | 2.74 | 2.58 | 5.56 | 41.1 (7/10) | 98.1% | 1.4 | 0.2 |
| office_step_scale | pf | 0.84 | 0.73 | 1.87 | 2.0 | - | - | 74.4 |
| office_step_scale | pf-nomap | 0.94 | 0.81 | 2.28 | 4.7 | - | - | 40.5 |
| office_step_scale | ekf | 1.11 | 0.97 | 2.55 | 9.3 | - | - | 0.1 |
| office_step_scale | wcl | 2.34 | 2.23 | 4.80 | 43.8 (8/10) | - | - | 0.2 |
