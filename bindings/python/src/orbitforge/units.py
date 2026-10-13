"""Explicit quantities at the editing API boundary; native calculations use SI."""
from dataclasses import dataclass
from math import pi
from numbers import Integral, Real
from fractions import Fraction


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


@dataclass(frozen=True, slots=True)
class Duration:
    """Exact signed int64 nanoseconds; queries additionally require a positive step."""
    nanoseconds: int

    def __post_init__(self) -> None:
        if isinstance(self.nanoseconds, bool) or not isinstance(self.nanoseconds, Integral):
            raise TypeError("Duration requires integer nanoseconds")
        value = int(self.nanoseconds)
        if not -(1 << 63) <= value < (1 << 63):
            raise ValueError("Duration is outside the signed 64-bit nanosecond range")
        object.__setattr__(self, "nanoseconds", value)


def _duration(value: float, scale: int) -> Duration:
    if isinstance(value, bool) or not isinstance(value, Real):
        raise TypeError("Expected a real duration")
    # Decimal spelling avoids binary-float drift for inputs such as s(0.1).
    # Fraction preserves all integer digits and does not use mutable Decimal context.
    ticks = Fraction(str(value)) * scale
    if ticks.denominator != 1:
        raise ValueError("Duration must resolve to whole nanoseconds")
    return Duration(ticks.numerator)


def s(value: float) -> Duration:
    return _duration(value, 1_000_000_000)


def ms(value: float) -> Duration:
    return _duration(value, 1_000_000)


def ns(value: int) -> Duration:
    return Duration(value)
