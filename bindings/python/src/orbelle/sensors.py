"""Sensor geometry and ideal pointing; no implicit attitude or slew model."""
from __future__ import annotations
from dataclasses import dataclass, field
from typing import TYPE_CHECKING
from . import _native as n
from . import units as u
from ._values import require
if TYPE_CHECKING:
    from ._project import Project
    from .objects import GroundStationHandle


@dataclass(frozen=True, slots=True)
class Cone:
    half_angle: u.Angle


@dataclass(frozen=True, slots=True)
class ComplexConic:
    inner_half_angle: u.Angle
    outer_half_angle: u.Angle
    minimum_clock: u.Angle = u.deg(0)
    maximum_clock: u.Angle = u.deg(360)


@dataclass(frozen=True, slots=True)
class Rectangular:
    horizontal_half_angle: u.Angle
    vertical_half_angle: u.Angle


@dataclass(frozen=True, slots=True)
class Custom:
    """Ordered (cone angle, clock angle) vertices; closure is implicit."""
    points: tuple[tuple[u.Angle, u.Angle], ...]

    @staticmethod
    def from_pattern(text: str) -> Custom:
        return _from_native(n.parse_sensor_pattern(text))


FieldOfView = Cone | ComplexConic | Rectangular | Custom


def _to_native(value: FieldOfView) -> n.SensorPattern:
    p = n.SensorPattern()
    def angle(v: u.Angle) -> float:
        return require(v, u.Angle, "sensor angle").radians
    if isinstance(value, Cone):
        p.half_angle_rad = angle(value.half_angle)
    elif isinstance(value, ComplexConic):
        p.type = n.SensorPatternType.COMPLEX_CONIC
        p.half_angle_rad = angle(value.outer_half_angle)
        p.inner_half_angle_rad = angle(value.inner_half_angle)
        p.minimum_clock_rad = angle(value.minimum_clock)
        p.maximum_clock_rad = angle(value.maximum_clock)
    elif isinstance(value, Rectangular):
        p.type = n.SensorPatternType.RECTANGULAR
        p.horizontal_half_angle_rad = angle(value.horizontal_half_angle)
        p.vertical_half_angle_rad = angle(value.vertical_half_angle)
    elif isinstance(value, Custom):
        p.type = n.SensorPatternType.CUSTOM
        p.custom_points = [n.SensorPatternPoint(angle(cone), angle(clock)) for cone, clock in value.points]
    else:
        raise TypeError("field_of_view must be Cone, ComplexConic, Rectangular or Custom")
    return p


def _from_native(p: n.SensorPattern) -> FieldOfView:
    if p.type == n.SensorPatternType.SIMPLE_CONIC:
        return Cone(u.rad(p.half_angle_rad))
    if p.type == n.SensorPatternType.COMPLEX_CONIC:
        return ComplexConic(u.rad(p.inner_half_angle_rad), u.rad(p.half_angle_rad),
                            u.rad(p.minimum_clock_rad), u.rad(p.maximum_clock_rad))
    if p.type == n.SensorPatternType.RECTANGULAR:
        return Rectangular(u.rad(p.horizontal_half_angle_rad), u.rad(p.vertical_half_angle_rad))
    return Custom(tuple((u.rad(v.cone_rad), u.rad(v.clock_rad)) for v in p.custom_points))


@dataclass(frozen=True, slots=True)
class Pointing:
    target_id: str | None = None
    _project: Project | None = field(default=None, repr=False)

    @staticmethod
    def nadir() -> Pointing:
        return Pointing()

    @staticmethod
    def track(station: GroundStationHandle) -> Pointing:
        from .objects import GroundStationHandle
        require(station, GroundStationHandle, "target")
        return Pointing(station.id, station._project)

    def _native(self) -> n.PointingDefinition:
        if self.target_id is None:
            return n.PointingDefinition()
        if self._project is None:
            raise TypeError("Use Pointing.track(station) to select a target")
        return n.PointingDefinition(self._project._manager().get(self.target_id))
