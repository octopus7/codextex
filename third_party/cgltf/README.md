# cgltf

Vendored unmodified `cgltf.h` and MIT `LICENSE` from
https://github.com/jkuhlmann/cgltf at commit
`85cd62382dfea638278962690cf515023f33ed00` (retrieved 2026-09-07).

The implementation is compiled once in `src/core/GlbImporter.cpp`.
CodexTex performs its own bounded, Unicode-aware resource loading and validates
accessor byte ranges before calling the library's validation/reading utilities.
Sparse attribute values are read with their tightly packed stride, including
when the base accessor is interleaved.
