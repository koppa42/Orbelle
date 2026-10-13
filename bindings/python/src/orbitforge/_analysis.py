"""Fixed-input calculation facade. No live object reads or implicit data updates."""
from __future__ import annotations
from dataclasses import dataclass
from itertools import islice
from importlib import import_module
from os import PathLike
from typing import TYPE_CHECKING, Any, Iterable, Iterator
from . import _native as n
from ._values import require
from .objects import ObjectHandle, SatelliteHandle, GroundStationHandle
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


class DataChangedError(RuntimeError):
    """The pinned plan requires an explicit decision about changed data."""
    def __init__(self, plan: AnalysisPlan) -> None:
        self.plan = plan
        super().__init__("Selected calculation data differ from the project's recorded data; "
                         "inspect error.plan.data_changes before explicitly accepting its plan")


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
