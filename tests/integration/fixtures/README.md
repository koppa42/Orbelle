# Current project format fixture

`project-current.orbitforge` is a fixed v28 input assembled independently with Python's `struct.pack` from the [documented binary layout](../../../docs/architecture/project_files.md), without calling OrbitForge's writer. C++ and Python must read it and reproduce its bytes exactly. It guards against reader and writer drifting together.

- 1454 bytes; SHA-256 `b3dfca5f67b642f4030678a3e84f9642a8bd7871980860ca2228fa374c428ea5`.
- `Schema fixture`, TAI nanoseconds `[123, 600000000123]`, current time `1000000123`.
- Two-body `satellite-a` named `观测卫星 A`, and mission sequence `satellite-b` with an empty display name (use its ID). Both use GCRF state `[7000000, 0, 0]` m and `[0, 6200, 5300]` m/s at TAI nanosecond 123.
- Mission uses HPOP point-mass coasts: `Drift` (200000000001 ns), `Raise` (VNC delta-v `[12, -3, 0.5]` m/s), `Coast` (399999999999 ns). The sequence covers the scenario exactly.
- HPOP stores an empty gravity selection (built-in EGM96), GM `3.986004415e14 m³/s²`, and uses RKF78, relative tolerance `2e-12`, position tolerance `0.002 m`, velocity tolerance `2e-6 m/s`, minimum/maximum steps `0.005/45 s`, inactive RK4 step `12 s`, minimum geocentric altitude `1000 m`.
- Relativistic accelerations are disabled. Satellite mass is `1250 kg`; both inactive spherical areas are `37.5 m²` from their explicit ratios.
- Solid tide draft is Full with truncation and time-dependent terms on; it is inactive for 0/0 gravity.
- Secular variations are enabled as an inactive draft for the 0/0 base gravity; no reference epoch/rates are inferred.
- Ocean tides are disabled; the draft retains model `fixture-ocean` and independent 30×12 truncation.
- SRP is disabled; its draft preserves Cr `1.25`, area/mass `0.03 m²/kg`, dual cone, spacecraft apparent Sun, Earth shadow on / Moon shadow off, atmospheric height `23000 m`, pressure `4.56e-6 Pa` at `149597870700 m`, Sun/Earth/Moon radii `696000000/6378137/1737400 m`.
- Drag is disabled; its draft preserves Cd `2.3`, area/mass `0.03 m²/kg`, NRLMSISE-00, inactive low-altitude NRLMSISE selection, blend `42000 m`, F10.7 `170 sfu`, average `150 sfu`, Kp `4.3`, apparent Sun on, file selection `fixture-weather`, three-hour updates, Kp source and approximate altitude on.
- All ten third bodies are disabled, with JPL DE sources; the saved ephemeris selection is `de440s`, with no GM overrides.
- Both tracks use color `0xe9a2ba`, dash-dot lines and width 3.25 logical pixels.
- `station-a` named `地面站 A`, WGS84 longitude 1.2 rad, latitude -0.4 rad, height 2500 m, minimum elevation 0.2 rad.
- `sensor-a` named `Camera`, parent `satellite-a`, cone half-angle 0.3 rad, tracking `station-a`.
- One AER report binding `station-a` to `sensor-a`, following the scenario with a 60-second step, SI defaults and TAI timestamps. Its display template orders `time.epoch`, `object.name`, `aer.range` and displays range in km with title `Slant range`.
- Empty data lock; no DATA, TIME or ANLY sections. Input-only files do not claim a prepared analysis.

The development build supports only the current project format. When the schema changes, replace this fixture and its expectations deliberately; do not retain historical-format readers or automatically regenerate the fixture with the production writer. Version rejection tests alter the header without reproducing historical layouts.
