"""Ordered coast/impulse segments. Durations use TAI; delta-v uses metres/second."""
from dataclasses import dataclass
from math import isfinite, hypot
from typing import Literal
from . import _native as n
from . import units as u
from ._values import require


@dataclass(frozen=True, slots=True)
class Coast:
    duration: u.Duration
    name: str = ""

    def __post_init__(self) -> None:
        if require(self.duration, u.Duration, "duration").nanoseconds <= 0:
            raise ValueError("Coast duration must be positive")
        require(self.name, str, "name")

    def _native(self) -> n.MissionSegment:
        value = n.MissionSegment()
        value.kind, value.name = n.MissionSegmentKind.COAST, self.name
        value.duration_nanoseconds = self.duration.nanoseconds
        return value


@dataclass(frozen=True, slots=True)
class Impulse:
    """Instantaneous delta-v. VNC: velocity/normal/co-normal; LVLH: radial/transverse/normal."""
    delta_v_mps: tuple[float, float, float]
    frame: Literal["gcrf", "vnc", "lvlh"] = "vnc"
    name: str = ""

    def __post_init__(self) -> None:
        values = tuple(u._number(component) for component in self.delta_v_mps)
        if len(values) != 3 or not isfinite(hypot(*values)):
            raise ValueError("delta_v_mps requires three finite components and magnitude")
        if self.frame not in ("gcrf", "vnc", "lvlh"):
            raise ValueError("frame must be 'gcrf', 'vnc' or 'lvlh'")
        require(self.name, str, "name")
        object.__setattr__(self, "delta_v_mps", values)

    def _native(self) -> n.MissionSegment:
        value = n.MissionSegment()
        value.kind, value.name = n.MissionSegmentKind.IMPULSE, self.name
        value.delta_v_mps = self.delta_v_mps
        value.frame = {"gcrf": n.ManeuverFrame.GCRF, "vnc": n.ManeuverFrame.VNC,
                       "lvlh": n.ManeuverFrame.LVLH}[self.frame]
        return value


@dataclass(frozen=True, slots=True)
class Plan:
    segments: tuple[Coast | Impulse, ...] = ()
    propagation: Literal["two_body", "hpop"] = "two_body"

    def __post_init__(self) -> None:
        object.__setattr__(self, "segments", tuple(self.segments))
        if self.propagation not in ("two_body", "hpop"):
            raise ValueError("propagation must be 'two_body' or 'hpop'")
        if len(self.segments) > 256:
            raise ValueError("A mission supports at most 256 segments")
        for segment in self.segments:
            if not isinstance(segment, (Coast, Impulse)):
                raise TypeError("Each mission segment must be a Coast or Impulse")

    def _native(self) -> n.MissionPlan:
        value = n.MissionPlan()
        value.coast_model = (n.MissionCoastModel.TWO_BODY if self.propagation == "two_body"
                             else n.MissionCoastModel.HPOP)
        value.segments = [segment._native() for segment in self.segments]
        return value

    @classmethod
    def _read(cls, value: n.MissionPlan) -> "Plan":
        frames = {n.ManeuverFrame.GCRF: "gcrf", n.ManeuverFrame.VNC: "vnc", n.ManeuverFrame.LVLH: "lvlh"}
        return cls(tuple(
            Coast(u.ns(s.duration_nanoseconds), s.name) if s.kind == n.MissionSegmentKind.COAST
            else Impulse(tuple(s.delta_v_mps), frames[s.frame], s.name) for s in value.segments),
            "two_body" if value.coast_model == n.MissionCoastModel.TWO_BODY else "hpop")
