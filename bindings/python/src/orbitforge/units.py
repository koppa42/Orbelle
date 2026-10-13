"""Explicit quantities at the editing API boundary; native calculations use SI."""
from dataclasses import dataclass
from math import pi
from numbers import Real


def _number(value: float) -> float:
    if isinstance(value, bool) or not isinstance(value, Real):
        raise TypeError(f"Expected a real number, got {type(value).__name__}")
    return float(value)


@dataclass(frozen=True, slots=True)
class Angle:
    radians: float

    def __post_init__(self) -> None:
        object.__setattr__(self, "radians", _number(self.radians))


@dataclass(frozen=True, slots=True)
class Length:
    metres: float

    def __post_init__(self) -> None:
        object.__setattr__(self, "metres", _number(self.metres))


def deg(value: float) -> Angle:
    return Angle(_number(value) * pi / 180)


def rad(value: float) -> Angle:
    return Angle(value)


def m(value: float) -> Length:
    return Length(value)


def km(value: float) -> Length:
    return Length(_number(value) * 1000)
