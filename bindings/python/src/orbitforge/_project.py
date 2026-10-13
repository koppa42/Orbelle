"""Python editing facade; all domain mutations are C++ application commands."""
from __future__ import annotations
import asyncio
from contextvars import ContextVar, Token
from os import PathLike
from threading import get_ident
from typing import Callable, Literal
import weakref
from . import _native as n
from ._values import UNSET, Unset, require
from .objects import ObjectCollection
from ._analysis import Analysis, AnalysisPlan


def _owner() -> tuple[int, object]:
    try:
        task = asyncio.current_task()
    except RuntimeError:
        task = None
    return get_ident(), task


class ProjectEdit:
    __slots__ = ("_project", "_edit", "_token", "_owner", "_used", "_closed")

    def __init__(self, project: Project) -> None:
        self._project = project
        self._edit: n.ProjectEdit | None = None
        self._token: Token | None = None
        self._owner: tuple[int, object] | None = None
        self._used, self._closed = False, False

    def __enter__(self) -> Project:
        if self._used:
            raise RuntimeError("A project edit context can only be entered once")
        if self._project._active_edit() is not None:
            raise RuntimeError("Nested edits of the same project are not supported")
        self._used, self._owner = True, _owner()
        self._edit = self._project._document.edit()
        self._token = self._project._route.set(self)
        return self._project

    def __exit__(self, exc_type: object, exc_value: object, traceback: object) -> Literal[False]:
        if self._owner != _owner() or self._closed or self._token is None:
            raise RuntimeError("Exit the edit in the thread and task that entered it")
        # Reset BEFORE C++ publishes and invokes observers. They must see the
        # committed project, and may start their own independent edit.
        self._project._route.reset(self._token)
        self._closed = True
        assert self._edit is not None
        if exc_type is not None:
            self._edit.discard()
        else:
            self._edit.commit()
        return False


class Project:
    """Editable project with stable object handles and atomic edits.

    Epochs are explicit TAI nanoseconds from 2000-01-01 12:00:00 TAI.
    No constructor, attribute access or edit downloads data or computes an orbit.
    """
    __slots__ = ("_document", "_objects", "_route", "_observers", "__weakref__")

    def __init__(self, name: str, first: n.Epoch, last: n.Epoch) -> None:
        self._initialize(n.Project(require(name, str, "name"), first, last))

    def _initialize(self, data: n.Project) -> None:
        self._document = n.ProjectDocument(data)
        self._route: ContextVar[ProjectEdit | None] = ContextVar("orbitforge_project_edit", default=None)
        self._objects = ObjectCollection(self)
        self._observers: dict[int, Callable[[n.ChangeSet], None]] = {}

    @classmethod
    def from_data(cls, data: n.Project) -> Project:
        """Validate and copy a low-level project value; preserve its stable IDs."""
        result = cls.__new__(cls)
        result._initialize(require(data, n.Project, "data"))
        return result

    @classmethod
    def open(cls, path: str | PathLike[str]) -> Project:
        """Read the stored input and provenance, without replacing external data."""
        return cls.from_data(n.load_project(path))

    def _active_edit(self) -> n.ProjectEdit | None:
        context = self._route.get()
        if context is None or context._closed:
            return None
        if context._owner != _owner():
            raise RuntimeError("An active project edit cannot be shared with another thread or asyncio task")
        return context._edit

    def _manager(self) -> n.ObjectManager:
        edit = self._active_edit()
        return edit.objects() if edit is not None else self._document.objects()

    def _snapshot(self) -> n.ProjectSnapshot:
        edit = self._active_edit()
        return edit.snapshot() if edit is not None else self._document.snapshot()

    def _change(self, operation: Callable[[n.ProjectEdit], None]) -> None:
        edit = self._active_edit()
        if edit is not None:
            operation(edit)
            return
        edit = self._document.edit()
        try:
            operation(edit)
            edit.commit()
        finally:
            edit.discard()

    @property
    def objects(self) -> ObjectCollection:
        return self._objects

    @property
    def revision(self) -> int:
        return self._snapshot().revision

    @property
    def dirty(self) -> bool:
        """Whether the published project differs from its last saved revision."""
        return self._document.dirty

    def edit(self) -> ProjectEdit:
        return ProjectEdit(self)

    @property
    def name(self) -> str:
        return self._snapshot().name

    @name.setter
    def name(self, value: str) -> None:
        self.update(name=value)

    @property
    def first(self) -> n.Epoch:
        return self._snapshot().first

    @first.setter
    def first(self, value: n.Epoch) -> None:
        self.update(first=value)

    @property
    def last(self) -> n.Epoch:
        return self._snapshot().last

    @last.setter
    def last(self, value: n.Epoch) -> None:
        self.update(last=value)

    @property
    def current(self) -> n.Epoch:
        return self._snapshot().current

    @current.setter
    def current(self, value: n.Epoch) -> None:
        self.update(current=value)

    def update(self, *, name: str | Unset = UNSET, first: n.Epoch | Unset = UNSET,
               last: n.Epoch | Unset = UNSET, current: n.Epoch | Unset = UNSET) -> None:
        values = (None if name is UNSET else require(name, str, "name"),
                  None if first is UNSET else require(first, n.Epoch, "first"),
                  None if last is UNSET else require(last, n.Epoch, "last"),
                  None if current is UNSET else require(current, n.Epoch, "current"))
        self._change(lambda edit: edit.settings(*values))

    @property
    def earth_orientation(self) -> n.EopDataInfo | None:
        return self._snapshot().earth_orientation

    @earth_orientation.setter
    def earth_orientation(self, value: n.EopDataInfo | None) -> None:
        if value is not None:
            require(value, n.EopDataInfo, "earth_orientation")
        self._change(lambda edit: edit.earth_orientation(value))

    @property
    def data_lock(self) -> n.DataLock:
        return self._snapshot().data_lock

    def set_reports(self, definitions: list[n.ReportDefinition]) -> None:
        """Copy and replace stored report definitions in one validated edit."""
        self._change(lambda edit: edit.reports(definitions))

    def to_data(self) -> n.Project:
        """Return a detached low-level copy, for persistence/legacy integration.

        Mutating the returned value never changes this project. Ordinary object
        reads should use objects.read(handle), which returns an immutable state.
        """
        return self._snapshot().project

    def _calculation_data(self) -> n.Project:
        if self._active_edit() is not None:
            raise RuntimeError("Finish the project edit before saving or calculating")
        return self._document.snapshot().project

    def to_scenario(self, time: n.TimeProvider | None = None) -> n.Scenario:
        return self._calculation_data().to_scenario(time)

    def plan_prepare(self, *, data: n.DataSnapshot | None = None) -> AnalysisPlan:
        """Pin committed inputs and selected providers; validate before any consent."""
        if self._active_edit() is not None:
            raise RuntimeError("Finish the project edit before preparing an analysis")
        selected = n.DataSnapshot() if data is None else require(data, n.DataSnapshot, "data")
        return AnalysisPlan(n.AnalysisPlan(self._document, selected), self)

    def prepare(self, *, data: n.DataSnapshot | None = None) -> Analysis:
        """Prepare fixed inputs. Changed recorded data raises DataChangedError with its plan."""
        return self.plan_prepare(data=data).prepare()

    def save(self, path: str | PathLike[str], overwrite: bool = False) -> None:
        if self._active_edit() is not None:
            raise RuntimeError("Finish the project edit before saving or calculating")
        snapshot = self._document.snapshot()
        snapshot.project.save(path, overwrite)
        self._document.mark_saved(snapshot.revision)

    def subscribe(self, observer: Callable[[n.ChangeSet], None]) -> int:
        """Observe successful commits; retain the token to unsubscribe later.

        Notifications run on the committing thread. GUI clients must dispatch to
        their owner thread. Exceptions are retained in observer_errors.
        """
        if not callable(observer):
            raise TypeError("observer must be callable")
        owner = weakref.ref(self)

        def notify(changes: n.ChangeSet) -> None:
            project = owner()
            if project is not None and token in project._observers:
                project._observers[token](changes)

        token = self._document.subscribe(notify)
        self._observers[token] = observer
        return token

    def unsubscribe(self, token: int) -> None:
        self._document.unsubscribe(token)
        self._observers.pop(token, None)

    @property
    def observer_errors(self) -> tuple[str, ...]:
        return tuple(self._document.observer_errors)

    def __repr__(self) -> str:
        snapshot = self._snapshot()
        return f"Project(name={snapshot.name!r}, revision={snapshot.revision})"


def load_project(path: str | PathLike[str]) -> Project:
    return Project.open(path)


def _data(project: Project | n.Project) -> n.Project:
    return project._calculation_data() if isinstance(project, Project) else require(project, n.Project, "project")


def estimate_report_rows(project: Project | n.Project, definition: n.ReportDefinition) -> int:
    return n.estimate_report_rows(_data(project), definition)


def report_input_sha256(project: Project | n.Project) -> str:
    return n.report_input_sha256(_data(project))


def generate_report(project: Project | n.Project, definition: n.ReportDefinition,
                    eop_manifest: str | PathLike[str] | None = None,
                    time_file: str | PathLike[str] | None = None) -> n.ReportResult:
    return n.generate_report(_data(project), definition, eop_manifest, time_file)


class AccessSession(n.AccessSession):
    def __init__(self, project: Project | n.Project, station_id: str, target_id: str,
                 eop_manifest: str | PathLike[str], time_file: str | PathLike[str] | None = None) -> None:
        super().__init__(_data(project), station_id, target_id, eop_manifest, time_file)
