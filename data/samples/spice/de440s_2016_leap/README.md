# Historical SPICE sample

This is an Orbelle-produced, reduced validation kernel, derived from JPL
DE440s using CSPICE N0067 `spksub`. It is not an unchanged NAIF-distributed SPK.
The derived producer, original source and source hash are embedded in its
comment area and recorded in `manifest.json`. Coefficients were copied, not
refitted. The original binary remains outside the repository.

Retained segments: target 3 relative to 0, target 10 relative to 0, targets 301
and 399 relative to 3; SPICE J2000 axes (DE440's ICRF alignment), Type 2.
Binary coverage is 2016-12-27 00:00:00 through 2017-01-05 00:00:00 TDB.
The manifest deliberately allows the narrower, closed interval 2016-12-28
00:00:00 through 2017-01-04 00:00:00 UTC, matching the EOP validation sample.

`naif0012.tls` is the unmodified NAIF LSK. Orbelle uses its uniform time
constants for TAI/TDB only; the application's UTC route remains its separately
versioned IERS time provider.

Source files were retrieved on 2026-10-09. Original downloads:

- https://naif.jpl.nasa.gov/pub/naif/generic_kernels/spk/planets/de440s.bsp
- https://naif.jpl.nasa.gov/pub/naif/generic_kernels/lsk/naif0012.tls

See [NAIF rules](https://naif.jpl.nasa.gov/naif/rules.html) for redistribution
and kernel modification conditions. NASA/JPL's SPICE system and original JPL
ephemeris data are acknowledged. The modified file has a new name and producer;
it must not be described as an original NAIF product or as current-epoch data.

Recreate the BSP with `orbelle_spice_extract` (built by the `astronomy`
preset), passing the two original files and a new output path. The tool checks
the pinned original hashes and refuses to overwrite an existing file.
