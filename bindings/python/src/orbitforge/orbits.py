"""Immutable orbit inputs. Mean elements remain distinct from osculating ones."""
from dataclasses import dataclass
from typing import Literal
from . import _native as n
from . import units as u
from . import missions
from ._values import require


@dataclass(frozen=True, slots=True)
class Cartesian:
    epoch: n.Epoch
    position_m: tuple[float, float, float]
    velocity_mps: tuple[float, float, float]
    origin: n.OriginId = n.OriginId.EARTH
    frame: n.FrameId = n.FrameId.GCRF

    def __post_init__(self) -> None:
        object.__setattr__(self, "position_m", tuple(self.position_m))
        object.__setattr__(self, "velocity_mps", tuple(self.velocity_mps))

    def _native(self) -> n.CartesianState:
        return n.CartesianState(self.epoch, self.origin, self.frame, self.position_m, self.velocity_mps)

    @classmethod
    def _read(cls, value: n.CartesianState) -> "Cartesian":
        x, y, z = value.position_m
        vx, vy, vz = value.velocity_mps
        return cls(value.epoch, (x, y, z), (vx, vy, vz), value.origin, value.frame)


@dataclass(frozen=True, slots=True)
class Keplerian:
    epoch: n.Epoch
    semi_major_axis: u.Length
    eccentricity: float
    inclination: u.Angle
    raan: u.Angle = u.deg(0)
    argument_of_periapsis: u.Angle = u.deg(0)
    true_anomaly: u.Angle = u.deg(0)

    def _cartesian(self, mu: float | None = None) -> Cartesian:
        elements = n.KeplerianElements(
            self.epoch, require(self.semi_major_axis, u.Length, "semi_major_axis").metres,
            self.eccentricity, require(self.inclination, u.Angle, "inclination").radians,
            require(self.raan, u.Angle, "raan").radians,
            require(self.argument_of_periapsis, u.Angle, "argument_of_periapsis").radians,
            require(self.true_anomaly, u.Angle, "true_anomaly").radians,
        )
        return Cartesian._read(n.to_cartesian(
            elements, n.central_body().gravitational_parameter_m3_s2 if mu is None else mu))


THIRD_BODY_NAMES = ("sun", "moon", "jupiter", "venus", "saturn", "mars", "mercury", "uranus", "neptune", "pluto")


@dataclass(frozen=True, slots=True)
class ThirdBodySettings:
    """GM source and retained drafts for one body. mu is SI m^3/s^2.

    cb is a captured value from load_cb_gravity(); all positions use the selected DE440s.
    Mars and outer planets use system barycentres and require SystemGm in Cb files.
    """
    body: str
    source: Literal["jpl_de", "cb_file", "user_specified"] = "jpl_de"
    mu: float | None = None
    cb: n.CbGravity | None = None

    def _native(self) -> n.ThirdBodyGravity:
        if self.body not in THIRD_BODY_NAMES:
            raise ValueError("Unknown third body")
        sources = {"jpl_de": n.ThirdBodyGravitySource.JPL_DE,
                   "cb_file": n.ThirdBodyGravitySource.CB_FILE,
                   "user_specified": n.ThirdBodyGravitySource.USER_SPECIFIED}
        if self.source not in sources:
            raise ValueError("Unknown third-body gravity source")
        value = n.ThirdBodyGravity()
        value.source, value.user_mu, value.cb = sources[self.source], self.mu, self.cb
        return value


def load_cb_gravity(path, body: str, *, mu_scale: float = 1.0) -> n.CbGravity:
    """Capture Gm/SystemGm from body.cb. SI by default; km^3/s^2 uses mu_scale=1e9."""
    if body not in THIRD_BODY_NAMES:
        raise ValueError("Unknown third body")
    return n.load_cb_gravity(path, getattr(n.ThirdBody, body.upper()), mu_scale)


@dataclass(frozen=True, slots=True)
class HpopSettings:
    """Static file or built-in gravity and Cowell integration with local error tolerances.

    Optional solar-system third-body gravity uses an explicitly selected DE440s ephemeris. No drag,
    SRP or tides in this release. Noncentral gravity requires
    explicitly selected EOP covering the entire integration path, including initial epoch.
    gravity_model selects a catalog/DataSnapshot ID (empty: built-in EGM96, up to 20).
    File models support degrees through 360. gravity_mu must match the selected file's
    GM in m^3/s^2 and is also used when converting initial Keplerian elements.
    """
    gravity_degree: int = 8
    gravity_order: int = 8
    integrator: Literal["rkf78", "rk4"] = "rkf78"
    relative_tolerance: float = 1e-12
    position_tolerance_m: float = 1e-3
    velocity_tolerance_mps: float = 1e-6
    min_step_seconds: float = 0.001
    max_step_seconds: float = 60
    fixed_step_seconds: float = 10
    minimum_altitude_m: float = 0
    gravity_model: str = ""
    gravity_mu: float = 3.986004415e14
    third_bodies: tuple[str, ...] = ()
    ephemeris: str = "de440s"
    body_gravity: tuple[ThirdBodySettings, ...] = ()

    def __post_init__(self) -> None:
        bodies = tuple(self.third_bodies)
        if any(body not in THIRD_BODY_NAMES for body in bodies) or len(set(bodies)) != len(bodies):
            raise ValueError("third_bodies accepts solar-system body names, each at most once")
        settings = tuple(self.body_gravity)
        if any(not isinstance(v, ThirdBodySettings) for v in settings):
            raise TypeError("body_gravity requires ThirdBodySettings values")
        if len({v.body for v in settings}) != len(settings):
            raise ValueError("Duplicate third-body gravity settings")
        for item in settings:
            item._native()
        object.__setattr__(self, "third_bodies", tuple(b for b in THIRD_BODY_NAMES if b in bodies))
        active = (v for v in settings if v.source != "jpl_de" or v.mu is not None or v.cb is not None)
        object.__setattr__(self, "body_gravity", tuple(sorted(active, key=lambda v: THIRD_BODY_NAMES.index(v.body))))

    def _native(self) -> n.HpopOptions:
        value = n.HpopOptions()
        if self.integrator not in ("rkf78", "rk4"):
            raise ValueError("integrator must be 'rkf78' or 'rk4'")
        for name in self.__dataclass_fields__:
            if name not in ("integrator", "third_bodies", "ephemeris", "body_gravity"):
                setattr(value, name, getattr(self, name))
        rows = list(value.third_bodies)
        for item in self.body_gravity:
            rows[THIRD_BODY_NAMES.index(item.body)] = item._native()
        for name, row in zip(THIRD_BODY_NAMES, rows):
            row.enabled = name in self.third_bodies
        value.third_bodies = rows
        value.ephemeris_model = self.ephemeris
        value.integrator = n.HpopIntegrator.RKF78 if self.integrator == "rkf78" else n.HpopIntegrator.RK4
        value.validate()
        return value

    @classmethod
    def _read(cls, value: n.HpopOptions) -> "HpopSettings":
        fields = {name: getattr(value, name) for name in cls.__dataclass_fields__
                  if name not in ("integrator", "third_bodies", "ephemeris", "body_gravity")}
        rows = value.third_bodies
        fields["third_bodies"] = tuple(b for b, row in zip(THIRD_BODY_NAMES, rows) if row.enabled)
        sources = {n.ThirdBodyGravitySource.JPL_DE: "jpl_de",
                   n.ThirdBodyGravitySource.CB_FILE: "cb_file",
                   n.ThirdBodyGravitySource.USER_SPECIFIED: "user_specified"}
        fields["body_gravity"] = tuple(ThirdBodySettings(b, sources[row.source], row.user_mu, row.cb)
                                      for b, row in zip(THIRD_BODY_NAMES, rows)
                                      if row.source != n.ThirdBodyGravitySource.JPL_DE or
                                      row.user_mu is not None or row.cb is not None)
        fields["ephemeris"] = value.ephemeris_model
        return cls(**fields, integrator="rkf78" if value.integrator == n.HpopIntegrator.RKF78 else "rk4")


@dataclass(frozen=True, slots=True)
class Orbit:
    """Canonical configuration returned by orbit constructors and handle reads."""
    initial: Cartesian
    propagator: n.OrbitPropagator = n.OrbitPropagator.TWO_BODY
    input_frame: n.FrameId = n.FrameId.GCRF
    tle: n.Tle = n.Tle("", "")
    step_seconds: int = 60
    hpop: HpopSettings = HpopSettings()
    mission: missions.Plan = missions.Plan()

    def _native(self) -> n.OrbitInput:
        value = n.OrbitInput()
        value.initial = require(self.initial, Cartesian, "initial")._native()
        value.propagator, value.input_frame = self.propagator, self.input_frame
        value.tle, value.step_seconds = self.tle, self.step_seconds
        if self.propagator == n.OrbitPropagator.MISSION:
            value.mission = require(self.mission, missions.Plan, "mission")._native()
            value.mission.end_epoch(self.initial.epoch)
        if self.propagator == n.OrbitPropagator.HPOP or (
            self.propagator == n.OrbitPropagator.MISSION and self.mission.propagation == "hpop"
        ):
            value.hpop = require(self.hpop, HpopSettings, "hpop")._native()
        return value

    @classmethod
    def from_input(cls, value: n.OrbitInput) -> "Orbit":
        """Copy a low-level SI input, without its object ID or analysis bounds."""
        require(value, n.OrbitInput, "orbit")
        return cls(Cartesian._read(value.initial), value.propagator, value.input_frame,
                   n.Tle(value.tle.line1, value.tle.line2), value.step_seconds,
                   HpopSettings._read(value.hpop), missions.Plan._read(value.mission))


def TwoBody(*, initial: Keplerian | Cartesian | n.CartesianState) -> Orbit:
    if isinstance(initial, Keplerian):
        initial = initial._cartesian()
    elif isinstance(initial, n.CartesianState):
        initial = Cartesian._read(initial)
    return Orbit(require(initial, Cartesian, "initial"))


def Mission(*, initial: Keplerian | Cartesian | n.CartesianState,
            segments: tuple[missions.Coast | missions.Impulse, ...] | list[missions.Coast | missions.Impulse],
            propagation: Literal["two_body", "hpop"] = "two_body",
            hpop: HpopSettings = HpopSettings()) -> Orbit:
    """Forward coast/impulse sequence covering the whole scenario; exact burns are post-burn.

    Example: segments=[Coast(u.s(600)), Impulse((100, 0, 0)), Coast(u.s(6600))].
    Impulses use SI m/s, default VNC axes. No finite thrust or automatic targeting.
    """
    plan = missions.Plan(tuple(segments), propagation)
    if isinstance(initial, Keplerian):
        initial = initial._cartesian(mu=hpop.gravity_mu if propagation == "hpop" else None)
    elif isinstance(initial, n.CartesianState):
        initial = Cartesian._read(initial)
    orbit = Orbit(require(initial, Cartesian, "initial"), n.OrbitPropagator.MISSION,
                  hpop=hpop, mission=plan)
    orbit._native()
    return orbit


def J2(*, initial: n.J2MeanElements) -> Orbit:
    return Orbit(Cartesian._read(n.j2_mean_ellipse_state(require(initial, n.J2MeanElements, "initial"))),
                 n.OrbitPropagator.J2_SECULAR)


def J4(*, initial: n.J4MeanElements) -> Orbit:
    return Orbit(Cartesian._read(n.j4_mean_ellipse_state(require(initial, n.J4MeanElements, "initial"))),
                 n.OrbitPropagator.J4_SECULAR)


def Sgp4(*, tle: n.Tle) -> Orbit:
    value = n.OrbitInput()
    value.propagator, value.tle = n.OrbitPropagator.SGP4, require(tle, n.Tle, "tle")
    return Orbit.from_input(value)


def Hpop(*, initial: Keplerian | Cartesian | n.CartesianState,
         gravity_model: str = "", gravity_mu: float = 3.986004415e14,
         gravity_degree: int = 8, gravity_order: int | None = None,
         integrator: Literal["rkf78", "rk4"] = "rkf78", max_step_seconds: float = 60,
         min_step_seconds: float = 0.001, fixed_step_seconds: float = 10,
         relative_tolerance: float = 1e-12, position_tolerance_m: float = 1e-3,
         velocity_tolerance_mps: float = 1e-6, minimum_altitude_m: float = 0,
         third_bodies: tuple[str, ...] = (), ephemeris: str = "de440s",
         sun_mu: float | None = None, moon_mu: float | None = None,
         body_gravity: tuple[ThirdBodySettings, ...] = ()) -> Orbit:
    """Cowell orbit with static gravity, solar-system third bodies, RKF78 or RK4.

    Built-in EGM96 supports up to degree 20; installed gravity files up to 360.
    third_bodies=("sun", "moon") requires explicitly supplied DE440s ephemerides.
    sun_mu and moon_mu are optional overrides in m^3/s^2; None uses DE440 constants.

    Output sampling is set on analysis.states(), independently of integration steps.
    Omitted gravity_order uses gravity_degree; order=0 retains only zonal terms.
    """
    if isinstance(initial, Keplerian):
        initial = initial._cartesian(mu=gravity_mu)
    elif isinstance(initial, n.CartesianState):
        initial = Cartesian._read(initial)
    overrides = tuple(ThirdBodySettings(body, "user_specified", mu)
                      for body, mu in (("sun", sun_mu), ("moon", moon_mu)) if mu is not None)
    settings = HpopSettings(
        gravity_degree, gravity_degree if gravity_order is None else gravity_order, integrator,
        relative_tolerance, position_tolerance_m, velocity_tolerance_mps,
        min_step_seconds, max_step_seconds, fixed_step_seconds, minimum_altitude_m,
        gravity_model.lower(), gravity_mu, third_bodies, ephemeris, tuple(body_gravity) + overrides)
    settings._native()  # Fail at construction, before changing a project.
    return Orbit(require(initial, Cartesian, "initial"), n.OrbitPropagator.HPOP, hpop=settings)
