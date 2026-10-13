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
