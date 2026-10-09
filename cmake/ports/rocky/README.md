# Rocky overlay

Pins the upstream 1.1.0 release commit
`59d953669485b07c31e8207780a89a90973044bf` and its source archive SHA512.
The main vcpkg registry has no Rocky port at OrbitForge's fixed baseline.

The port builds the VSG renderer, PROJ, vsgXchange with FreeType and optional
GDAL for local raster files. It disables upstream demos/tests and optional
HTTP, hosted map services, MBTiles, ImGui and Qt demos. OrbitForge supplies its
own Qt integration and explicitly selected local display data. These options
do not claim to sandbox arbitrary PROJ/GDAL operations.

Linkage follows the triplet. Shaders and data retain their upstream relative
layout in `share/rocky`. The original MIT license is installed. Compatibility
and runtime validation are recorded separately under `docs/validation/`.

`optional-imgui.patch` fixes the upstream `ROCKY_SUPPORTS_IMGUI=OFF` build:
guard the ImGui integration include and install overloads, label-system registration and implementation
with the same `ROCKY_HAS_IMGUI` switch already used by the public label header.
The patch leaves the enabled-ImGui path unchanged; OrbitForge does not use those
widgets or labels in this integration.

`update-synchronization.patch` fixes two concurrent reads in `VSGContextImpl::update`:
the one-shot callback queue and pending compile result must be read under their
respective writer mutexes. Callbacks run after releasing the queue mutex so they
can enqueue subsequent work. Compilation results retain the upstream update/reset
locking scope, with the availability check moved inside that scope.
