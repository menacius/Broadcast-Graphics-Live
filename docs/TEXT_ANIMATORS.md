# Text Animators

Text Animators are the renderer-neutral text-animation model used by Broadcast Graphics Live. A text layer owns an ordered stack of animators. Each animator combines editable animated properties with one or more selectors and is evaluated against the immutable shaped text layout shared by the editor and the OBS source renderer.

## Model and stack order

A text animator contains:

- a stable identifier, editable name, enable state, and expanded state;
- an Add, Replace, or Multiply composition mode;
- a default granularity;
- a text-change policy;
- animated properties;
- ordered selectors;
- an optional built-in/custom preset identifier and schema version.

Animators are evaluated from top to bottom. Multiple animators may affect the same shaped cluster. Selectors are evaluated in their visible order and combine with Add, Subtract, Intersect, Difference, Minimum, Maximum, or Multiply. The resulting influence is applied to every enabled property in the animator.

The Properties panel and timeline do not hold separate copies. Both expose the same `AnimatedProperty` objects, including static values, keyframes, interpolation modes, easing, and manual temporal handles.

## Text units and Unicode

Selection is resolved from the shaped `TextLayoutData`, not from UTF-8 bytes or UTF-16 code units. The unit map supports:

- grapheme clusters;
- visible characters;
- characters excluding spaces;
- words;
- shaped lines;
- paragraphs;
- rich-text paint runs;
- the complete text layer;
- legacy-compatible shaped sentences.

Cluster mappings retain canonical UTF-8 byte ranges for exact-text, regular-expression, external-data, newly-added, and changed-text selectors. Emoji ZWJ sequences, combining marks, Greek, RTL runs, and other complex shaped clusters remain indivisible when the shaping pipeline reports them as one cluster.

## Properties

The model stores transform, character-formatting, paragraph/layout, and visual properties. Transform-only properties are applied to batched glyph geometry without reshaping. Fill/stroke colour, opacity, reveal, SDF blur, and stroke-width changes are carried in the same glyph vertex batches.

Properties that can change line breaking or glyph placement are explicitly marked layout-affecting. Tracking currently uses a shaped-line post-pass when it can preserve the existing layout. Font-size and scale changes can use glyph-quad scaling when a full relayout is not required. Future layout-path work must continue to use the same animator model rather than introducing a second implementation.

## Selectors

### Range Selector

Range selectors support percentage/index units, Start, End, Offset, Amount, Square/Ramp/Triangle/Round/Smooth shapes, Ease High/Low, Smoothness, deterministic random order, seed, direction, and inversion. Meaningful numeric fields are ordinary timeline properties.

### Procedural Selector

Procedural modes include Random, Noise, Wave, Sine, Sawtooth, Pulse, Alternating, distances from start/end/centre, and distance from a custom index. Seeded modes are deterministic during playback, scrubbing, caching, and prerendering.

### Text-based Selector

Text-based selection supports character/word/line/paragraph ranges, exact text, regular expressions, whitespace, numbers, uppercase, lowercase, punctuation, rich-text runs, external-data byte ranges, newly-added text, and changed text.

### Wiggly Selector

The Wiggly selector uses amount, frequency, correlation, temporal/spatial phase, seed, minimum/maximum influence, dimension locking, and optional per-character values. A supplied seed produces repeatable output.

### Staggered Selector

The Staggered selector is the generic selector used by the historical BGL text transitions. It stores a keyframeable Completion track, stagger percentage, entrance/exit mode, unit easing, order/direction, deterministic random seed, and whitespace policy. Its evaluation reproduces the historical two-stage easing contract: the authored completion curve is evaluated first, followed by the same per-unit easing after the stagger delay. It is not a preset-specific renderer and can be edited or reused by custom animators.

## Timeline and keyframes

Animator and selector properties are published through `TimelinePropertyRef`. They therefore use the existing timeline implementation for:

- Linear, Hold, and Bezier interpolation;
- manual temporal velocity/influence handles;
- Value and Speed Graph views;
- copy/paste and multi-keyframe selection;
- Easy Ease commands;
- undo/redo and property reset;
- frame-accurate evaluation at project time.

Timeline labels use the form `Text › <Animator> › <Property or Selector> › <Component>`.

## Presets

A preset is serialized generic animator data, never a renderer command. Applying a preset creates editable properties, selectors, and keyframes.

Standalone presets use the `.obgtextanim` extension. They contain metadata, schema version, the complete animator, selector configuration, seeds, and all temporal keyframe fields. Imported structures receive fresh stable IDs so they cannot collide with tracks already present on the layer.

### Legacy migration mapping

The legacy runtime presets present in development version 133 map as follows:

| Existing identifier | New properties | New selector |
| --- | --- | --- |
| `text.fade` | Opacity | Staggered Selector |
| `text.slide-in` | Position, Opacity | Staggered Selector |
| `text.scale` | Scale, Opacity | Staggered Selector + shared unit transform origin |
| `text.blur` | Blur, Opacity | Staggered Selector |
| `text.wipe` | Character Reveal, Opacity | Staggered Selector + directional shaped-unit clipping |
| `text.blur-slide-in` | Position, Blur, Opacity | Staggered Selector |

Legacy entrance/exit edge, duration, easing, direction, offset, scale-from, blur amount, character/word/sentence unit, order, and stagger are converted to layer-local animator data. The transition descriptor remains as authoring metadata so the existing timeline handles and Transition Editor keep working, but it is never executed by a renderer. A `transition_managed` animator bound by stable transition ID is the sole runtime implementation.

The binding stores a signature of the authored descriptor and its effective layer-local timing. Editing duration, direction, unit, easing, stagger, or edge timing rebuilds the bound animator; unrelated refresh, trim, save, or reload operations do not overwrite manual edits to its generated properties, selectors, or keyframes. Duplicating a managed animator detaches the copy as a normal custom animator. Deleting the managed animator also removes its authoring descriptor, preventing it from being recreated on reload.

Projects saved by intermediate Development Versions 134/135, which could contain generated animators without retained timeline descriptors, are detected and repaired deterministically. Converted projects save the current schema on the next save; legacy deserialization remains only as an import/conversion layer. General layer transitions remain untouched.

## Dynamic text, Live Text Cues, clocks, and external data

When source text changes, BGL compares shaped clusters, preserves stable prefix/suffix and LCS matches, and generates canonical byte ranges for additions and changes. Large strings use a bounded deterministic mapping instead of an unbounded quadratic diff.

Each animator can restart, continue local time, preserve completion, animate newly-added text, animate changed text, select removed text where retained geometry exists, or re-evaluate the complete text. Old cluster indices are never reused blindly after a new layout is created.

Auto Text Styling and rich-text shaping must finish before selector evaluation so style-run and line mappings match the rendered layout.

## Rendering, cache, and determinism

The shared evaluator produces one state per shaped cluster. The GPU renderer fans that state out to glyph quads and keeps batching by atlas/material page; it does not invoke one full text pass per character.

Wipe transitions clip glyph quads against shaped character/word/sentence bounds before applying the common transform. Word/sentence transforms can opt into a shared shaped-unit origin so Scale Text behaves like the historical grouped unit instead of shrinking every glyph around its own centre. Blur Text uses a bounded multi-sample SDF blur for fill and stroke, contracting to the sharp source as selector influence reaches zero.

When an exact glyph cannot use the SDF atlas (for example color-font emoji, failed alpha-map extraction, oversized glyphs, or atlas exhaustion), the Qt compatibility raster consumes the same immutable layout and `TextAnimatorEvaluation`. It isolates shaped clusters where practical and uses a bounded flattened fallback for very long content. No `LayerTransition`-specific compatibility renderer remains.

Text Animator signatures include animator order, properties, keyframes, selectors, seeds, preset schema, and migration version. Time-dependent animators participate in frame cache keys. Changes invalidate the affected text layer/title path rather than unrelated titles, and migrated legacy cache output is incompatible with the new signature.

Static text layers bypass animator evaluation and keep the existing shaping/rendering path.

## Diagnostics

Migration logs identify the layer and preset conversion. Any unsupported legacy field emits one descriptive fallback warning with the project/title, layer, preset, parameter, and deterministic replacement behavior. Dynamic text remapping and cache invalidation are logged at event boundaries, never once per glyph or frame.

## Validation

The source tree contains deterministic tests for shaped unit maps, Greek and combining text, emoji ZWJ clusters, RTL runs, selector composition, seeded procedural output, multiple animators, legacy mapping, local timing, content-length changes, cache signatures, timeline discovery, and a 1,200-cluster/10-animator stress case. Full pixel-equivalence validation still requires a matching OBS/libobs/Qt runtime on Windows and Linux.

## Development Version 138 glyph-envelope and blur parity correction

Development Version 138 fixes two visual regressions introduced when transition-managed layers were routed through the conservative flattened compositor in version 137. Unified text transitions again use the isolated shaped-unit compositor first. Unit images are derived from actual alpha/ink bounds, keep transparent interpolation gutters, and render inside a font-, rich-text-, stroke-, and antialias-aware surface envelope. The flattened compositor remains only as a guarded fallback.

Text Animator blur no longer owns a transition-specific kernel. Blur Text and Blur Slide call the same premultiplied-pixel backend, blur-pass mapping, and support-radius calculation as the standard BGL Blur effect. This preserves the established radius-driven behavior and keeps editor/source/cache output on one implementation. The renderer cache ABI is advanced so frames created before this correction are invalidated.

## Development Version 137 runtime correction

Development Version 137 activates the unified text-transition runtime in the real OBS/editor source path. It repairs the split `.inc` translation-unit boundary left by version 136, reconstructs managed animators from authoritative transition descriptors at render time, and sends transition-managed text through the generic raster compositor until the per-glyph GPU path has completed visual validation. Both render routes consume the same shaped layout and `TextAnimatorEvaluation`; no legacy descriptor-specific evaluator is used.

## Development Version 136 status

Development Version 136 completes the migration and runtime parity of the six historical BGL text-transition types: Fade Text, Slide In/Out, Scale Text, Blur Text, Wipe Text, and Blur Slide In/Out. Their existing authoring surfaces, edge handles, duration editing, units, ordering, timing, easing, preview, cache participation, and save/reload behavior are retained, while output is evaluated exclusively by `TextAnimatorStack`.

The following broader Text Animator specification work remains outside this legacy-transition milestone:

- true relayout for every layout-affecting animator property, including animated font size, word/line/paragraph spacing, and wrapping changes;
- complete generic effects-stack animation for glow, shadow, and motion-blur sampling beyond the transition properties used by the historical presets;
- complete typewriter cursor/audio events, removed-character retained geometry, character replacement, and scramble rendering;
- the expanded entrance, exit, continuous, ticker, clock, and broadcast preset library with generated thumbnails, category management, search, restore, and visual fixtures;
- pixel-comparison fixtures and full Windows/Linux OBS/libobs/Qt editor/source/cache validation;
- final profiling of high-DPI output, multiple animated layers, external-data churn, masks/mattes, group transforms, and motion blur.

All future work must extend the shared animator, selector, shaped-layout, and renderer paths rather than restoring descriptor-specific transition execution.


## Development Version 139 compile correction

The glyph-envelope height hint introduced in version 138 now reads font size and vertical scale from effective `RichTextCharFormat` values at rich-text range boundaries. `TextLayoutPaintStyle` remains paint-only, matching the immutable text-layout architecture and fixing the MSVC compile failure without weakening animation overscan or mixed-style glyph protection.
