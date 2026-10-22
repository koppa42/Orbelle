"""Reusable report presentation; applying a template never recalculates a query."""
from dataclasses import dataclass
from itertools import islice
from typing import Iterable

from . import _native as n

ALL = n.ReportScope.ALL_COMPATIBLE


@dataclass(frozen=True)
class Column:
    field: str
    unit: str | None = None
    title: str | None = None

    def __post_init__(self) -> None:
        if not isinstance(self.field, str) or not self.field:
            raise TypeError("field must be a nonempty string")
        for name in ("unit", "title"):
            value = getattr(self, name)
            if value is not None and not isinstance(value, str):
                raise TypeError(f"{name} must be a string or None")


@dataclass(frozen=True, init=False)
class Template:
    columns: tuple[Column, ...] | None
    units: n.ReportUnits
    time_scale: n.ReportTimeScale

    def __init__(self, columns: Iterable[str | Column] | None = None, *,
                 units: n.ReportUnits = n.ReportUnits.ENGINEERING,
                 time_scale: n.ReportTimeScale = n.ReportTimeScale.TAI) -> None:
        if not isinstance(units, n.ReportUnits):
            raise TypeError("units must be ReportUnits")
        if not isinstance(time_scale, n.ReportTimeScale):
            raise TypeError("time_scale must be ReportTimeScale")
        selected = None
        if columns is not None:
            if isinstance(columns, str):
                raise TypeError("columns must be an iterable of fields, not a string")
            selected = tuple(Column(c) if isinstance(c, str) else c for c in islice(columns, 65))
            if not selected or len(selected) > 64:
                raise ValueError("Select 1–64 columns, or omit columns for the full schema")
            if any(not isinstance(c, Column) for c in selected):
                raise TypeError("columns must contain field strings or Column values")
            if len({c.field for c in selected}) != len(selected):
                raise ValueError("Duplicate report field")
        object.__setattr__(self, "columns", selected)
        object.__setattr__(self, "units", units)
        object.__setattr__(self, "time_scale", time_scale)

    def _to_native(self) -> n.ReportTemplate:
        value = n.ReportTemplate()
        value.units, value.time_scale = self.units, self.time_scale
        value.columns = [n.ReportColumnSpec(c.field, c.unit or "", c.title or "")
                         for c in self.columns or ()]
        return value

    def apply(self, result: n.ReportResult) -> n.ReportResult:
        """Return a new view of the captured SI data, preserving provenance."""
        if not isinstance(result, n.ReportResult):
            raise TypeError("result must be a ReportResult")
        return result.with_template(self._to_native())


def fields(kind: n.ReportKind) -> tuple[n.ReportColumn, ...]:
    """Available field IDs, default titles and SI dimensions for a report kind."""
    return tuple(n.report_columns(kind))
