"""Fixed-input calculation facade. No live object reads or implicit data updates."""
from __future__ import annotations
from dataclasses import dataclass
from itertools import islice
from importlib import import_module
from math import pi
from os import PathLike
from operator import index as integer_index
from typing import TYPE_CHECKING, Any, Callable, Iterable, Iterator, overload
from . import _native as n
from . import reports
from ._values import require
from .objects import ObjectHandle, SatelliteHandle, GroundStationHandle, SensorHandle
from .units import Duration

if TYPE_CHECKING:
    from ._project import Project


def _numpy() -> Any:
    try:
        numpy = import_module("numpy")
    except ImportError as error:
        raise ImportError("NumPy conversion requires numpy; install it with `pip install numpy`") from error
    return numpy


@dataclass(frozen=True, slots=True)
class VectorColumn:
    """Owned SI values; NumPy conversions always return independent (N, 3) arrays."""
    values: tuple[tuple[float, float, float], ...]
    unit: str

    def __len__(self) -> int:
        return len(self.values)

    def __iter__(self) -> Iterator[tuple[float, float, float]]:
        return iter(self.values)

    def to_numpy(self, *, unit: str | None = None) -> Any:
        target = self.unit if unit is None else unit
        factors = {"m": {"m": 1, "km": 0.001}, "m/s": {"m/s": 1, "km/s": 0.001}}
        if target not in factors[self.unit]:
            raise ValueError(f"Cannot convert {self.unit} to {target!r}")
        array = _numpy().array(self.values, dtype="float64", copy=True).reshape((-1, 3))
        array *= factors[self.unit][target]
        return array


@dataclass(frozen=True, slots=True)
class EpochColumn:
    """TAI nanoseconds from 2000-01-01 12:00:00 TAI, never a datetime64 scale."""
    tai_nanoseconds: tuple[int, ...]

    def __len__(self) -> int:
        return len(self.tai_nanoseconds)

    def __iter__(self) -> Iterator[n.Epoch]:
        return (n.Epoch(tick) for tick in self.tai_nanoseconds)

    def to_numpy(self) -> Any:
        return _numpy().array(self.tai_nanoseconds, dtype="int64", copy=True)


@dataclass(frozen=True, slots=True)
class ScalarColumn:
    """Owned SI scalars; undefined values become NaN only in NumPy/pandas copies."""
    values: tuple[float | None, ...]
    unit: str

    def __len__(self) -> int:
        return len(self.values)

    def __iter__(self) -> Iterator[float | None]:
        return iter(self.values)

    def to_numpy(self, *, unit: str | None = None) -> Any:
        target = self.unit if unit is None else unit
        factors = {"m": {"m": 1, "km": 0.001}, "m/s": {"m/s": 1, "km/s": 0.001},
                   "rad": {"rad": 1, "deg": 180 / pi}}
        if target not in factors[self.unit]:
            raise ValueError(f"Cannot convert {self.unit} to {target!r}")
        return _numpy().array(self.values, dtype="float64", copy=True) * factors[self.unit][target]


@dataclass(frozen=True)
class Trajectory:
    """Owned state query, independent of future edits and of the Analysis lifetime."""
    _native: n.Trajectory

    def __len__(self) -> int:
        return len(self._native)

    def __iter__(self) -> Iterator[n.CartesianState]:
        return iter(self._native.states)

    @property
    def epoch(self) -> EpochColumn:
        return EpochColumn(tuple(self._native.epoch_tai_ns))

    @property
    def position(self) -> VectorColumn:
        return VectorColumn(tuple((v[0], v[1], v[2]) for v in self._native.position_m), "m")

    @property
    def velocity(self) -> VectorColumn:
        return VectorColumn(tuple((v[0], v[1], v[2]) for v in self._native.velocity_mps), "m/s")

    @property
    def origin(self) -> n.OriginId:
        return self._native.origin

    @property
    def frame(self) -> n.FrameId:
        return self._native.frame

    @property
    def object_name(self) -> str:
        return self._native.object_name

    @property
    def provenance(self) -> n.Provenance:
        return self._native.provenance

    @property
    def input_sha256(self) -> str:
        return self._native.input_sha256

    @property
    def data_sha256(self) -> str:
        return self._native.data_sha256

    def to_csv(self, path: str | PathLike[str], *, overwrite: bool = False) -> None:
        """Atomically export exact epoch_tai_ns, SI states, origin and frame."""
        self._native.to_csv(path, require(overwrite, bool, "overwrite"))

    def to_pandas(self) -> Any:
        """Return a copy, retaining integer TAI nanoseconds and metadata in attrs."""
        try:
            pd = import_module("pandas")
        except ImportError as error:
            raise ImportError("DataFrame conversion requires pandas; install it with `pip install pandas`") from error
        position, velocity = self.position.to_numpy(), self.velocity.to_numpy()
        table = pd.DataFrame({"epoch_tai_ns": self.epoch.to_numpy(),
            **{f"position.{axis}_m": position[:, i] for i, axis in enumerate("xyz")},
            **{f"velocity.{axis}_mps": velocity[:, i] for i, axis in enumerate("xyz")}}, copy=True)
        table.attrs = {"frame": self.frame.name, "origin": self.origin.name,
            "epoch_origin": "2000-01-01T12:00:00 TAI", "time_scale": "TAI",
            "units": {"epoch_tai_ns": "ns", "position": "m", "velocity": "m/s"},
            "object_id": self.provenance.object_id, "object_name": self.object_name,
            "input_sha256": self.input_sha256, "data_sha256": self.data_sha256,
            "algorithm": self.provenance.algorithm}
        return table


@dataclass(frozen=True)
class AccessWindow:
    """A positive-duration visible interval; clipped boundaries are query limits."""
    _native: n.AccessWindow

    @property
    def start(self) -> n.Epoch:
        return self._native.start

    @property
    def end(self) -> n.Epoch:
        return self._native.end

    @property
    def duration(self) -> Duration:
        return Duration(self._native.duration_nanoseconds)

    @property
    def start_clipped(self) -> bool:
        return self._native.start_clipped

    @property
    def end_clipped(self) -> bool:
        return self._native.end_clipped


@dataclass(frozen=True)
class AccessWindows:
    """Owned access results, query settings and provenance; export never recomputes."""
    _native: n.AccessWindows

    def __len__(self) -> int:
        return len(self._native)

    def __iter__(self) -> Iterator[AccessWindow]:
        return (AccessWindow(window) for window in self._native.windows)

    @overload
    def __getitem__(self, index: int) -> AccessWindow: ...
    @overload
    def __getitem__(self, index: slice) -> tuple[AccessWindow, ...]: ...
    def __getitem__(self, index: int | slice) -> AccessWindow | tuple[AccessWindow, ...]:
        if isinstance(index, slice):
            return tuple(self[i] for i in range(*index.indices(len(self))))
        offset = integer_index(index)
        if offset < 0:
            offset += len(self)
        if not 0 <= offset < len(self):
            raise IndexError("Access window index out of range")
        return AccessWindow(self._native.window(offset))

    @property
    def provenance(self) -> n.AccessProvenance:
        return self._native.provenance

    @property
    def first(self) -> n.Epoch:
        return self._native.first

    @property
    def last(self) -> n.Epoch:
        return self._native.last

    @property
    def search_step(self) -> Duration:
        return Duration(self._native.search_step_nanoseconds)

    @property
    def boundary_tolerance(self) -> Duration:
        return Duration(self._native.boundary_tolerance_nanoseconds)

    @property
    def input_sha256(self) -> str:
        return self.provenance.input_sha256

    @property
    def data_sha256(self) -> str:
        return self.provenance.data_sha256

    def to_csv(self, path: str | PathLike[str], *, overwrite: bool = False) -> None:
        """Atomically export IDs, exact TAI ns bounds/duration, seconds and clip flags (0/1)."""
        self._native.to_csv(path, require(overwrite, bool, "overwrite"))

    def to_pandas(self) -> Any:
        """Return an independent DataFrame with exact integer TAI ns and metadata."""
        try:
            pd = import_module("pandas")
        except ImportError as error:
            raise ImportError("DataFrame conversion requires pandas; install it with `pip install pandas`") from error
        windows = tuple(self)
        provenance = self.provenance
        table = pd.DataFrame({
            "station_id": pd.Series([provenance.station_id] * len(self), dtype="str"),
            "target_id": pd.Series([provenance.target_id] * len(self), dtype="str"),
            "start_tai_ns": pd.Series([w.start.tai_nanoseconds for w in windows], dtype="int64"),
            "end_tai_ns": pd.Series([w.end.tai_nanoseconds for w in windows], dtype="int64"),
            "duration_ns": pd.Series([w.duration.nanoseconds for w in windows], dtype="int64"),
            "duration_s": pd.Series([w.duration.nanoseconds / 1e9 for w in windows], dtype="float64"),
            "start_clipped": pd.Series([w.start_clipped for w in windows], dtype="bool"),
            "end_clipped": pd.Series([w.end_clipped for w in windows], dtype="bool"),
        }, copy=True)
        table.attrs = {
            "epoch_origin": "2000-01-01T12:00:00 TAI", "time_scale": "TAI",
            "first_tai_ns": self.first.tai_nanoseconds, "last_tai_ns": self.last.tai_nanoseconds,
            "search_step_ns": self.search_step.nanoseconds,
            "boundary_tolerance_ns": self.boundary_tolerance.nanoseconds,
            **{name: getattr(provenance, name) for name in (
                "station_id", "station_name", "target_id", "target_name", "input_revision",
                "input_sha256", "data_sha256", "algorithm")},
        }
        return table


@dataclass(frozen=True)
class AerSample:
    """One station-local observation in SI units, including invisible targets."""
    epoch: n.Epoch
    azimuth_rad: float | None
    elevation_rad: float
    range_m: float
    range_rate_mps: float
    off_boresight_rad: float | None
    line_of_sight: bool
    visible: bool

    @classmethod
    def _from_native(cls, sample: n.AccessSample) -> AerSample:
        o = sample.observation
        return cls(sample.epoch, o.azimuth_rad, o.elevation_rad, o.range_m, o.range_rate_mps,
                   sample.off_boresight_rad, o.line_of_sight, sample.visible)


@dataclass(frozen=True)
class AerSeries:
    """Owned AER values in requested order; no visibility filtering or live object reads."""
    _native: n.AerSeries

    def __len__(self) -> int:
        return len(self._native)

    def __iter__(self) -> Iterator[AerSample]:
        return (AerSample._from_native(sample) for sample in self._native.samples)

    @overload
    def __getitem__(self, index: int) -> AerSample: ...

    @overload
    def __getitem__(self, index: slice) -> tuple[AerSample, ...]: ...

    def __getitem__(self, index: int | slice) -> AerSample | tuple[AerSample, ...]:
        if isinstance(index, slice):
            return tuple(self[i] for i in range(*index.indices(len(self))))
        offset = integer_index(index)
        if offset < 0:
            offset += len(self)
        if not 0 <= offset < len(self):
            raise IndexError("AER sample index out of range")
        return AerSample._from_native(self._native.sample(offset))

    @property
    def epoch(self) -> EpochColumn:
        return EpochColumn(tuple(s.epoch.tai_nanoseconds for s in self))

    @property
    def azimuth(self) -> ScalarColumn:
        return ScalarColumn(tuple(s.azimuth_rad for s in self), "rad")

    @property
    def elevation(self) -> ScalarColumn:
        return ScalarColumn(tuple(s.elevation_rad for s in self), "rad")

    @property
    def range(self) -> ScalarColumn:
        return ScalarColumn(tuple(s.range_m for s in self), "m")

    @property
    def range_rate(self) -> ScalarColumn:
        return ScalarColumn(tuple(s.range_rate_mps for s in self), "m/s")

    @property
    def off_boresight(self) -> ScalarColumn:
        return ScalarColumn(tuple(s.off_boresight_rad for s in self), "rad")

    @property
    def line_of_sight(self) -> tuple[bool, ...]:
        return tuple(s.line_of_sight for s in self)

    @property
    def visible(self) -> tuple[bool, ...]:
        return tuple(s.visible for s in self)

    @property
    def provenance(self) -> n.AccessProvenance:
        return self._native.provenance

    @property
    def input_sha256(self) -> str:
        return self.provenance.input_sha256

    @property
    def data_sha256(self) -> str:
        return self.provenance.data_sha256

    def to_csv(self, path: str | PathLike[str], *, overwrite: bool = False) -> None:
        """Atomic SI CSV; exact epoch_tai_ns, blank undefined angles, boolean flags as 0/1."""
        self._native.to_csv(path, require(overwrite, bool, "overwrite"))

    def to_pandas(self) -> Any:
        """Independent columns with int64 epochs, float64 SI values and bool flags."""
        try:
            pd = import_module("pandas")
        except ImportError as error:
            raise ImportError("DataFrame conversion requires pandas; install it with `pip install pandas`") from error
        samples = tuple(self)
        provenance = self.provenance
        columns = {
            "station_id": pd.Series([provenance.station_id] * len(self), dtype="str"),
            "target_id": pd.Series([provenance.target_id] * len(self), dtype="str"),
            "epoch_tai_ns": pd.Series([s.epoch.tai_nanoseconds for s in samples], dtype="int64"),
        }
        for name in ("azimuth_rad", "elevation_rad", "range_m", "range_rate_mps", "off_boresight_rad"):
            columns[name] = pd.Series([getattr(s, name) for s in samples], dtype="float64")
        for name in ("line_of_sight", "visible"):
            columns[name] = pd.Series([getattr(s, name) for s in samples], dtype="bool")
        table = pd.DataFrame(columns, copy=True)
        table.attrs = {
            "epoch_origin": "2000-01-01T12:00:00 TAI", "time_scale": "TAI",
            "frame": "WGS84 station-local ENU", "units": "SI",
            **{name: getattr(provenance, name) for name in (
                "station_id", "station_name", "target_id", "target_name", "input_revision",
                "input_sha256", "data_sha256", "algorithm")},
        }
        return table


class DataChangedError(RuntimeError):
    """The pinned plan requires an explicit decision about changed data."""
    def __init__(self, plan: AnalysisPlan | ProjectOpenPlan) -> None:
        self.plan = plan
        super().__init__("Selected calculation data differ from the project's recorded data; "
                         "inspect error.plan.data_changes before explicitly accepting its plan")


@dataclass(frozen=True)
class ProjectOpenPlan:
    """Parsed file and selected data held in memory until the caller decides."""
    _native: n.ProjectOpenPlan
    _project_type: type[Project]

    @property
    def name(self) -> str:
        return self._native.snapshot.name

    @property
    def data(self) -> n.DataSnapshot:
        return self._native.data

    @property
    def data_changes(self) -> tuple[n.DataChange, ...]:
        return tuple(self._native.data_changes)

    def open(self, *, accept_data_changes: bool = False) -> Project:
        require(accept_data_changes, bool, "accept_data_changes")
        try:
            return self._project_type._from_document(self._native.open(accept_data_changes))
        except n.DataChangedError:
            raise DataChangedError(self) from None


@dataclass(frozen=True)
class AnalysisPlan:
    _native: n.AnalysisPlan
    _project: Project

    @property
    def revision(self) -> int:
        return self._native.snapshot.revision

    @property
    def data(self) -> n.DataSnapshot:
        return self._native.data

    @property
    def data_changes(self) -> tuple[n.DataChange, ...]:
        return tuple(self._native.data_changes)

    def prepare(self, *, accept_data_changes: bool = False) -> Analysis:
        if self._project._active_edit() is not None:
            raise RuntimeError("Finish the project edit before preparing an analysis")
        require(accept_data_changes, bool, "accept_data_changes")
        try:
            return Analysis(self._native.prepare(accept_data_changes))
        except n.DataChangedError:
            raise DataChangedError(self) from None


@dataclass(frozen=True)
class Analysis:
    _native: n.Analysis

    def covariance(self, satellite: SatelliteHandle, *, at: n.Epoch,
                   cancelled: Callable[[], bool] | None = None) -> n.HpopCovarianceState:
        """Propagate this snapshot's configured covariance to an exact TAI endpoint.

        Returns the full active joint covariance, nominal state, STM and contributions
        in Earth/GCRF SI units. Requires enabled covariance on a single HPOP coast.
        """
        return self._native.covariance(require(satellite, SatelliteHandle, "satellite")._ref,
                                       require(at, n.Epoch, "at"), cancelled)

    def maneuvers(self, satellite: SatelliteHandle, *,
                  cancelled: Callable[[], bool] | None = None) -> n.ManeuverSeries:
        """Burns inside this scenario, with GCRF pre/post states and cumulative delta-v.

        Non-mission satellites return an empty series. Results own their data and provenance.
        Cumulative delta-v includes preceding burns when the initial epoch is before the scenario.
        """
        return self._native.maneuvers(require(satellite, SatelliteHandle, "satellite")._ref, cancelled)

    @property
    def revision(self) -> int:
        return self._native.snapshot.revision

    @property
    def first(self) -> n.Epoch:
        return self._native.snapshot.first

    @property
    def last(self) -> n.Epoch:
        return self._native.snapshot.last

    @property
    def data(self) -> n.DataSnapshot:
        return self._native.data

    def state(self, object: SatelliteHandle | GroundStationHandle, *, at: n.Epoch,
              frame: n.FrameId = n.FrameId.GCRF) -> n.CartesianState:
        return self._native.state(require(object, ObjectHandle, "object")._ref,
                                  require(at, n.Epoch, "at"), require(frame, n.FrameId, "frame"))

    def aer(self, station: GroundStationHandle, target: SatelliteHandle | SensorHandle, *,
            step: Duration | None = None, times: Iterable[n.Epoch] | None = None,
            first: n.Epoch | None = None, last: n.Epoch | None = None,
            cancelled: Callable[[], bool] | None = None) -> AerSeries:
        """Sample station-local AER with fixed UTC/EOP data. Specify step or exact times.

        A positive step includes both endpoints; explicit times preserve order/duplicates.
        Visibility uses line of sight, station minimum elevation and optional sensor FOV.
        Undefined azimuth/off-boresight remains None. Cancellation never returns partial data.
        """
        require(station, GroundStationHandle, "station")
        if not isinstance(target, (SatelliteHandle, SensorHandle)):
            raise TypeError("target must be a SatelliteHandle or SensorHandle")
        if (step is None) == (times is None):
            raise TypeError("Specify exactly one of step or times")
        if step is not None:
            require(step, Duration, "step")
            if first is not None:
                require(first, n.Epoch, "first")
            if last is not None:
                require(last, n.Epoch, "last")
            return AerSeries(self._native.aer_step(station._ref, target._ref, step.nanoseconds,
                                                   first, last, cancelled))
        if first is not None or last is not None:
            raise TypeError("first/last are only used with step; times gives exact query epochs")
        assert times is not None
        epochs = tuple(islice(times, n.MAX_ANALYSIS_SAMPLES + 1))
        if len(epochs) > n.MAX_ANALYSIS_SAMPLES:
            raise ValueError("AER query exceeds 200000 samples; split the query")
        for epoch in epochs:
            require(epoch, n.Epoch, "times item")
        return AerSeries(self._native.aer_at(station._ref, target._ref, epochs, cancelled))

    def report(self, kind: n.ReportKind, *,
               objects: ObjectHandle | Iterable[ObjectHandle] | n.ReportScope | None = None,
               station: GroundStationHandle | None = None,
               target: SatelliteHandle | SensorHandle | None = None,
               step: Duration | None = None,
               first: n.Epoch | None = None, last: n.Epoch | None = None,
               frame: n.FrameId = n.FrameId.GCRF,
               units: n.ReportUnits | None = None,
               time_scale: n.ReportTimeScale | None = None,
               template: reports.Template | None = None,
               cancelled: Callable[[], bool] | None = None,
               progress: Callable[[int, int], None] | None = None) -> n.ReportResult:
        """Generate a built-in report from this snapshot, with no file/data rereads.

        Select objects explicitly (reports.ALL for all compatible objects), or
        station/target for AER and access reports. A template controls presentation.
        SCENARIO_SUMMARY needs no selection. Sampled reports require step. TAI is
        the default display scale; UTC requires data selected when preparing.
        """
        require(kind, n.ReportKind, "kind")
        access = kind in (n.ReportKind.AER, n.ReportKind.ACCESS_INTERVALS)
        sampled = kind not in (n.ReportKind.SCENARIO_SUMMARY, n.ReportKind.OBJECT_SUMMARY,
                               n.ReportKind.GROUND_STATIONS)
        if access:
            if objects is not None:
                raise TypeError("AER/access reports select station and target, not objects")
            require(station, GroundStationHandle, "station")
            if not isinstance(target, (SatelliteHandle, SensorHandle)):
                raise TypeError("target must be a SatelliteHandle or SensorHandle")
            selection = (station, target)
        else:
            if station is not None or target is not None:
                raise TypeError("station/target are only used by AER/access reports")
            if kind == n.ReportKind.SCENARIO_SUMMARY:
                if objects is not None:
                    raise TypeError("SCENARIO_SUMMARY describes the project; omit objects")
                selection = ()
            elif isinstance(objects, n.ReportScope) and objects == reports.ALL:
                selection = ()
            else:
                if objects is None:
                    raise TypeError("Select report objects explicitly")
                selection = (objects,) if isinstance(objects, ObjectHandle) else tuple(islice(objects, 10001))
                if not selection:
                    raise ValueError("Select at least one report object")
                if len(selection) > 10000:
                    raise ValueError("Report selection exceeds 10000 objects")
        snapshot = self._native.snapshot
        ids = []
        for object in selection:
            ref = require(object, ObjectHandle, "objects item")._ref
            snapshot.read(ref)  # Captured scope/existence; a deleted live handle still works.
            ids.append(ref.id)
        definition = n.ReportDefinition()
        definition.id = "analysis-" + kind.name.lower().replace("_", "-")
        definition.name = kind.name.replace("_", " ").title()
        definition.query.kind = kind
        definition.query.scope = (n.ReportScope.ALL_COMPATIBLE
                                  if isinstance(objects, n.ReportScope) and objects == reports.ALL
                                  else n.ReportScope.OBJECTS)
        definition.query.object_ids = ids
        definition.query.frame = require(frame, n.FrameId, "frame")
        if template is not None:
            if units is not None or time_scale is not None:
                raise TypeError("Set units/time_scale in template when supplying a template")
            require(template, reports.Template, "template")
        else:
            template = reports.Template(
                units=n.ReportUnits.ENGINEERING if units is None else units,
                time_scale=n.ReportTimeScale.TAI if time_scale is None else time_scale)
        definition.format = template._to_native()
        if sampled:
            definition.query.step_nanoseconds = require(step, Duration, "step").nanoseconds
            if first is not None or last is not None:
                definition.query.follow_scenario = False
                definition.query.first = snapshot.first if first is None else require(first, n.Epoch, "first")
                definition.query.last = snapshot.last if last is None else require(last, n.Epoch, "last")
        elif step is not None or first is not None or last is not None:
            raise TypeError("Static reports do not use step or first/last")
        return self._native.report(definition, cancelled, progress)

    def access(self, station: GroundStationHandle, target: SatelliteHandle | SensorHandle, *,
               search_step: Duration, boundary_tolerance: Duration = Duration(1_000_000),
               first: n.Epoch | None = None, last: n.Epoch | None = None,
               cancelled: Callable[[], bool] | None = None) -> AccessWindows:
        """Search this snapshot using station elevation/LOS and optional sensor FOV.

        UTC/EOP data must be selected at prepare(). A fixed discovery grid can
        miss windows or gaps shorter than search_step. Boundary tolerance only
        refines discovered transitions. Cancellation raises AnalysisCancelled;
        no partial results are returned. A zero-length query returns no windows.
        """
        require(station, GroundStationHandle, "station")
        if not isinstance(target, (SatelliteHandle, SensorHandle)):
            raise TypeError("target must be a SatelliteHandle or SensorHandle")
        require(search_step, Duration, "search_step")
        require(boundary_tolerance, Duration, "boundary_tolerance")
        if first is not None:
            require(first, n.Epoch, "first")
        if last is not None:
            require(last, n.Epoch, "last")
        if cancelled is not None and not callable(cancelled):
            raise TypeError("cancelled must be callable or None")
        return AccessWindows(self._native.access(
            station._ref, target._ref, search_step_nanoseconds=search_step.nanoseconds,
            boundary_tolerance_nanoseconds=boundary_tolerance.nanoseconds,
            first=first, last=last, cancelled=cancelled))

    def states(self, object: SatelliteHandle | GroundStationHandle, *,
               step: Duration | None = None, times: Iterable[n.Epoch] | None = None,
               frame: n.FrameId = n.FrameId.GCRF, first: n.Epoch | None = None,
               last: n.Epoch | None = None) -> Trajectory:
        """Use exactly one of step/times. Subinterval bounds apply only to step.

        step includes the exact stop, including a shorter last interval. times
        preserves order/duplicates and accepts []; it never inserts endpoints.
        """
        ref = require(object, ObjectHandle, "object")._ref
        require(frame, n.FrameId, "frame")
        if (step is None) == (times is None):
            raise TypeError("Specify exactly one of step or times")
        if step is not None:
            require(step, Duration, "step")
            if first is not None:
                require(first, n.Epoch, "first")
            if last is not None:
                require(last, n.Epoch, "last")
            return Trajectory(self._native.states_step(ref, step.nanoseconds, frame, first, last))
        if first is not None or last is not None:
            raise TypeError("first/last are only used with step; times gives exact query epochs")
        assert times is not None
        epochs = tuple(islice(times, n.MAX_ANALYSIS_SAMPLES + 1))
        if len(epochs) > n.MAX_ANALYSIS_SAMPLES:
            raise ValueError("State query exceeds 200000 samples; split the query")
        for epoch in epochs:
            require(epoch, n.Epoch, "times item")
        return Trajectory(self._native.states_at(ref, epochs, frame))
