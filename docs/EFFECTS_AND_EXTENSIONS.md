# Effects and extensions

## Panel-based effect stack

Effects Settings is a direct representation of the layer effect stack. Each effect is a collapsible panel. Panel order is render order. Dragging a header reorders the underlying model; the header switch enables/disables the effect; the overflow menu provides Duplicate, Delete, Move Up, and Move Down.

The bottom toolbar contains **Add Effect** and **Respect Masks**. Effect settings are bound to their own effect instance so several expanded panels can remain visible without controls writing into the wrong stack entry.

## Built-in effects and presets

Built-in effects use stable namespaced IDs (`bgl.builtin.*`) while retaining legacy numeric adapters for older project files. Presets and Add Effect menus are generated from the shared catalog. Built-ins include color/generate effects, blur families, shadows, glow, outline, grading, noise, vignette, emboss, lens flare, motion blur, and generated gradients.

Effect parameters may be static or animated. Position-like parameters can expose canvas handles, and keyframe diamonds use the same animation model as ordinary layer properties.

## Transition presets

Layer transitions are stored separately from effects but use the same preset/data packaging area. Text and general transitions can expose host-owned controls and previews without embedding third-party Qt widgets.

## Manifest extensions

Portable GPU extensions live under `broadcast-graphics-live/extensions/` and contain a `*.bgl-effect.json` manifest plus an OBS `.effect` shader. The manifest declares:

- globally unique effect ID;
- display/category/provider metadata;
- parameter schema and defaults;
- supported shader techniques;
- editor schema, presets, and assets where applicable;
- capability flags for keyframes and compound graphs.

Supported parameter concepts include float, integer, boolean, color, enum, texture, point, and string. Animatable parameters are evaluated by the host so timeline behavior remains consistent across built-in and external effects.

## Native extension ABI

Optional native extensions are loaded only from the user configuration extension directory. They expose the stable C ABI declared in `src/extensions/bgl-plugin-api.h` and must not depend on private Qt/C++ editor headers.

Compatibility rules:

- IDs are permanent and namespaced.
- API/ABI mismatches are rejected.
- Duplicate IDs are ignored and reported.
- Unknown extension IDs and raw parameter JSON round-trip without data loss.
- Missing extensions do not destroy project settings.
- Native extensions execute inside the OBS process and should only be installed from trusted sources.

## Compound graphs

API v2 can flatten ordered graph elements to bounded shader uniforms (`elementCount`, `element0_*` through `element15_*`). Animation paths use the same element/property names, allowing compound effect elements to be keyframed without extension-specific timeline code.
