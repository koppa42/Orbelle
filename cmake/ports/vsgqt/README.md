# vsgQt release metadata overlay

Based on the fixed registry's vsgQt 0.4.0#1 port, using the same `v0.4.0`
source archive and SHA512. The tagged source still declares `project(VERSION
0.3.0)`, so its installed CMake config rejects an exact 0.4.0 request.

Port 0.4.0#2 changes that release metadata to 0.4.0. The upstream SOVERSION 3
is retained; the patch does not claim a new ABI or change rendering behavior.
Qt6, dependency discovery, example exclusion and license installation follow
the fixed registry port. Compatibility is verified by OrbitForge's graphics
build and runtime checks.
