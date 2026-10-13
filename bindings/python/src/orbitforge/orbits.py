"""Immutable orbit inputs. Mean elements remain distinct from osculating ones."""
from dataclasses import dataclass
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

    def _cartesian(self) -> Cartesian:
        elements = n.KeplerianElements(
            self.epoch, require(self.semi_major_axis, u.Length, "semi_major_axis").metres,
            self.eccentricity, require(self.inclination, u.Angle, "inclination").radians,
            require(self.raan, u.Angle, "raan").radians,
            require(self.argument_of_periapsis, u.Angle, "argument_of_periapsis").radians,
            require(self.true_anomaly, u.Angle, "true_anomaly").radians,
        )
        return Cartesian._read(n.to_cartesian(elements, n.central_body().gravitational_parameter_m3_s2))


@dataclass(frozen=True, slots=True)
class Orbit:
    """Canonical configuration returned by orbit constructors and handle reads."""
    initial: Cartesian
    propagator: n.OrbitPropagator = n.OrbitPropagator.TWO_BODY
    input_frame: n.FrameId = n.FrameId.GCRF
    tle: n.Tle = n.Tle("", "")
    step_seconds: int = 60

    def _native(self) -> n.OrbitInput:
        value = n.OrbitInput()
        value.initial = require(self.initial, Cartesian, "initial")._native()
        value.propagator, value.input_frame = self.propagator, self.input_frame
        value.tle, value.step_seconds = self.tle, self.step_seconds
        return value

    @classmethod
    def from_input(cls, value: n.OrbitInput) -> "Orbit":
        """Copy a low-level SI input, without its object ID or analysis bounds."""
        require(value, n.OrbitInput, "orbit")
        return cls(Cartesian._read(value.initial), value.propagator, value.input_frame,
                   n.Tle(value.tle.line1, value.tle.line2), value.step_seconds)


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
