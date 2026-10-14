"""Immutable orbit inputs. Mean elements remain distinct from osculating ones."""
from dataclasses import dataclass
from typing import Literal
from . import _native as n
from . import units as u
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


@dataclass(frozen=True, slots=True)
class HpopSettings:
    """Static EGM96 gravity and Cowell integration; tolerances are local numerical errors.

    No drag, third bodies, SRP or tides in this release. Noncentral gravity requires
    explicitly selected EOP covering the entire integration path, including initial epoch.
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

    def _native(self) -> n.HpopOptions:
        value = n.HpopOptions()
        if self.integrator not in ("rkf78", "rk4"):
            raise ValueError("integrator must be 'rkf78' or 'rk4'")
        for name in self.__dataclass_fields__:
            if name != "integrator":
                setattr(value, name, getattr(self, name))
        value.integrator = n.HpopIntegrator.RKF78 if self.integrator == "rkf78" else n.HpopIntegrator.RK4
        value.validate()
        return value

    @classmethod
    def _read(cls, value: n.HpopOptions) -> "HpopSettings":
        fields = {name: getattr(value, name) for name in cls.__dataclass_fields__ if name != "integrator"}
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

    def _native(self) -> n.OrbitInput:
        value = n.OrbitInput()
        value.initial = require(self.initial, Cartesian, "initial")._native()
        value.propagator, value.input_frame = self.propagator, self.input_frame
        value.tle, value.step_seconds = self.tle, self.step_seconds
        if self.propagator == n.OrbitPropagator.HPOP:
            value.hpop = require(self.hpop, HpopSettings, "hpop")._native()
        return value

    @classmethod
    def from_input(cls, value: n.OrbitInput) -> "Orbit":
        """Copy a low-level SI input, without its object ID or analysis bounds."""
        require(value, n.OrbitInput, "orbit")
        return cls(Cartesian._read(value.initial), value.propagator, value.input_frame,
                   n.Tle(value.tle.line1, value.tle.line2), value.step_seconds,
                   HpopSettings._read(value.hpop))


def TwoBody(*, initial: Keplerian | Cartesian | n.CartesianState) -> Orbit:
    if isinstance(initial, Keplerian):
        initial = initial._cartesian()
    elif isinstance(initial, n.CartesianState):
        initial = Cartesian._read(initial)
    return Orbit(require(initial, Cartesian, "initial"))


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
         gravity_degree: int = 8, gravity_order: int | None = None,
         integrator: Literal["rkf78", "rk4"] = "rkf78", max_step_seconds: float = 60,
         min_step_seconds: float = 0.001, fixed_step_seconds: float = 10,
         relative_tolerance: float = 1e-12, position_tolerance_m: float = 1e-3,
         velocity_tolerance_mps: float = 1e-6, minimum_altitude_m: float = 0) -> Orbit:
    """Numerical orbit with EGM96 (0/0 point mass or degree 2..20), RKF78 or RK4.

    Output sampling is set on analysis.states(), independently of integration steps.
    Omitted gravity_order uses gravity_degree; order=0 retains only zonal terms.
    """
    if isinstance(initial, Keplerian):
        initial = initial._cartesian(mu=3.986004415e14)
    elif isinstance(initial, n.CartesianState):
        initial = Cartesian._read(initial)
    settings = HpopSettings(
        gravity_degree, gravity_degree if gravity_order is None else gravity_order, integrator,
        relative_tolerance, position_tolerance_m, velocity_tolerance_mps,
        min_step_seconds, max_step_seconds, fixed_step_seconds, minimum_altitude_m)
    settings._native()  # Fail at construction, before changing a project.
    return Orbit(require(initial, Cartesian, "initial"), n.OrbitPropagator.HPOP, hpop=settings)
