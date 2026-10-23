from dataclasses import dataclass
from typing import TypeVar
from . import _native as n
from . import units as u

T = TypeVar("T")


def require(value: object, expected: type[T], field: str) -> T:
    if not isinstance(value, expected):
        raise TypeError(f"{field} expects {expected.__name__}, got {type(value).__name__}")
    return value


class Unset:
    __slots__ = ()

    def __repr__(self) -> str:
        return "<omitted>"


UNSET = Unset()


@dataclass(frozen=True, slots=True)
class Wgs84Location:
    longitude: u.Angle
    latitude: u.Angle
    ellipsoid_height: u.Length = u.m(0)
    height_above_ground: u.Length = u.m(0)

    def _native(self) -> n.GroundStationLocation:
        return n.GroundStationLocation(
            require(self.longitude, u.Angle, "longitude").radians,
            require(self.latitude, u.Angle, "latitude").radians,
            require(self.ellipsoid_height, u.Length, "ellipsoid_height").metres,
            require(self.height_above_ground, u.Length, "height_above_ground").metres,
        )

    def coordinates(self, type: n.GroundPositionType) -> tuple[float, float, float]:
        """Ground-point coordinates in SI units; excludes height above ground."""
        return tuple(n.ground_station_coordinates(self._native(), type))

    @classmethod
    def from_coordinates(cls, type: n.GroundPositionType, coordinates: tuple[float, float, float],
                         *, height_above_ground: u.Length = u.m(0)) -> "Wgs84Location":
        """Create from ground-point coordinates (radians/metres); see GroundPositionType."""
        value = n.ground_station_location(type, coordinates,
            require(height_above_ground, u.Length, "height_above_ground").metres)
        return cls(u.rad(value.longitude_rad), u.rad(value.latitude_rad), u.m(value.height_m),
                   u.m(value.height_above_ground_m))


@dataclass(frozen=True, slots=True)
class OrbitStyle:
    """Track appearance. Width is in logical pixels; color is opaque 0xRRGGBB."""
    line_color: int = 0x73CDEE
    line_width: float = 1.5
    line_style: n.LineStyle = n.LineStyle.SOLID
    track_2d: bool = True
    labels_2d: bool = True
    track_3d: bool = True
    extent: int = 0
    past_minutes: float = 45
    future_minutes: float = 45

    def _native(self) -> n.OrbitDisplay:
        result = n.OrbitDisplay()
        for field in self.__dataclass_fields__:
            setattr(result, field, getattr(self, field))
        return result

    @classmethod
    def _read(cls, value: n.OrbitDisplay) -> "OrbitStyle":
        return cls(**{field: getattr(value, field) for field in cls.__dataclass_fields__})
