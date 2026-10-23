# Native Windows update handoff

The root `Orbelle.exe` and versioned `OrbelleUpdater.exe` use Win32 and the static
MSVC runtime. They do not load Qt, renderer libraries, or scientific data.

Protocol 1 installs one complete payload in `versions/X.Y.Z/`, containing
`OrbelleApp.exe`, `OrbelleUpdater.exe`, `OrbelleLauncher.exe`, its DLLs, plugins, and resources. The root
contains `orbelle-layout.txt` (`1\n`), `active-version.txt` (`X.Y.Z\n`), and an empty
`.orbelle-running.lock`. The launcher holds that lock exclusively until its child
exits and forwards every argument and the exit code. The pre-created lock permits
ordinary launches from a read-only installation. An update requires write access.

After the application has downloaded and verified a complete new payload, launch
the **new version's** helper:

```text
versions/NEW/OrbelleUpdater.exe --apply-update ABSOLUTE_ROOT NEW PARENT_PID --parent-ready-event EVENT
```

`EVENT` must start with `Local\Orbelle.UpdateParent.`. The caller creates manual-reset
events named `EVENT` and `EVENT.cancel`. The helper validates its installation and
parent image, opens and holds the parent process handle and both events, then sets
`EVENT`. The caller accepts its normal close only after this acknowledgement. If
handoff fails, the caller signals `EVENT.cancel` and keeps the desktop open. The
helper then exits without changing anything. Calls without this optional handshake
are supported for external supervised use, but callers must keep the parent alive
until the helper has opened it.

The helper waits for normal parent exit, obtains the installation run lock, then
starts the candidate with `--update-ready-event Local\Orbelle.Ready.PID.NONCE`.
The candidate opens that event with `EVENT_MODIFY_STATE` and signals it only when
startup has reached a visible, usable window. A visible first-run data setup window
may count as ready so a long scientific-data download is not mistaken for a failed
program launch. The helper waits up to 120 seconds. A candidate exiting before the
signal leaves the old pointer intact and reopens the previous program. If a live
candidate times out, the helper requests a normal window close and waits; it never
forcibly terminates a desktop process. After readiness, the helper copies the new
`OrbelleLauncher.exe` into a held temporary file, flushes it, and atomically replaces
the root launcher by file handle. The active-version pointer is published only
after the launcher succeeds. Protocol-1 launchers must also support earlier
protocol-1 payloads, so failure to publish the pointer still permits the old program
to run with the new launcher. A locked launcher or pointer aborts the switch without
cleanup. The helper continues holding the run lock for the new desktop.

After a successful switch, old payloads can be removed only if they contain:

- `orbelle-managed-files.txt`: UTF-8 lines `sha256` + two spaces + relative path + LF,
  listing every payload file except this list and its marker.
- `orbelle-managed.txt`: `1\n` + the list's lowercase SHA-256 + `\n`.
- A listed `manifest.json`, `OrbelleApp.exe`, `OrbelleUpdater.exe`, and `OrbelleLauncher.exe`.

Cleanup checks the entire tree and every file hash before deletion. Unexpected
files, unexpected empty directories, changed files, reparse points, hard links,
the helper's working directory, or `ORBELLE_DATA_HOME` inside an old version cause
that version to be preserved. Files are opened against replacement and rehashed
before file-level deletion; only empty directories are removed. Scientific data
and user projects are never recursively removed. Failure to reclaim disk space
does not roll back a working update. After successful cleanup, reverting requires
downloading the old version again.

This protocol does not make scientific data schema changes reversible. It never
opens or edits the user's data catalog. The desktop must maintain its own data
compatibility contract when deciding what constitutes successful startup.

`tests/native_updater_test.py` launches an isolated native stub and exercises the
real process/lock/event/file paths, including pre-ready failure, cancellation,
argument quoting, and conservative cleanup. Run through CTest with
`desktop.NativeUpdater`; no Qt or GPU is required.

Native dialog text is generated at CMake configure time from the shared desktop
`resources/text/zh_CN.json` catalogue. The launchers embed these four messages and
do not need Qt, a runtime catalogue file, or Python to show diagnostics.
