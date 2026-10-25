"""Immutable WGS84 surface boundaries, separate from coverage sampling settings."""
from __future__ import annotations
from dataclasses import dataclass
from . import _native as n
from . import units as u
from ._values import require


@dataclass(frozen=True, slots=True)
class Vertex:
    longitude: u.Angle
    latitude: u.Angle

    def _native(self) -> n.RegionVertex:
        return n.RegionVertex(require(self.longitude, u.Angle, "longitude").radians,
                              require(self.latitude, u.Angle, "latitude").radians)


@dataclass(frozen=True, slots=True)
class Rectangle:
    south: u.Angle
    north: u.Angle
    west: u.Angle
    east: u.Angle

    def _native(self) -> n.RegionBoundary:
        value = n.RegionBoundary()
        for name in ("south", "north", "west", "east"):
            setattr(value, name + "_rad", require(getattr(self, name), u.Angle, name).radians)
        n.validate_region_boundary(value)
        return value


@dataclass(frozen=True, slots=True)
class Polygon:
    """3–256 vertices joined by minor great-circle arcs; no holes or repeated closure."""
    vertices: tuple[Vertex, ...]

    def __post_init__(self) -> None:
        object.__setattr__(self, "vertices", tuple(self.vertices))

    def _native(self) -> n.RegionBoundary:
        value = n.RegionBoundary()
        value.vertices = [require(v, Vertex, "vertex")._native() for v in self.vertices]
        if not value.vertices:
            raise ValueError("A polygon requires at least three vertices")
        n.validate_region_boundary(value)
        return value


Boundary = Rectangle | Polygon


def _to_native(value: Boundary) -> n.RegionBoundary:
    if not isinstance(value, (Rectangle, Polygon)):
        raise TypeError("boundary expects regions.Rectangle or regions.Polygon")
    return value._native()


def _from_native(value: n.RegionBoundary) -> Boundary:
    if value.vertices:
        return Polygon(tuple(Vertex(u.rad(v.longitude_rad), u.rad(v.latitude_rad))
                             for v in value.vertices))
    return Rectangle(u.rad(value.south_rad), u.rad(value.north_rad),
                     u.rad(value.west_rad), u.rad(value.east_rad))
