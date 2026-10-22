"""Explicit leap-second conversion and an upstream synthetic Earth-orientation fixture."""
import orbelle as of

time = of.make_builtin_time_provider()
leap = time.from_utc(of.UtcDateTime(2016, 12, 31, 23, 59, 60, 123456789))
utc = time.to_utc(leap)
print("Time data:", time.data_info.id)
print("TAI nanoseconds:", leap.tai_nanoseconds)
print(f"UTC: {utc.year:04}-{utc.month:02}-{utc.day:02}T"
      f"{utc.hour:02}:{utc.minute:02}:{utc.second:02}.{utc.nanosecond:09}Z")

# ERFA t_c2t06a uses TT = UT1 = MJD 53736. This is a test fixture, not measured EOP.
epoch = of.Epoch((2192 * 86400 - 43200) * 1_000_000_000 - 32_184_000_000)
eop = of.EarthOrientationSample(epoch, 32.184, 2.55060238e-7, 1.860359247e-6, 0, 0,
                                "ERFA-2.0.1-t_c2t06a-synthetic")
rotation = of.make_frame_provider().gcrf_to_itrf(epoch, eop)
print("Frame model:", rotation.model)
print("ITRF position (m):", rotation.apply_position([7_000_000.0, 0.0, 0.0]))
