"""Fixed-input calculation facade. No live object reads or implicit data updates."""
from __future__ import annotations
from dataclasses import dataclass
from itertools import islice
from importlib import import_module
from os import PathLike
from operator import index as integer_index
from typing import TYPE_CHECKING, Any, Callable, Iterable, Iterator, overload
from . import _native as n
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
