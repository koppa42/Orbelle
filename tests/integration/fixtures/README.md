# Current project format fixture

`project-current.orbitforge` is a fixed v12 input assembled independently with Python's `struct.pack` from the [documented binary layout](../../../docs/architecture/project_files.md), without calling OrbitForge's writer. C++ and Python must read it and reproduce its bytes exactly. It guards against reader and writer drifting together.

- 1000 bytes; SHA-256 `e2788f1a13f1c21ad0d4142e34158eed1dc07a210ddff444bdaf23ad8b9f29ae`.
- `Schema fixture`, TAI nanoseconds `[123, 600000000123]`, current time `1000000123`.
- Two-body `satellite-a` named `观测卫星 A`, and J2 `satellite-b` with an empty display name (use its ID). Both use GCRF state `[7000000, 0, 0]` m and `[0, 6200, 5300]` m/s at TAI nanosecond 123.
- Both tracks use color `0xe9a2ba`, dash-dot lines and width 3.25 logical pixels.
- `station-a` named `地面站 A`, WGS84 longitude 1.2 rad, latitude -0.4 rad, height 2500 m, minimum elevation 0.2 rad.
- `sensor-a` named `Camera`, parent `satellite-a`, cone half-angle 0.3 rad, tracking `station-a`.
- One AER report binding `station-a` to `sensor-a`, following the scenario with a 60-second step, SI defaults and TAI timestamps. Its display template orders `time.epoch`, `object.name`, `aer.range` and displays range in km with title `Slant range`.
- Empty data lock; no DATA, TIME or ANLY sections. Input-only files do not claim a prepared analysis.

The development build supports only the current project format. When the schema changes, replace this fixture and its expectations deliberately; do not retain historical-format readers or automatically regenerate the fixture with the production writer. Version rejection tests alter the header without reproducing historical layouts.
