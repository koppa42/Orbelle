# CSPICE overlay

Based on the `cspice` 67#4 port at the repository's pinned vcpkg baseline.
The original platform archive hashes, source build rules, license and `isatty`
patch are retained. Orbelle requires static linking, position independent
code and hidden C symbol visibility so the Python extension does not share a
process-global CSPICE instance with unrelated extensions through exported C
symbols. The project wrapper serializes access to its instance.

An SDK host must route all calls to that instance through the same Orbelle
wrapper; directly calling CSPICE against it is outside the supported contract.
