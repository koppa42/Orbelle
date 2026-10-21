"""Process support for desktop validation on a developer machine.

Portable-package checks intentionally build their own isolated environment.
Correctness and timing callers must select validation explicitly.
"""
from __future__ import annotations

from contextlib import contextmanager
import os
from pathlib import Path
import subprocess
import time


def desktop_environment(prefix: Path, *, scale: float = 1, validation: bool = False,
                        environ=None) -> dict[str, str]:
    if not 0.5 <= scale <= 4:
        raise ValueError("Scale must be in [0.5, 4]")
    environment = dict(os.environ if environ is None else environ)
    environment.update(
        PATH=str(prefix / "bin") + os.pathsep + environment.get("PATH", ""),
        QT_QPA_PLATFORM_PLUGIN_PATH=str(prefix / "Qt6/plugins/platforms"),
        QT_PLUGIN_PATH=str(prefix / "Qt6/plugins"), QT_QPA_PLATFORM="windows",
        QT_SCALE_FACTOR=f"{scale:g}", VK_LAYER_PATH=str(prefix / "bin"),
        PROJ_DATA=str(prefix / "share/proj"), GDAL_DATA=str(prefix / "share/gdal"),
        PROJ_NETWORK="OFF")
    settings = ("VK_LAYER_VALIDATE_SYNC", "VK_LAYER_VALIDATE_CORE",
                "VK_LAYER_OBJECT_LIFETIME", "VK_LAYER_THREAD_SAFETY")
    for name in ("VK_INSTANCE_LAYERS", "VK_LOADER_LAYERS_ENABLE", "VK_LOADER_LAYERS_DISABLE",
                 "VK_LAYER_ENABLES", "VK_LAYER_DISABLES", *settings):
        environment.pop(name, None)
    # Supported by the pinned VVL; do not inherit deprecated enable/disable lists.
    if validation:
        environment.update({name: "1" for name in settings})
    return environment


def stop_process(process: subprocess.Popen, timeout: float = 10) -> None:
    """Reap only a process owned by this caller, even if graceful termination fails."""
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


@contextmanager
def logged_process(command, *, cwd, env, directory: Path, stem: str = ""):
    """Keep complete logs on success, failure and timeout; always reap the child."""
    with (directory / f"{stem}stdout.log").open("wb") as stdout, \
            (directory / f"{stem}stderr.log").open("wb") as stderr:
        process = subprocess.Popen(command, cwd=cwd, env=env, stdout=stdout, stderr=stderr)
        try:
            yield process
        finally:
            stop_process(process)


def run_logged(command, *, cwd, env, directory: Path, timeout: float, stem: str = ""):
    with logged_process(command, cwd=cwd, env=env, directory=directory, stem=stem) as process:
        return subprocess.CompletedProcess(command, process.wait(timeout=timeout))


def timeout_message(error: subprocess.TimeoutExpired) -> str:
    return f"External {error.timeout:g}-second timeout"


def wait_until(predicate, process, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    while not predicate():
        if process.poll() is not None:
            raise RuntimeError(f"Process {process.pid} exited early with {process.returncode}")
        if time.monotonic() >= deadline:
            raise TimeoutError("Timed out waiting for capture readiness")
        time.sleep(0.1)
