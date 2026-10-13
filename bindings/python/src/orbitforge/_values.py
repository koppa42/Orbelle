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

    def _native(self) -> n.GroundStationLocation:
        return n.GroundStationLocation(
            require(self.longitude, u.Angle, "longitude").radians,
            require(self.latitude, u.Angle, "latitude").radians,
            require(self.ellipsoid_height, u.Length, "ellipsoid_height").metres,
        )


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
