# Current project format fixture

`project-current.orbelle` is a fixed v48 input assembled independently with Python's `struct.pack` from the [documented binary layout](../../../docs/architecture/project_files.md), without calling Orbelle's writer. C++ and Python must read it and reproduce its bytes exactly. It guards against reader and writer drifting together.

- 3041 bytes; SHA-256 `4484d2143792e53cea41947cbada9f71bf0357c13e6a4281e2b7343c8e5bff9d`.
- `Schema fixture`, TAI nanoseconds `[123, 600000000123]`, current time `1000000123`.
- Two-body `satellite-a` named `观测卫星 A`, and mission sequence `satellite-b` with an empty display name (use its ID). Both use GCRF state `[7000000, 0, 0]` m and `[0, 6200, 5300]` m/s at TAI nanosecond 123.
- Mission uses HPOP point-mass coasts: `Drift` (200000000001 ns), `Raise` (VNC delta-v `[12, -3, 0.5]` m/s), `Coast` (399999999999 ns). The sequence covers the scenario exactly.
- The VOP integration boolean is independently packed as `<B` immediately after the integrator, with value 1. It remains inactive with Gauss–Jackson, is preserved on save, and does not affect calculation identity. The v48 / OBJS32 payload and both CRC layers are rebuilt without calling the production writer.
- HPOP stores an empty gravity selection (built-in EGM96), GM `3.986004415e14 m³/s²`, and uses Gauss–Jackson 12 with Pseudo Correction and explicit Fixed Step control, inactive relative tolerance `2e-12`, position tolerance `0.002 m`, velocity tolerance `2e-6 m/s`, minimum/maximum steps `0.005/45 s`, fixed step `12 s`, minimum geocentric altitude `1000 m`.
- Hermite interpolation uses polynomial order 5, inactive VOP GM `4.1e14 m³/s²` and fixed ephemeris reporting. The GM is independently packed as `<d` after method/order; OBJS32 and both CRC layers are updated without the production writer. The v48 / OBJS32 regularization fields follow these fields: independent `struct.pack("<BdI", 1, 1.25, 240)`, with both CRC layers recomputed without calling the production writer. Time regularization is enabled with exponent 1.25 and 240 steps per orbit; the ordinary 12-second step remains an inactive draft.
- Relativistic accelerations are disabled. Satellite mass is `1250 kg`; both inactive spherical areas are `37.5 m²` from their explicit ratios.
- Solid tide draft is Full with truncation and time-dependent terms on; it is inactive for 0/0 gravity.
- Secular variations are enabled as an inactive draft for the 0/0 base gravity; no reference epoch/rates are inferred.
- Ocean tides are disabled; the draft retains model `fixture-ocean` and independent 30×12 truncation.
- SRP boundary mitigation is true as an inactive draft; the v48 / OBJS32 boolean is independently packed between Moon shadow and Cr, with both CRC layers recomputed.
- SRP is disabled; its draft preserves Cr `1.25`, area/mass `0.03 m²/kg`, dual cone, spacecraft apparent Sun, Earth shadow on / Moon shadow off, atmospheric height `23000 m`, pressure `4.56e-6 Pa` at `149597870700 m`, Sun/Earth/Moon radii `696000000/6378137/1737400 m`.
- SRP adds two inactive physical-centre occultors: Mercury radius `2440000.125 m` and Venus radius `6051800 m`, independently packed as `<IIdId` after the existing SRP constants; both CRC layers were recomputed.
- Drag is disabled; its draft preserves Cd `2.3`, area/mass `0.03 m²/kg`, NRLMSISE-00, inactive low-altitude NRLMSISE selection, blend `42000 m`, F10.7 `170 sfu`, average `150 sfu`, Kp `4.3`, apparent Sun on, file selection `fixture-weather`, three-hour natural cubic spline updates, Kp source and approximate altitude on.
- All ten third bodies are disabled, with JPL DE sources; the saved ephemeris selection is `de440s`, with no GM overrides.
- Both tracks use color `0xe9a2ba`, dash-dot lines and width 3.25 logical pixels.
- `station-a` named `地面站 A`, WGS84 longitude 1.2 rad, latitude -0.4 rad, height 2500 m, minimum elevation 0.2 rad.
- `sensor-a` named `Camera`, parent `satellite-a`, cone half-angle 0.3 rad, tracking `station-a`.
- SNSR2 independently packs the pattern type, six angle drafts, zero custom vertices, zero roll, both view flags and fill enabled, color `0xf1a6ca`, opacity `.16` and a 20000 km space extension. The v48 header and both CRC layers were rebuilt with struct/zlib, without the production writer.
- One AER report binding `station-a` to `sensor-a`, following the scenario with a 60-second step, SI defaults and TAI timestamps. Its display template orders `time.epoch`, `object.name`, `aer.range` and displays range in km with title `Slant range`.
- Empty data lock; no DATA, TIME or ANLY sections. Input-only files do not claim a prepared analysis.

The development build supports only the current project format. When the schema changes, replace this fixture and its expectations deliberately; do not retain historical-format readers or automatically regenerate the fixture with the production writer. Version rejection tests alter the header without reproducing historical layouts.

The final HPOP record has 438 independently encoded covariance bytes: disabled,
LVLH/rotating, diagonal `(25,25,25,.25,.25,.25)`, independent 12x7, tolerance 2e-14,
consider analysis disabled, D/K Use true with variances 1e-4/2e-4, D/state cross
`(.01,.02,.03,0,0,0)`, K/state cross `(-.01,0,0,.001,0,0)`, cross switch true,
Cov(D,K)=5e-5. Dormant values must survive exactly.

RPTS7 independently packs `<IQ` values `(1, 2000000)` immediately after the report step:
extremum-assisted mode and a non-default 2 ms tolerance, retained even on the AER template.
Both the RPTS section CRC and the payload CRC are recalculated independently.

RPTS7 appends UTF-8 `fixture-sun` with a little-endian u32 byte length after `<IQ>`. The v48 header, section CRC and payload CRC are independently rebuilt with Python struct/zlib, without the production writer.

RPTS7 adds an independently packed second report: `orbital-events`, kind 12, satellite-a, custom [123,600000000123] ns, 30 s check interval, extremum-assisted search, 1 us tolerance, GCRF/engineering/TAI, and all columns. No production writer is used.

RPTS7 adds two independently packed reports, kinds 13 and 14, ordered IDs `[satellite-a, satellite-b]`, the same custom nanosecond interval and search settings as the orbital-event fixture, and complete columns. The second satellite's mission is retained in this persistence-only fixture; attempting an intersatellite calculation with a mission sequence is rejected.

RPTS7 appends six independently packed little-endian doubles and one strict boolean after the solar ephemeris string. Existing records retain default coverage drafts; kinds 15/16 bind satellite-a with bounds (-0.2, 0.3, 2.9, -2.9), spacing 0.1 rad, minimum elevation 0.2 rad and end gaps disabled. Header v48 and both CRC layers are rebuilt with struct/zlib.

The v48 branding update independently rebuilds the empty LOCK canonical bytes with
`orbelle-data-lock-v1`, its SHA-256, section length/CRC and file length/CRC using
Python struct/hashlib/zlib. All non-LOCK section bytes are unchanged.
