"""Object definitions, live handles and the project's sole creation entry point."""
from __future__ import annotations
from dataclasses import dataclass
from typing import Iterator, TYPE_CHECKING, TypeVar, overload, cast
from . import _native as n
from . import units as u
from ._values import OrbitStyle, UNSET, Unset, Wgs84Location, require
from .orbits import Orbit
from .sensors import FieldOfView, Pointing, _to_native, _from_native
if TYPE_CHECKING:
    from ._project import Project


@dataclass(frozen=True, slots=True)
class Satellite:
    name: str
    orbit: Orbit
    display: OrbitStyle = OrbitStyle()


@dataclass(frozen=True, slots=True)
class GroundStation:
    name: str
    location: Wgs84Location
    minimum_elevation: u.Angle = u.deg(0)
    position_type: n.GroundPositionType = n.GroundPositionType.GEODETIC
    description: str = ""


@dataclass(frozen=True, slots=True)
class Sensor:
    name: str
    parent: SatelliteHandle
    field_of_view: FieldOfView
    pointing: Pointing = Pointing()


@dataclass(frozen=True, slots=True)
class SatelliteState:
    id: str
    name: str
    orbit: Orbit
    display: OrbitStyle
    model_kind: int
    model_diameter_m: float
    model_path: str


@dataclass(frozen=True, slots=True)
class GroundStationState:
    id: str
    name: str
    location: Wgs84Location
    minimum_elevation: u.Angle
    position_type: n.GroundPositionType
    description: str


@dataclass(frozen=True, slots=True)
class SensorState:
    id: str
    name: str
    parent_id: str
    field_of_view: FieldOfView
    pointing: Pointing


class ObjectHandle:
    """A project-scoped identity. Reads follow this context's active edit."""
    __slots__ = ("_project", "_ref")

    def __init__(self, project: Project, ref: n.ObjectRef) -> None:
        self._project, self._ref = project, ref

    @property
    def id(self) -> str:
        return self._ref.id

    @property
    def name(self) -> str:
        return self._project._manager().read(self._ref).name

    @name.setter
    def name(self, value: str) -> None:
        self._project.objects.rename(self, value)

    def __eq__(self, other: object) -> bool:
        if not isinstance(other, ObjectHandle):
            return NotImplemented
        return self._ref == other._ref

    def __hash__(self) -> int:
        return hash((id(self._project), self.id))

    def __repr__(self) -> str:
        try:
            state = self._project._manager().read(self._ref)
            details = f"name={state.name!r}"
            if isinstance(state, n.ProjectSensor):
                details += f", parent={state.parent_satellite_id!r}"
        except n.ObjectNotFoundError:
            details = "deleted"
        return f"{type(self).__name__}(id={self.id!r}, {details})"


class SatelliteHandle(ObjectHandle):
    __slots__ = ()

    @property
    def orbit(self) -> Orbit:
        return self._project.objects.read(self).orbit

    @orbit.setter
    def orbit(self, value: Orbit) -> None:
        self.update(orbit=value)

    @property
    def display(self) -> OrbitStyle:
        return self._project.objects.read(self).display

    @display.setter
    def display(self, value: OrbitStyle) -> None:
        self.update(display=value)

    def update(self, *, name: str | Unset = UNSET, orbit: Orbit | Unset = UNSET,
               display: OrbitStyle | Unset = UNSET) -> None:
        patch = n.SatelliteUpdate()
        if name is not UNSET:
            patch.name = require(name, str, "name")
        if orbit is not UNSET:
            patch.orbit = require(orbit, Orbit, "orbit")._native()
        if display is not UNSET:
            patch.display = require(display, OrbitStyle, "display")._native()
        self._project._manager().update(self._ref, patch)


class GroundStationHandle(ObjectHandle):
    __slots__ = ()

    @property
    def position_type(self) -> n.GroundPositionType:
        return self._project.objects.read(self).position_type

    @position_type.setter
    def position_type(self, value: n.GroundPositionType) -> None:
        self.update(position_type=value)

    @property
    def description(self) -> str:
        return self._project.objects.read(self).description

    @description.setter
    def description(self, value: str) -> None:
        self.update(description=value)

    @property
    def location(self) -> Wgs84Location:
        return self._project.objects.read(self).location

    @location.setter
    def location(self, value: Wgs84Location) -> None:
        self.update(location=value)

    @property
    def minimum_elevation(self) -> u.Angle:
        return self._project.objects.read(self).minimum_elevation

    @minimum_elevation.setter
    def minimum_elevation(self, value: u.Angle) -> None:
        self.update(minimum_elevation=value)

    def update(self, *, name: str | Unset = UNSET, location: Wgs84Location | Unset = UNSET,
               minimum_elevation: u.Angle | Unset = UNSET,
               position_type: n.GroundPositionType | Unset = UNSET,
               description: str | Unset = UNSET) -> None:
        patch = n.GroundStationUpdate()
        if name is not UNSET:
            patch.name = require(name, str, "name")
        if location is not UNSET:
            patch.location = require(location, Wgs84Location, "location")._native()
        if minimum_elevation is not UNSET:
            patch.minimum_elevation_rad = require(minimum_elevation, u.Angle, "minimum_elevation").radians
        if position_type is not UNSET:
            patch.position_type = require(position_type, n.GroundPositionType, "position_type")
        if description is not UNSET:
            patch.description = require(description, str, "description")
        self._project._manager().update(self._ref, patch)


class SensorHandle(ObjectHandle):
    __slots__ = ()

    @property
    def parent(self) -> SatelliteHandle:
        return cast(SatelliteHandle, self._project.objects[self._project.objects.read(self).parent_id])

    @property
    def field_of_view(self) -> FieldOfView:
        return self._project.objects.read(self).field_of_view

    @field_of_view.setter
    def field_of_view(self, value: FieldOfView) -> None:
        self.update(field_of_view=value)

    @property
    def pointing(self) -> Pointing:
        return self._project.objects.read(self).pointing

    @pointing.setter
    def pointing(self, value: Pointing) -> None:
        self.update(pointing=value)

    def update(self, *, name: str | Unset = UNSET, field_of_view: FieldOfView | Unset = UNSET,
               pointing: Pointing | Unset = UNSET) -> None:
        patch = n.SensorUpdate()
        if name is not UNSET:
            patch.name = require(name, str, "name")
        if field_of_view is not UNSET:
            patch.pattern = _to_native(field_of_view)
        if pointing is not UNSET:
            patch.pointing = require(pointing, Pointing, "pointing")._native()
        self._project._manager().update(self._ref, patch)


_KINDS = {Satellite: n.ObjectKind.SATELLITE, GroundStation: n.ObjectKind.GROUND_STATION,
          Sensor: n.ObjectKind.SENSOR}
_HANDLES = {n.ObjectKind.SATELLITE: SatelliteHandle, n.ObjectKind.GROUND_STATION: GroundStationHandle,
            n.ObjectKind.SENSOR: SensorHandle}
H = TypeVar("H", bound=ObjectHandle)
T = TypeVar("T")


class ObjectCollection:
    __slots__ = ("_project",)

    def __init__(self, project: Project) -> None:
        self._project = project

    def _wrap(self, ref: n.ObjectRef) -> ObjectHandle:
        return _HANDLES[ref.kind](self._project, ref)

    def __getitem__(self, id: str) -> ObjectHandle:
        try:
            return self._wrap(self._project._manager().get(require(id, str, "id")))
        except n.ObjectNotFoundError:
            raise KeyError(id) from None

    @overload
    def get(self, id: str) -> ObjectHandle | None: ...
    @overload
    def get(self, id: str, default: T) -> ObjectHandle | T: ...
    def get(self, id: str, default: T | None = None) -> ObjectHandle | T | None:
        try:
            return self[id]
        except KeyError:
            return default

    def __iter__(self) -> Iterator[ObjectHandle]:
        return iter(tuple(self._wrap(ref) for ref in self._project._manager().find()))

    def __len__(self) -> int:
        return len(self._project._manager().find())

    @overload
    def find(self, *, name: str | None = None, type: type[Satellite]) -> tuple[SatelliteHandle, ...]: ...
    @overload
    def find(self, *, name: str | None = None, type: type[GroundStation]) -> tuple[GroundStationHandle, ...]: ...
    @overload
    def find(self, *, name: str | None = None, type: type[Sensor]) -> tuple[SensorHandle, ...]: ...
    @overload
    def find(self, *, name: str | None = None, type: None = None) -> tuple[ObjectHandle, ...]: ...
    def find(self, *, name: str | None = None,
             type: type[Satellite] | type[GroundStation] | type[Sensor] | None = None) -> tuple[ObjectHandle, ...]:
        if type is not None and type not in _KINDS:
            raise TypeError("type expects objects.Satellite, GroundStation or Sensor")
        if name is not None:
            require(name, str, "name")
        return tuple(self._wrap(ref) for ref in self._project._manager().find(name, None if type is None else _KINDS[type]))

    @overload
    def create(self, definition: Satellite) -> SatelliteHandle: ...
    @overload
    def create(self, definition: GroundStation) -> GroundStationHandle: ...
    @overload
    def create(self, definition: Sensor) -> SensorHandle: ...
    def create(self, definition: Satellite | GroundStation | Sensor) -> ObjectHandle:
        if isinstance(definition, Satellite):
            return self.satellite(definition.name, orbit=definition.orbit, display=definition.display)
        if isinstance(definition, GroundStation):
            return self.ground_station(definition.name, location=definition.location,
                                       minimum_elevation=definition.minimum_elevation,
                                       position_type=definition.position_type, description=definition.description)
        if isinstance(definition, Sensor):
            return self.sensor(definition.name, parent=definition.parent,
                               field_of_view=definition.field_of_view, pointing=definition.pointing)
        raise TypeError("definition expects objects.Satellite, GroundStation or Sensor")

    def satellite(self, name: str, *, orbit: Orbit, display: OrbitStyle = OrbitStyle()) -> SatelliteHandle:
        definition = n.SatelliteDefinition(require(name, str, "name"), require(orbit, Orbit, "orbit")._native(),
                                           require(display, OrbitStyle, "display")._native())
        return SatelliteHandle(self._project, self._project._manager().create(definition))

    def ground_station(self, name: str, *, location: Wgs84Location,
                       minimum_elevation: u.Angle = u.deg(0),
                       position_type: n.GroundPositionType = n.GroundPositionType.GEODETIC,
                       description: str = "") -> GroundStationHandle:
        definition = n.GroundStationDefinition(require(name, str, "name"),
            require(location, Wgs84Location, "location")._native(),
            require(minimum_elevation, u.Angle, "minimum_elevation").radians,
            require(position_type, n.GroundPositionType, "position_type"), require(description, str, "description"))
        return GroundStationHandle(self._project, self._project._manager().create(definition))

    def sensor(self, name: str, *, parent: SatelliteHandle, field_of_view: FieldOfView,
               pointing: Pointing = Pointing()) -> SensorHandle:
        definition = n.SensorDefinition(require(name, str, "name"),
            require(parent, SatelliteHandle, "parent")._ref,
            _to_native(field_of_view),
            require(pointing, Pointing, "pointing")._native())
        return SensorHandle(self._project, self._project._manager().create(definition))

    @overload
    def read(self, obj: SatelliteHandle) -> SatelliteState: ...
    @overload
    def read(self, obj: GroundStationHandle) -> GroundStationState: ...
    @overload
    def read(self, obj: SensorHandle) -> SensorState: ...
    @overload
    def read(self, obj: ObjectHandle) -> SatelliteState | GroundStationState | SensorState: ...
    def read(self, obj: ObjectHandle) -> SatelliteState | GroundStationState | SensorState:
        state = self._project._manager().read(require(obj, ObjectHandle, "object")._ref)
        if isinstance(state, n.ProjectSatellite):
            return SatelliteState(obj.id, state.name, Orbit.from_input(state.orbit), OrbitStyle._read(state.display),
                                  state.model_kind, state.model_diameter_m, state.model_path)
        if isinstance(state, n.ProjectGroundStation):
            location = Wgs84Location(u.rad(state.location.longitude_rad), u.rad(state.location.latitude_rad),
                                     u.m(state.location.height_m), u.m(state.location.height_above_ground_m))
            return GroundStationState(obj.id, state.name, location, u.rad(state.minimum_elevation_rad),
                                      state.position_type, state.description)
        pointing = Pointing(state.target_station_id, self._project) if state.pointing == n.SensorPointing.TARGETED else Pointing()
        return SensorState(obj.id, state.name, state.parent_satellite_id, _from_native(state.pattern), pointing)

    def rename(self, obj: ObjectHandle, name: str) -> None:
        self._project._manager().rename(require(obj, ObjectHandle, "object")._ref, require(name, str, "name"))

    @overload
    def update(self, obj: SatelliteHandle, *, name: str | Unset = UNSET,
               orbit: Orbit | Unset = UNSET, display: OrbitStyle | Unset = UNSET) -> None: ...
    @overload
    def update(self, obj: GroundStationHandle, *, name: str | Unset = UNSET,
               location: Wgs84Location | Unset = UNSET, minimum_elevation: u.Angle | Unset = UNSET,
               position_type: n.GroundPositionType | Unset = UNSET, description: str | Unset = UNSET) -> None: ...
    @overload
    def update(self, obj: SensorHandle, *, name: str | Unset = UNSET,
               field_of_view: FieldOfView | Unset = UNSET, pointing: Pointing | Unset = UNSET) -> None: ...
    def update(self, obj: ObjectHandle, *, name: str | Unset = UNSET, orbit: Orbit | Unset = UNSET,
               display: OrbitStyle | Unset = UNSET, location: Wgs84Location | Unset = UNSET,
               minimum_elevation: u.Angle | Unset = UNSET, field_of_view: FieldOfView | Unset = UNSET,
               pointing: Pointing | Unset = UNSET,
               position_type: n.GroundPositionType | Unset = UNSET, description: str | Unset = UNSET) -> None:
        self._project._manager().read(require(obj, ObjectHandle, "object")._ref)
        values = {"name": name, "orbit": orbit, "display": display, "location": location,
                  "minimum_elevation": minimum_elevation, "field_of_view": field_of_view, "pointing": pointing,
                  "position_type": position_type, "description": description}
        if isinstance(obj, SatelliteHandle):
            allowed = {"name", "orbit", "display"}
        elif isinstance(obj, GroundStationHandle):
            allowed = {"name", "location", "minimum_elevation", "position_type", "description"}
        elif isinstance(obj, SensorHandle):
            allowed = {"name", "field_of_view", "pointing"}
        else:
            raise TypeError("Expected a satellite, ground station or sensor handle")
        invalid = {field for field, value in values.items() if value is not UNSET} - allowed
        if invalid:
            raise TypeError(f"{type(obj).__name__} does not accept {', '.join(sorted(invalid))}; "
                            f"expected {', '.join(sorted(allowed))}")
        if isinstance(obj, SatelliteHandle):
            obj.update(name=name, orbit=orbit, display=display)
        elif isinstance(obj, GroundStationHandle):
            obj.update(name=name, location=location, minimum_elevation=minimum_elevation,
                       position_type=position_type, description=description)
        elif isinstance(obj, SensorHandle):
            obj.update(name=name, field_of_view=field_of_view, pointing=pointing)

    def duplicate(self, obj: H, *, name: str | None = None) -> H:
        ref = require(obj, ObjectHandle, "object")._ref
        return cast(H, self._wrap(self._project._manager().duplicate(ref, obj.name if name is None else require(name, str, "name"))))

    def children(self, parent: ObjectHandle) -> tuple[SensorHandle, ...]:
        refs = self._project._manager().children(require(parent, ObjectHandle, "parent")._ref)
        return tuple(SensorHandle(self._project, ref) for ref in refs)

    def reparent(self, sensor: SensorHandle, parent: SatelliteHandle) -> None:
        self._project._manager().reparent(require(sensor, SensorHandle, "sensor")._ref,
                                          require(parent, SatelliteHandle, "parent")._ref)

    def plan_remove(self, obj: ObjectHandle) -> n.RemovalPlan:
        return self._project._manager().plan_remove(require(obj, ObjectHandle, "object")._ref)

    def remove(self, obj: ObjectHandle) -> None:
        self._project._manager().remove(require(obj, ObjectHandle, "object")._ref)
