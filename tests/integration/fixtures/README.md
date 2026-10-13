# Current project format fixture

`project-current.orbitforge` is a fixed v13 input assembled independently with Python's `struct.pack` from the [documented binary layout](../../../docs/architecture/project_files.md), without calling OrbitForge's writer. C++ and Python must read it and reproduce its bytes exactly. It guards against reader and writer drifting together.

- 1068 bytes; SHA-256 `627d5191259514d569ef0e5b955d9f261590dccfb4e967b842ff35906dba0ccd`.
- `Schema fixture`, TAI nanoseconds `[123, 600000000123]`, current time `1000000123`.
- Two-body `satellite-a` named `观测卫星 A`, and HPOP point-mass `satellite-b` with an empty display name (use its ID). Both use GCRF state `[7000000, 0, 0]` m and `[0, 6200, 5300]` m/s at TAI nanosecond 123.
- HPOP uses RKF78, relative tolerance `2e-12`, position tolerance `0.002 m`, velocity tolerance `2e-6 m/s`, minimum/maximum steps `0.005/45 s`, inactive RK4 step `12 s`, minimum geocentric altitude `1000 m`.
- Both tracks use color `0xe9a2ba`, dash-dot lines and width 3.25 logical pixels.
- `station-a` named `地面站 A`, WGS84 longitude 1.2 rad, latitude -0.4 rad, height 2500 m, minimum elevation 0.2 rad.
- `sensor-a` named `Camera`, parent `satellite-a`, cone half-angle 0.3 rad, tracking `station-a`.
- One AER report binding `station-a` to `sensor-a`, following the scenario with a 60-second step, SI defaults and TAI timestamps. Its display template orders `time.epoch`, `object.name`, `aer.range` and displays range in km with title `Slant range`.
- Empty data lock; no DATA, TIME or ANLY sections. Input-only files do not claim a prepared analysis.

The development build supports only the current project format. When the schema changes, replace this fixture and its expectations deliberately; do not retain historical-format readers or automatically regenerate the fixture with the production writer. Version rejection tests alter the header without reproducing historical layouts.
