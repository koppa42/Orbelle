# Vallado SGP4

`SGP4.cpp` and `SGP4.h` are unmodified bytes from `extension/` in
[brandon-rhodes/python-sgp4](https://github.com/brandon-rhodes/python-sgp4/tree/8126f773923c2902719d45aab8bdb5cce86cf39d),
commit `8126f773923c2902719d45aab8bdb5cce86cf39d` (Vallado 2023-05-09 release).
The repository's MIT `LICENSE` is included. Keep the upstream header comments.

Only `sgp4init` and `sgp4` are called. Orbelle provides its own bounded TLE parser;
the upstream interactive/file I/O helpers are not used. Compiler warnings are disabled
for this source only. Updates must review the upstream license, rerun the pinned
verification cases, and revise algorithm provenance if behavior changes.

Private compile definitions prefix the upstream namespace and two unused debug globals
to avoid symbol collisions with another SGP4 in a C++ SDK consumer. SHA-256 of the original bytes:

- `SGP4.cpp`: `a96940bb6533f80f7aa472d9223292fab61f4e5df356f81b560f15aec3cac122`
- `SGP4.h`: `2a5ec44e059a52b3173d78d9a28bda8142b4f6497c1eb16febc2cea4b5006b0c`

The implementation is compiled into the private Sgp4 provider with no runtime downloads.
Notices are included in the C++ SDK, Python wheel, and Windows portable package.
