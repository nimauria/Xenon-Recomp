# Xenon XEX Loader

> **Superseded by `docs/xbox/XEX_LOADER_V2.md`.** This document describes the
> original metadata-first loader (MZ-scanning, no encryption/compression, a
> byte-diff placeholder for title updates). It is kept for history; the
> production retail loader is XEX Loader V2.

## Purpose

The Xenon XEX loader is the first-party executable-loading subsystem for Xbox 360 XEX images. It owns XEX parsing, PE extraction, section mapping, metadata exposure, and title-update composition without delegating Xbox executable semantics to Project Gracemeria.

## Scope

This subsystem is intentionally metadata-first and runtime-friendly:

- Detect XEX1 and XEX2 payloads.
- Parse header and optional records.
- Extract PE metadata and section layout.
- Preserve XEX execution metadata (entry point, module flags, title/media IDs, region, TLS, and imports/exports).
- Parse PE exception/function records when present into validated function ranges for static recompilation.
- Build a stable `XexImage`/`LoadedXex` result for the rest of the recompilation pipeline.
- Map XEX image sections into Memory V2 using the existing Xenon address-space contracts.
- Support deterministic title-update composition while keeping the original source images immutable.

## Design goals

- Xenon owns XEX behavior.
- No separate XEX memory model is introduced.
- The runtime uses Memory V2 reservations, commits, and executable-page generation.
- Import metadata is exposed structurally and passed to the unified runtime/export resolver instead of being resolved immediately in the parser.
- Title updates are applied as a base-image-plus-patch composition rather than modifying the original payload.

## Loader API

The public API is defined in `include/xenon/xbox/xex_loader.hpp` and centers on:

- `detect_xex_format()`
- `parse_xex_image()`
- `load_xex()`
- `apply_title_update()`

The result objects are:

- `XexImage`: parsed metadata, section list, imports, exports, TLS, and effective image bytes.
- `LoadedXex`: mapped XEX data in Memory V2, including executable ranges and the loaded image description.

## XEX image pipeline

1. Validate the XEX header and optional header count.
2. Extract the XEX identity and execution metadata.
3. Discover the PE image within the XEX payload.
4. Parse PE section headers and derive memory protections.
5. Record imports, exports, TLS, and title identity.
6. Map the image into Memory V2 using the Xenon guest address-space helper APIs.
7. Surface executable ranges and entry-point metadata to the compiler/recompilation pipeline.

## Memory V2 integration

The loader maps sections directly into the guest address-space facilities that already recognize the Xenon XEX region layout. Regions are reserved and committed with the corresponding protections, and sections that are executable are exposed in the `LoadedXex.executable_ranges` view for CPU V2 executable generation.

## Title updates

Title-update payloads are composed by applying the patch to an in-memory copy of the base image while leaving the original input bytes untouched. This keeps the base executable immutable and allows deterministic reconstitution of the effective XEX image.

## Validation

The focused validation is in `tests/xbox/xex_loader_tests.cpp` and covers:

- malformed XEX rejection
- invalid header bounds
- valid parse/load behavior
- PE section mapping into Memory V2
- title-update application semantics

## Notes

This is a foundational XEX loading subsystem aligned with Xenon's native recompilation model. It intentionally keeps the scope focused on import/export metadata extraction, PE section mapping, and deterministic title-update composition while leaving deeper runtime resolution to later integration work.
