## Development Version 144 — v0.8.8-alpha and Character panel cleanup

- Bumped the public plugin version from `v0.8.7-alpha` to `v0.8.8-alpha` across CMake, compile-time/runtime metadata, vcpkg metadata, installation/package examples, tests, audits, and canonical documentation.
- Removed the Font and Style labels from the Character properties section and made both combo boxes span the complete available panel width, preserving expansion under narrow and wide inspector layouts.
- Added consolidated README release notes covering the major changes completed since `v0.8.7-alpha` Development Version 107.

## Development Version 143 — Keyframe authoring, Graph Editor modifiers, and matte-column cleanup

- Removed the obsolete legacy Easing submenu from keyframe context menus. Temporal Interpolation is now the sole authoring UI and is also available directly from animated-property rows in the layer list, including Linear, Hold, Auto/Continuous/Manual Bezier, Easy Ease variants, and Keyframe Velocity.
- Made layer-list keyframe controls playhead-aware: diamonds indicate whether the current frame contains a key, numeric fields display evaluated animated values, edits write at the active playhead, and fields use the plugin's normal palette, border, hover, focus, selection, and disabled states.
- Added live Graph Editor drag modifiers: Shift constrains keyframes, handles, and panning to the dominant axis; Ctrl/Command snaps keyframe time/value and handle influence/speed; Alt continues to break linked temporal handles.
- Replaced the Graph Editor toolbar glyph with the supplied `graph.svg`, normalized to `currentColor`, and restored a visible, interactive time ruler and playhead marker above the graph.
- Renamed the layer-list Matte header to Matte Source and consolidated matte source/destination role indicators into one column headed by the matte-destination glyph.

## Development Version 142 — Synchronized keyframe sections and layer-list icon refresh

- Unified layer-list and timeline keyframe-section expansion behind one shared predicate, including the three-state Group caret. Animated property rows now appear and disappear in both panels together.
- Removed the timeline-only aggregate keyframe markers and hit targets from collapsed layer strips, eliminating the state where keyframes were visible in the timeline while their layer-list section was closed.
- Replaced the layer-list lock, unlock, hidden-visibility, matte-only, matte-source, and matte-destination artwork with the supplied SVG geometry. All supplied glyphs use `currentColor` and continue through the OBS-theme-aware icon renderer.

## Development Version 141 — Timeline strip, transition, ruler, and layer-state fixes

- Enlarged the visible layer-strip trim handles and widened their mouse hit zones, with outer strip edges taking priority over transition overlays so in/out points remain easy to resize when transitions are present.
- Made the complete area of an existing transition a valid drag-and-drop replacement target; replacement presets preserve the previous transition duration, subject only to the layer and opposite-edge limits.
- Made text-transition deletion immediately remove its managed Text Animator and generated keyframes, including stale timeline keyframe selections.
- Repainted the complete ruler band during playhead movement so time labels no longer lose their right edge for a frame as the playhead crosses them.
- Replaced normal, hidden, and matte-only layer visibility glyphs with the supplied SVG artwork and normalized it to `currentColor` for the active OBS theme. Existing lock/unlock glyphs remain routed through the same theme-aware renderer.

## Development Version 140 — Editor graph, timeline layout, and theme-aware icon fixes

- Added the supplied graph icon to the checkable Graph Editor button and made Value/Speed Graph, Fit Graphs, and Fit Selection controls visible only while Graph Editor mode is active.
- Restored shared playhead interaction in Graph Editor mode: ruler clicks/drags, loop/pause marker drags, and direct playhead-line dragging are no longer swallowed by graph hover or release handlers.
- Matched sidebar flyout long-press timing to the editor Fit control and made flyouts open horizontally toward the available side of the active screen rather than below the tool icon.
- Enforced a safe minimum layer-list width, synchronized header/row column widths, and reserved a usable minimum Layer Name column so Mode, Parent, Mask, and matte controls cannot overlap.
- Reorganized the timeline ruler into separate label, tick, and cache bands so cache progress no longer clips ruler labels and loop/pause markers remain orderly.
- Replaced the distribute, flip, matte, and Graph Editor artwork with the supplied SVGs. All supplied glyphs use `currentColor` and are rendered through the OBS-theme-aware icon pipeline.

## Development Version 139 — Text-transition glyph-envelope compile fix

- Fixed the Windows/MSVC build failure in `max_rich_text_font_height_hint()`.
- Removed invalid access to `TextLayoutPaintStyle::font_size` and `TextLayoutPaintStyle::scale_y`; paint styles intentionally contain paint-only state.
- The animation-aware glyph envelope now resolves effective `RichTextCharFormat` values at canonical rich-text range boundaries, preserving mixed font sizes and vertical scales without duplicating shaping data into paint runs.
- Added a regression contract that rejects future paint/shaping model cross-access.

## Development Version 138 — Text-transition glyph bounds and integrated blur regression fix

- Restored the proven isolated shaped-unit compositor as the primary unified Text Animator raster path. The flattened compositor is now an emergency fallback only, preventing advance/layout rectangles from clipping italic overhangs, swashes, combining marks, strokes, and overlapping glyph ink.
- Cropped transition units from their actual alpha/ink envelope and retained a transparent resampling gutter around every crop so scale, slide, rotation, filtering, and blur cannot sample against a hard image edge.
- Expanded text-layer temporary surfaces and clips using font metrics, rich-text maximum font height, plain/rich stroke width, and antialiasing slack instead of fixed approximate padding.
- Moved Text Animator blur onto the same premultiplied-pixel blur backend, blur-type pass mapping, and support-radius calculation used by the built-in BGL Blur effect. The retired transition-only blur helper is no longer present.
- Preserved radius-driven blur semantics: a unit is rendered as the blurred representation while blur is active and resolves to the sharp representation at zero, avoiding a sharp core plus halo.
- Bumped the GPU/cache visual ABI so prerenders produced by the cropped/transition-only-blur implementation cannot be reused.
- Added structural regression coverage for primary-path ordering, alpha-bound crops, transparent gutters, animation-aware ink padding, shared blur backend usage, and removal of the old transition blur helper.

## Development Version 137 — Text-transition runtime activation fix

- Fixed the broken title-source include boundary introduced in Development Version 136: the orphaned tail of the retired legacy transition function was removed from `compatibility-layer-raster.inc`, so the unified renderer is emitted at valid file scope.
- Added runtime self-healing that resolves every serialized text-transition descriptor into its bound generic `TextAnimatorStack` before cache-key generation and rendering. Descriptor-only, stale, and intermediate 134/135 titles therefore animate without requiring an editor resave.
- Routed transition-managed text through the conservative compatibility raster compositor while preserving the shared shaped layout and `TextAnimatorEvaluation`; this bypasses the unvalidated per-glyph GPU route without reviving any preset-specific transition evaluator.
- Made transition-managed raster composition use the already rendered canonical text surface first, preserving rich fill, stroke, shadow, background, emoji, and color-font appearance while applying per-unit opacity, transform, wipe, and blur.
- Added regression coverage for descriptor-only runtime recovery, include-module boundaries, managed-transition GPU fallback, and flattened-compositor routing.

## Development Version 136 — Complete unified legacy text transitions

- Restored the complete BGL text-transition authoring workflow while keeping `TextAnimatorStack` as the only runtime evaluator. Text-transition descriptors remain timeline/Transition Editor metadata and are bound to editable managed animators.
- Added the generic `Staggered` selector with exact historical two-stage easing, entrance/exit timing, reverse order, character/word/sentence units, Hold behavior, and frame-accurate layer-local keyframes.
- Converted Fade Text, Slide In/Out, Scale Text, Blur Text, Wipe Text, and Blur Slide In/Out into editable unified animators without deleting their timeline handles.
- Added stable transition binding signatures so duration/direction/editor changes update the managed animator, while unrelated refreshes and saves preserve manual animator/property/selector/keyframe edits.
- Added live synchronization while dragging transition duration and trimming layer edges.
- Restored word/sentence Scale Text behavior through generic shared-unit transform origins instead of scaling each glyph independently.
- Added directional shaped-unit clipping for Wipe Text in the common GPU glyph compositor.
- Replaced SDF edge softening with a multi-sample contracting glyph/stroke blur so Blur Text and Blur Slide no longer render as a sharp core with a glow-like halo.
- Added a generic Qt compatibility-raster adapter driven by the same shaped layout and `TextAnimatorEvaluation`, covering color fonts, emoji, unsupported glyph alpha maps, oversized atlas glyphs, and bounded long-text fallback without reviving legacy preset-specific rendering.
- Updated Transition Editor text previews to create and evaluate the same managed animator model used by editor/source output rather than duplicating stagger/easing formulas.
- Added automatic recovery of timeline transition descriptors from intermediate Development Versions 134/135 and deterministic upgrade to the current managed binding schema.
- Fixed duplicate/delete/enable lifecycle behavior for managed transition animators and added regression coverage for runtime retirement, preview parity, fallback rendering, synchronization, migration, Unicode units, cache signatures, and 1,200-cluster stress.

## Development Version 135

- Fixed the `PropertiesPanel` implementation-module order so the Text Animator member definitions are emitted at file scope instead of interrupting the constructor.
- Fixed GPU ticker Text Animator preparation to use the render session title and cue state when sampling `ticker_runtime`.
- Added `DOWNLOAD_EXTRACT_TIMESTAMP TRUE` to the nlohmann/json `FetchContent_Declare` call to remove CMake CMP0135 warnings.
- Synchronized CMake and runtime development-version metadata.

# Development version 134 — Unified Text Animator Core and Legacy Preset Migration

- Introduced a single shaped-layout-based Text Animator data/evaluation model shared by editor and source rendering.
- Added ordered animator stacks, generic properties, four selector families, selector composition, deterministic seeds, dynamic-text policies, serialization, cache signatures, and timeline discovery.
- Converted every legacy text preset identifier found in development version 133 to editable animator/property/selector/keyframe data and retained legacy loading only as a conversion layer.
- Added a Text Animators inspector and standalone `.obgtextanim` preset round-trip support.
- Added Unicode/dynamic text/migration/performance tests and `TEXT_ANIMATORS.md`.
- This revision is the shared-core integration milestone; full layout-animation, effects parity, expanded preset library, pixel fixtures, and Windows/Linux OBS runtime validation remain explicitly tracked in `TEXT_ANIMATORS.md`.

## Development Version 133 — Dock caret refinement and updated application icon

- Replaced the Titles and Graphics collapse/expand arrows with the same monochrome disclosure caret used elsewhere in the UI. The expanded header caret points down and the collapsed rail caret points right.
- Kept the docking-side-aware placement of the caret/button while leaving the disclosure direction consistent.
- Hid the compact-rail cache icon whenever aggregate cache/prerendering is disabled instead of showing a disabled cache glyph.
- Replaced the Broadcast Graphics Live application/window icon with the newly supplied brand artwork without redrawing or restyling it.
- Updated build metadata and package naming to Development Version 133.

## Development Version 132 — Collapsible Titles and Graphics Dock

- Added a header caret that collapses only the Titles and Graphics pane; Live Text Cues remains mounted and operational.
- Added a compact rail showing the Broadcast Graphics Live icon, selected-title active/inactive state, cue state, and aggregate cache/prerender status.
- Collapse is visibility/layout-only: the title list widget and model are never recreated, selection is preserved, and cue/playback/prerender state continues uninterrupted.
- Persisted collapsed state, expanded dock width, expanded splitter size, and last dock area. Carets follow left/right docking and use vertical direction while floating.
- Avoided dock-width changes during collapse to prevent OBS layout jumps; floating expansion restores the saved width.

## Development Version 123 — Structural & Invisible Character Recognition

- Extended learned regex inference to recognize paragraph/newline boundaries, CRLF, LF, CR, tabs, form feed, vertical tab, spaces, repeated spaces, and Unicode line/paragraph separators.
- Added exact structural escaping (`\x20`, `\t`, `\r\n`, etc.) instead of collapsing invisible characters into ambiguous whitespace matches.
- Added recognition for non-breaking and other Unicode invisible spacing characters while preserving their exact UTF-8 form.
- Prevented learned prefix rules from crossing line or paragraph boundaries.
- Fixed punctuation-followed-by-newline inference so the newline is not incorrectly absorbed into the punctuation delimiter.
- Added standalone regression coverage for Windows/Linux line endings, tabs, repeated spaces, NBSP, Greek/Unicode text, and U+2028 line separators.

## Development Version 122 — Learned Regex Auto-Formatting (Milestone 1)

- Added **Learn formatting from text** to Auto Styling Rules.
- Infers reusable regex rules from manually formatted canonical rich-text runs.
- Supports delimiter-based paragraph prefixes such as speaker names before `:`, `|`, `-`, `)` or `]`.
- Added regex full-match/capture-group evaluation with fail-closed invalid-pattern handling.
- Learned rules preserve their inferred character-format mask without requiring a style preset.
- Persisted regex pattern, capture group and case-sensitivity in title JSON.
- Added Unicode/Greek regression coverage proving one formatted speaker prefix can style subsequent rows.

## Development Version 121 — Temporal Graph Editor and Manual Velocity Handles

- Added a dedicated Graph Editor mode to the timeline with switchable Value Graph and Speed Graph views, sub-frame curve sampling, final evaluator values, current-time indication, and correct layer-local-to-timeline time mapping.
- Added per-keyframe incoming/outgoing temporal influence and speed, linked/broken temporal tangent state, and Linear, Hold, Auto Bezier, Continuous Bezier, and Manual Bezier modes while preserving legacy segment easing until an explicit velocity edit is made.
- Added deterministic cubic temporal evaluation in real time/value space with Newton iteration plus bisection fallback. Time influences remain single-valued, while property values and speeds remain unclamped to support negative values and overshoot.
- Added direct incoming/outgoing velocity-handle dragging in Value and Speed graphs. Alt-drag breaks only the edited temporal pair; linked handles preserve paired speed/influence editing.
- Added Easy Ease, Easy Ease In, Easy Ease Out, and a numeric Keyframe Velocity dialog for mode, incoming/outgoing influence, and incoming/outgoing speed.
- Added marquee and Shift multi-keyframe selection, relative multi-edit for keyframe time/value and velocity deltas, graph zoom, pan, Fit Graphs, and Fit Selection in both axes.
- Added the temporal interpolation and velocity commands to the ordinary timeline keyframe context menu as well as the Graph Editor context menu.
- Routed scalar, vector Position, scalar-group, and numeric extension animation through the same temporal evaluator used by editor playback, OBS output, and prerender/cache rendering.
- Persisted temporal velocity metadata in title JSON, retained extension metadata during keyframe value updates, and included all temporal fields in cache fingerprints.
- Kept graph edits on the timeline's existing undo stack: drag operations commit once on release, while mode, Easy Ease, and numeric-dialog edits create immediate title snapshots.
- Preserved independently authored 0–100% incoming/outgoing influences without silent pair renormalization, keeping displayed graph handles identical to the temporal cubic used for final evaluation.
- Stabilized multi-keyframe crossing edits by preserving extension-track index identity during drag and remapping the selected keys only after the release-time sort. Legacy easing presets now explicitly exit velocity mode on native and extension tracks.
- Defined deterministic endpoint behavior even for sub-epsilon keyframe intervals and expanded finite-difference checks so the Speed Graph derivative agrees with the Value Graph curve.
- Expanded standalone animation tests and added a temporal Graph Editor source contract covering overshoot, negative values, very short intervals, UI interactions, persistence, cache invalidation, extension properties, and shared evaluation.

## Development Version 120 — On-Canvas Motion Paths

- Expanded the selected animated layer overlay into a directly editable final-space motion path with keyframe vertices, incoming/outgoing Bezier handles, current-position marker, and direction indication.
- Added direct vertex dragging and tangent dragging on the canvas. Vertex edits are inverse-mapped through the parent/group hierarchy into layer-local Position values; tangent edits remain layer-local and support Shift 45-degree constraints and Alt-drag break behavior for only the selected tangent pair.
- Added double-click path subdivision. Cubic segments use deterministic de Casteljau splitting so the inserted keyframe lies on the existing curve without moving the surrounding authored vertices.
- Added canvas and timeline context actions for Linear, Auto Bezier, Continuous Bezier, Manual Bezier, Rove Across Time, Break Tangents, and Join Tangents.
- Added deterministic roving-time redistribution for interior Position keyframes, persisted the rove flag in title JSON, retained it through keyframe copy/paste, and included it in cache fingerprints.
- Added motion-vertex snapping to user guides and other keyframe positions while excluding descendants when editing their parent/group, avoiding self-hierarchy snap feedback.
- Hid editable vertices/handles during inline text editing and on locked layers, while retaining a lightweight read-only path/current-position display.
- Kept hover highlighting overlay-only: handle/path hover invalidates the selection overlay rather than dirtying the title model or requesting a full rendered canvas frame.
- Added release-time undo snapshots for vertex/tangent drags and immediate snapshots for path insertion, interpolation mode, rove, and tangent break/join actions.
- Expanded animation and source-contract regression coverage for curve subdivision, stable keyframe vertices during mode changes, roving timing, transformed-space editing, snapping exclusions, and input wiring.

## Development Version 119 — Spatial Bezier Keyframes Core

- Separated temporal easing from spatial path interpolation for animated vector properties while retaining the existing temporal keyframe controls.
- Added layer-local incoming and outgoing spatial tangents, linked/broken state, and Linear, Auto Bezier, Continuous Bezier, and Manual Bezier modes to position keyframes.
- Added deterministic cubic Bezier evaluation in `AnimatedVec2Property`, shared automatically by the editor, prerender/cache pipeline, and OBS source output.
- Preserved backward compatibility: vector keyframes without spatial metadata deserialize as Linear and reproduce their previous straight-line motion exactly.
- Added canvas motion-path rendering and draggable incoming/outgoing handles. Shift constrains handle direction; Alt breaks a linked pair; linked handles preserve independent lengths.
- Added Position keyframe context-menu controls for spatial mode and Break/Join Tangents. Mode changes and tangent drags enter the existing undo/redo history.
- Persisted spatial tangents/mode/link state in title JSON and retained them through full-struct keyframe copy/paste.
- Kept tangent storage in layer-local coordinates and maps editing/display through parent/group transforms, preserving the authored curve under affine hierarchy transforms and nested composition placement.
- Added spatial metadata to cache fingerprints and cubic control hulls to dirty-region bounds so tangent edits invalidate prerenders and curved motion cannot be clipped by stale straight-line tile envelopes.
- Expanded standalone animation tests and added a source contract audit covering persistence, canvas editing, copy/paste, undo integration, cache invalidation, and shared evaluation.

## Development Version 118 — External Data Diagnostics Logging

- Added a dedicated **External Data** logging category covering provider lifecycle, asynchronous refreshes, parsing/publication, source states, field/table updates, table-to-cue synchronization, cue-cell resolution, and render-queue coalescing.
- Added Info, Debug, and Trace detail levels so normal sessions remain readable while diagnostics can follow individual source, field, row, layer, and cue paths.
- Added safe logging helpers that redact URL credentials/query strings and report value type, length, emptiness, and a deterministic fingerprint instead of raw external values or authentication tokens.
- Added UI action logs for source creation/removal/duplication, test/connect/disconnect/refresh, binding changes, mapping changes, and the actual Live Text Cue widget population path.
- Added a standard-library-only logging bridge so the external-data core remains testable without Qt/OBS, plus regression coverage for filtering, sanitization, redaction, fingerprints, and sink failures.
- Fixed table-managed Live Text Cue row changes in **Loop** and **Pause** modes: the OBS source now resolves the pending row through the same external binding/formatter path as immediate Play Once cues instead of applying the intentionally empty authored cell.
- Added cue-control diagnostics for requested/current/pending row, playback mode, transition phase, and the final source-side row commit, without logging raw cue values.

## Development Version 117 — Live Table Binding Preservation Fix

- Fixed mapped Live Text Cue rows being created with the correct count but blank values.
- `normalize_live_text_rows()` now validates generated cell bindings against the title's active column order instead of a moved-from temporary vector.
- Preserves `ExternalTableManaged` state, formatted live values, read-only behavior, and OBS/source runtime bindings when the cue table rebuilds.
- Added a runtime regression that reproduces the exact synchronize → normalize → dock display sequence.

## Development Version 116 — Live Table Value Resolution and Managed Cell State

- Fixed mapped Live Text Cue rows appearing blank while the table result preview contained values.
- Row-specific table runtime values now take precedence over same-path scalar fields or schema placeholders.
- Added an explicit `ExternalTableManaged` cue-cell state: mapped text/image cells are read-only and italic in the dock while remaining cueable.
- Added **Convert to editable value** and **Restore table-managed value** actions. Detached snapshots survive provider refreshes without changing the table mapping for other cells.
- Preserved the exact mapped cell value across transient provider-registry rebuilds.
- Added regression coverage for scalar shadowing, read-only state, detachment, authored-value preservation, and restoration.

## Development Version 115 — Live Table Value Resolution and Cue Styling

- Fixed table-managed Live Text Cue rows remaining blank when a provider exposed valid table cells but did not publish an identical row-specific scalar field key.
- Added a runtime-only authoritative value to generated table cell bindings; it is rebuilt from each table snapshot and is never serialized over authored cue data.
- Routed the runtime value through the same `ExternalDataManager::resolve()` pipeline, preserving connection state, keep-last-value, formatter, empty-value, fallback, and field-default behavior.
- Ensured `apply_live_text_runtime_binding()` carries the table value into editor playback and the OBS source path, not only the dock preview.
- Included runtime table values in unchanged-update comparison so a value-only table refresh updates cue widgets/output without rebuilding unrelated rows.
- Displayed table-managed values in italics inside the Live Text Cues table as a visual origin indicator; output typography remains unchanged.
- Added regression coverage for table snapshots with no scalar row paths and for value-only table updates.

## Development Version 114 — Live Table Cue Value Display Fix

- Fixed source-managed Live Text Cue rows appearing blank even though their generated external bindings were valid.
- Bound text and image cue cells now display the resolved, formatted live provider value in the dock.
- Preserved the authored/fallback cell value separately, so live display refreshes never overwrite authored cue data.
- Added a regression contract requiring table-mapped cells to resolve through `effective_live_text_cue_value()` when their widgets are created.

## Development Version 113 — External Table to Live Cue Mapping

- Added automatic table snapshots for JSON arrays, nested JSON arrays, CSV data rows, local text, and manual/internal providers.
- Added **Populate from external table…** to the Live Text Cues data menu with source/table selection and live result preview.
- Added per-column mapping from provider table fields to exposed cue columns, using the same formatter, fallback, and live-value pipeline as ordinary external bindings.
- Added Replace, Append, and Synchronize update modes, optional starting row/row limit, empty-row filtering, and preservation of manual cue rows.
- Added stable row identity from a selected field or provider row index so synchronized updates retain the correct cue row when source ordering changes.
- Generated cells remain real external-data bindings, retain last-known/fallback behavior, update without authored-value mutation or undo commands, and display the existing bound-cell indicators.
- Added asynchronous provider refresh integration, table-update coalescing, unchanged-snapshot suppression, source-managed row cleanup, and current/pending cue remapping by stable row ID.
- Added runtime and structural regression coverage for table discovery, mapping, shared formatting, row synchronization, cleanup, and UI integration.

## Development Version 112 — Automatic External Field Discovery

- External JSON file, HTTP JSON, WebSocket, and CSV providers now discover every scalar field on each successful update even when schema overrides already exist.
- Discovered nested JSON paths, array indexes, CSV headers, and numeric CSV columns appear immediately in binding popups and existing settings rows without reopening the dialog.
- Reworked **Fields** into an optional schema override table with explicit pinning, display-name/type customization, manual values, and live values.
- Binding a discovered field from a layer property, provider binding row, or live cue cell automatically pins its inferred schema so it remains available while offline.
- Authored/pinned field types remain stable, while unpinned discovered fields may follow genuine provider type changes until they are pinned.
- Unchecking/removing a schema override now releases its fixed type/alias on the next provider synchronization without discarding the last discovered value.
- Added runtime and structural coverage for discovery, auto-pinning, offline placeholders, JSON/CSV discovery with overrides, and unchanged authored-value behavior.

## Development Version 111 — External Data UI and Formatting

- Completed External Data Source Settings with a state-aware source list, add/remove/duplicate actions, provider-specific connection settings, test connection, manual refresh, live field values, timestamps, errors, and refresh status.
- Added serialized refresh behavior per source: refresh on cue, refresh continuously, or refresh manually.
- Added a reusable external-data binding popup for text and image properties with source, field, typed fallback, live raw/formatted preview, and provider state.
- Added a shared structured formatter pipeline for prefix/suffix, decimal places, thousands separators, text case, date/time formatting, conditional replacement, and empty-value behavior while retaining legacy formatter compatibility.
- Added external-data bindings to live text/image cue cells, including refresh-on-cue behavior and runtime-only overrides that preserve authored cue and layer values.
- Added visible bound-state indicators on property buttons, cue cells, and layer rows.
- Added backward-compatible serialization for formatter configurations, refresh modes, and stable cue-cell bindings.
- Added runtime coverage for formatting, cue bindings, authored-value preservation, and a Development Version 111 UI contract test.

## Development Version 110 — Isolated Qt WebSockets Bootstrap

- Downloads the official `qt/qtwebsockets` module automatically when it is absent from the OBS Qt6 dependency bundle.
- Pins the downloaded source to the exact detected Qt patch version (`v<Qt6_VERSION>`) to prevent ABI mismatches.
- Configures, builds, and installs the module through a separate `ExternalProject` CMake process.
- Prevents the module's internal `find_package(Qt6)` from colliding with vcpkg/OBS imported targets such as `Threads::Threads`.
- Disables examples, tests, benchmarks, manual tests, and standalone tests so Qt Quick is not required.
- Exposes the installed static library to the plugin through an imported `Qt6::WebSockets` target.
- Reuses the versioned `_deps/qtwebsockets-<Qt version>` source, build, and install cache.

# Changelog

## Development Version 108

- Fixed Windows OBS dependency discovery so Qt6 is resolved from `lib/cmake/Qt6` and other supported SDK layouts.
- Prevented a detected Qt6 OBS SDK from silently falling back to Qt5.
- Split Qt WebSockets discovery from the base Qt modules to produce an accurate missing-component diagnostic.


## Development Version 108 — Asynchronous External Data Providers

- Added a common `IExternalDataProvider` lifecycle with connect, disconnect, refresh, validation, state, and error reporting.
- Added providers for JSON files, CSV files, HTTP/HTTPS JSON endpoints, WebSocket feeds, local text files, and manual/internal data tables.
- Moved every provider, polling timer, file read, HTTP request, and WebSocket connection to a dedicated worker thread so the UI and OBS render thread remain non-blocking.
- Added nested JSON field paths and array indexing, JSON root selection, CSV row selection, first-row headers, and field-to-column mapping.
- Added HTTP headers, bearer-token authentication, configurable timeout, retry count, exponential retry backoff, and refresh intervals.
- Added WebSocket JSON message parsing, automatic exponential reconnect, explicit connect/disconnect/refresh controls, and configurable last-known-value behavior.
- Added provider-side rate limiting and latest-value coalescing before the existing manager/render queue, while unchanged values remain suppressed by `ExternalDataManager`.
- Added `Connected`, `Updating`, `Disconnected`, `Error`, and `Stale` runtime states without waking rendering when an informative state transition cannot change the effective value.
- Added a provider settings dialog with source configuration, field definitions, manual values, CSV mappings, request options, live state/error/last-update reporting, and direct provider-field bindings to text/image layer properties.
- Persisted provider configuration with titles while keeping current values, runtime errors, and connection state transient and backward compatible.
- Added shutdown synchronization, last-known-value behavioral coverage, and a Development 107 provider contract audit.

## Development Version 106 — Provider-neutral External Data Core

- Added the central, thread-safe `ExternalDataManager` with provider-neutral source schemas, typed fields, current values, source/field timestamps, and connection/error state.
- Added external field types for string, integer, float, boolean, color, date/time, image/file paths, and URLs.
- Added optional per-layer property bindings with source ID, field path, formatter, and binding fallback while preserving the ordinary authored property as the final fallback.
- Persisted external source definitions and layer bindings in title/project JSON without persisting runtime current values or connection state, preserving compatibility with titles that have no external-data keys.
- Wired `text.content` and `image.path` through transient effective-value resolution in the editor, OBS source renderer, scene-mask paths, compatibility/GPU image paths, and cache identity.
- Added a provider-free mock update API, editor refresh callbacks, and a coalescing thread-safe update queue consumed on the OBS render/tick path.
- Suppressed notifications, render work, runtime revision changes, and cache invalidation when a provider repeats an unchanged value.
- Added behavioral and source-contract tests for authored/live/fallback resolution, connection loss, typed updates, mock fields, serialization, and render-queue coalescing.

## Development Version 105 — OmniaTV branding, v0.8.7-alpha, application icon, and documentation consolidation

- Updated the public version to `v0.8.7-alpha` and Development Version 105 across CMake, runtime build metadata, packaging, and dependency manifests.
- Replaced the previous personal credit with **Developed by: omniatv**.
- Added theme-aware normal/inverted OmniaTV logos to the About dialog and linked the logo to `https://omniatv.com`.
- Added the Broadcast Graphics Live application icon and applied it to editor windows and plugin-owned dialogs that previously inherited the OBS application icon.
- Rebuilt the README as a concise project overview and removed stale, duplicated, and personal branding references.
- Consolidated the large collection of one-off documentation notes into seven canonical thematic documents plus this index/changelog set.
- Expanded `.gitignore` for CMake/build outputs, IDE files, generated packages, platform artifacts, caches, logs, temporary files, and local configuration overrides.
- Folded the obsolete standalone PowerShell manifest-fix notes into the maintained build documentation and removed the redundant root text files.

## Development Version 104 — Effects Panel Spacing, Bottom Toolbar and Dock Names

- Removed the extra gap between every effect header and its first setting while preserving the shared side and bottom panel insets.
- Moved the Effects toolbar below the scrollable effect stack, keeping **Add Effect** and **Respect Masks** permanently accessible at the bottom of the dock.
- Renamed the effect-stack dock to **Effects Settings** and the presets dock to **Effects and Presets**, including their Window-menu actions.

## Development Version 103 — Qt6 Title Thumbnail Repaint Compile Fix

- Removed the per-item `QListWidget::visualItemRect()` repaint path from title cue-state refreshes, eliminating the reported MSVC/Qt6 member-resolution compile failure.
- Title thumbnail cue-state changes are now accumulated and trigger one safe viewport repaint after all item roles are updated.
- Preserved immediate cue/uncue thumbnail feedback while reducing redundant paint requests for title lists with multiple items.

## Development Version 102 — Panel-based Effects Stack and Persistent Inspectors

- Replaced the separate Effects Stack list and single Effect Settings editor with one panel-based stack: every effect is shown as its own collapsible settings panel and the visible panel order is the actual render-stack order.
- Reduced the Effects toolbar to **Add Effect** and **Respect Masks**, removing the old Remove, Duplicate, Move Up, and Move Down buttons.
- Added an enable switch directly in every effect header, between the drag handle and effect name, and removed the duplicate Enabled control from the settings body.
- Added a compact overflow menu immediately before the caret with **Duplicate Effect**, **Delete Effect**, **Move Up**, and **Move Down** actions.
- Connected effect-panel drag-and-drop reordering directly to the layer effect model while preserving the selected effect, keyframe bindings, canvas handles, and effect-specific controls.
- Persisted collapsible-panel expanded state and user-defined panel order across editor sessions through `QSettings`; effect order continues to persist in the title/layer model rather than editor preferences.
- Replaced the layer-list expand/collapse icons with the same native caret renderer used by the inspector panels, including the three-state group expansion marker.
- Updated the effects interaction regression contract for the new list-free panel stack and persistent shared panel/caret infrastructure.

## Development Version 101 — Unified Collapsible and Reorderable Inspector Panels

- Made the compact switch track and thumb smaller, lower-contrast, and less visually dominant while retaining clear checked, mixed, hover, focus, and disabled states.
- Added the reusable `BglCollapsiblePanel` widget with a compact After Effects-style header, a drag handle at the left, and an expand/collapse caret at the right.
- Added true sibling-panel reordering by drag-and-drop from the header handle, including before/after drop indicators and preservation of layout stretch factors.
- Unified panel chrome, spacing, body shading, borders, accent separators, and content margins between the Properties and Effects inspectors.
- Migrated the Properties inspector sections, Shape/Image custom sections, gradient Presets/Stops, Effects Stack, Effect Settings, selected effect controls, and nested effect element controls to the common panel widget.
- Preserved dynamic section visibility, existing controls/signals, keyframe behavior, explicit custom colors, and active OBS palette inheritance without adding a third-party runtime dependency.

## Development Version 100 — Compact After Effects-style Qt Controls

- Added native compact Qt widgets that follow the active OBS palette instead of installing an application-wide theme or overriding explicitly customized colors.
- Replaced editor, Effects, Preferences, Transitions, Dock-dialog, and Prerender checkboxes with sleek switch controls.
- Added circular direction controls with synchronized numeric input for Drop Shadow, Inner Shadow, and Long Shadow angles.
- Added strong theme-accented section dividers throughout the layer Properties panel and tightened panel spacing for a denser After Effects-like workflow.
- Added a reusable internal widget layer for future compact controls without adding a third-party runtime dependency.
- Reviewed MIT-licensed Qlementine and LGPL-2.1 Qt Advanced Stylesheets; neither is bundled because both are designed to own the application-level style and could interfere with OBS theme inheritance.

## Development Version 099 — Qt6 Thumbnail Repaint Compile Fix

- Fixed the Titles & Graphics cue-state repaint call for Qt6/MSVC.
- `visualItemRect()` is now invoked on the owning `QListWidget`, rather than incorrectly on `QListWidgetItem`.
- Preserves the lightweight immediate thumbnail-state repaint without triggering preview or cache regeneration.

## Development Version 098 — Thumbnail Ending-State Cleanup

- Synchronized the **Titles and Graphics** thumbnails with the live cue runtime state.
- Active cues use the same red border as the active Live Text Cue row.
- Cue-to-cue transitions and manual uncue/outro playback switch the thumbnail border to yellow until the outgoing title reaches its authored end.
- Cues waiting to become active use a green border, while inactive titles have no cue-state border.
- Applied the state consistently in both list view and icon view, including titles without exposed Live Text fields.

## Development Version 096 — Live Cue Header Runtime Timer

- Added a live runtime counter at the right side of the **Live Text Cues** header.
- Pause and Loop cues show elapsed on-air time as `MM:SS`.
- Cue-to-cue transitions and full uncue outros show the exact remaining time as `MM:SS:T` (minutes omitted below one minute).
- Play Once cues show the remaining time from their appearance until the authored title end.
- The counter is driven by the active OBS source playhead and disappears when no selected title source is running.

## Development Version 095 — Yellow Uncue/Outro Cue State

- Added a dedicated yellow `ending` visual state for the active Live Text Cue row while its uncue/outro is playing.
- The Cue button/icon, cue-row background, and cue-row border switch from red to yellow immediately when uncue begins.
- The yellow state remains active through Play Once, Pause, Loop, and Ping-Pong Loop outro playback and clears only when the authored end is reached and `When cue ends` is applied.
- Kept queued cues green and normally active cues red, with the ending state taking visual priority over the active state.
- Added a regression contract covering the yellow state source, priority, dock refresh paths, static title-only cue rows, and row decoration.

## Development Version 094 — Continuous Uncue Playback and Live Cue Text Snapshots

- Changed manual uncue so Play Once, Pause, Loop, and Ping-Pong Loop titles continue forward from the exact current on-air frame instead of seeking to the pause/loop boundary.
- Kept the cue active throughout the outro and applied the configured `When cue ends` behavior only after the title reaches its authored end.
- Fixed title-only cue toggles for titles without exposed live-text fields by preserving their synthetic cue row until the outro completes.
- Made the active cue's already-applied text/image values authoritative for cached playback, so editing a live-text cue row does not alter the current cue or its uncue; the edited values appear on the next cue.
- Kept cue-to-cue Pause/Loop transitions using their authored hand-off points while limiting current-frame continuation specifically to manual uncue.
- Added a dedicated uncue/playback-text-snapshot regression contract covering dock actions, title hotkeys, source state transitions, cache lookup, and end behavior.

## Development Version 093 — Layer-Space Effects Stack and Background Bounds Fix

- Kept ordinary layer effects on the transform-neutral padded layer raster instead of rerouting the complete stack to a full-canvas pass when Shadow, Glow, Outline, Blur, Bloom, or Long Shadow is present.
- Fixed initial Background Color bounds by versioning the base raster retention mode and translating layer-box/clip metadata whenever an alpha-only raster is cropped.
- Made Background Color geometry explicitly layer-relative, preventing a later Shadow/Glow pass from expanding the fill across the complete canvas.
- Added an affine layer-space basis for full-canvas group, matte, and adjustment paths, so spatial effects follow layer translation, scaling, rotation, and parent transforms.
- Anchored 4-Color Gradient, Lens Flare, Vignette, Noise, and Roughen Edges to layer space while keeping texture sampling in surface space.
- Added missing local support padding for Lens Flare and Roughen Edges, excluded affect-behind effects from unnecessary local-raster expansion, and versioned GPU/disk cache identities.
- Added a dedicated effects-stack layer-space/bounds regression contract and reran the existing effect, procedural shader, cache, modularity, and MSVC include-order audits.

## Development Version 092 — MSVC GPU Cache Alias Compile Fix

- Fixed the MSVC `C2267`/`C2601` failure caused by defining `alias_global_gpu_frame_locked()` between implementation fragments that form one contiguous function body.
- Kept the GPU cache alias helper visible through a top-level forward declaration and moved its definition after all split `title-source` function continuations are closed.
- Added a structural regression audit that verifies the include order, top-level declaration, and balanced combined translation unit.

## Development Version 091 — Linux Text Visibility, Typing Refresh, and Temporal Cache Reuse

- Preserved Linux/FreeType `QImage::Format_Alpha8` glyph coverage explicitly before SDF generation, preventing valid glyph masks from becoming blank during GPU text rendering.
- Replaced partial dynamic-atlas writes with complete uploads of smaller 1024×1024 R8 pages, preventing discard-mapped atlas contents from producing random glyph fragments while typing.
- Made text edits cancel delayed presentation work and force an immediate full-canvas GPU present, so typed characters and text-box geometry appear without waiting for a coalesced playback/editor tick.
- Connected exact evaluated visual-state deduplication to the prerender worker. The first frame of an unchanged state is canonical; subsequent frames share its GPU tiles and create metadata-only SSD aliases instead of repeating layout, effects, compositing, readback, and compression.
- Resolved an equivalent canonical frame already waiting for GPU readback before submitting another one, eliminating redundant in-flight renders.
- Restricted temporal reuse to fully deterministic/cacheable titles and excluded animated asset timelines whose local playback mapping can differ from root timeline time.
- Bumped the cache ABI to invalidate older blank/corrupted Linux text payloads and added a dedicated Development Version 091 structural regression audit.

## Development Version 090 — Text Position-Map Compile Fix

- Fixed the Windows/MSVC build regression introduced by the Development Version 089 UTF-8/UTF-16 position-map optimization.
- Updated the remaining rich-text range call to pass the shared `EditorQtUtf8PositionMap` required by the new helper signature.
- Replaced stale editor-side `rich_byte_offset_from_qtext_position()` calls with the local position map already built for the `QTextDocument` conversion.
- Replaced stale source compatibility-renderer `qtext_position_from_rich_byte_offset_source()` calls with the local `SourceQtUtf8PositionMap`, resolving the cascading `std::min`/`std::max` template errors.
- Added regression-contract coverage that rejects the removed conversion helpers and verifies that editor and source compatibility paths reuse their local maps.

## Development Version 089 — Linux Text Rendering and Text Pipeline Performance Audit

- Retained the exact physical `QRawFont` selected by Qt shaping and Fontconfig and reused that face in the GPU glyph atlas. Linux fallback glyphs are no longer reconstructed only from family/style names, and visible glyphs that cannot produce an alpha map fall back to the Qt compatibility renderer instead of disappearing.
- Reworked inline typing around canonical UTF-8 range edits. Ordinary insertion, Backspace, Enter, and single-character replacement no longer rebuild the complete `QTextDocument`, synchronously render a frame, or repeat document-size and cursor conversions for the same keystroke.
- Coalesced Properties panel, Timeline model, live-output publication, and canvas invalidation during continuous typing while preserving immediate editor feedback and the normal full commit path at the end of editing.
- Added per-paragraph UTF-8/UTF-16 position maps across shaping, source compatibility rendering, and editor adapters, removing repeated string slicing/conversion work for Unicode, emoji, RTL, and large rich-text documents.
- Changed the glyph atlas to upload only newly dirtied rectangles instead of retransferring an entire 2048×2048 page for each uncached glyph, and reduced transient layout allocations through reserved/merged GPU batches and binary paint-run lookup.
- Removed repeated full `Layer` copies and redundant rich-text normalization passes from layout/evaluation hot paths. Auto-style state is now indexed by Unicode code-point boundaries, and transient editor drafts bypass the shared process cache while the retained cache is byte-bounded.
- Audited text property propagation from model through Qt layout and GPU paint runs. Fixed mixed-range underline/strikethrough handling, connected stroke antialiasing to the shader, preserved every fill/gradient/stroke field, and route unsupported miter/bevel inline joins through the exact compatibility raster path.
- Added standalone Unicode/model/layout tests and a text-pipeline performance contract covering Linux font identity, glyph upload behavior, canonical range editing, coalesced editor refreshes, property propagation, and removal of known quadratic conversion paths.

## Development Version 088 — Independent Monitor-Rate Editor Presentation

- Replaced the editor canvas `obs_display_t` presentation path with an editor-owned libobs GPU swap chain. OBS displays are rendered from the main project video loop, so timer-only throttling could not make a stopped editor present faster than the project frame rate.
- Editing, direct manipulation, text editing, keyframe changes, and timeline scrubbing now present from the Qt monitor-paced path and are capped by the refresh rate of the monitor hosting the editor.
- Real transport playback remains isolated: only project-rate transport ticks authorize a canvas present while playback is active. Generic Qt repaints cannot add extra playback frames.
- Retained the existing GPU artwork renderer, overlays, selection inversion, color-space updates, resize handling, and explicit shutdown teardown while changing only swap-chain ownership and presentation scheduling.
- Counted any graphics-backend present wait inside the monitor cadence and coalesced pending presents, preventing double waits and runaway repaint queues on high-refresh displays.
- Added regression coverage that rejects reintroduction of `obs_display_create()` for the editor canvas and verifies independent swap-chain creation, monitor-paced editing, project-paced playback, and shutdown destruction.

## Development Version 087 — Monitor-Capped Editing and Project-Rate Playback

- Canvas artwork refreshes while editing are capped to, and never scheduled above, the refresh rate of the monitor currently hosting the editor window.
- Timeline scrubbing is always treated as editing and never bypasses the monitor cap, even if transport state changes during the interaction.
- Playback refresh is driven only by the project/OBS frame rate, independent of monitor refresh rate.
- Removed the previous hidden 60 Hz transport ceiling, allowing 100/120/144 fps projects to preview at their configured project cadence when rendering can keep up.
- Render-cost pacing remains active, so expensive frames may run below the monitor cap without creating an event backlog.
- Moving the editor between monitors recalculates the editing cadence safely after the window/layout transition.

## Development Version 086 — Shutdown Safety and Effect-Handle Performance Audit

- Added an explicit, idempotent editor shutdown phase that stops all editor/dock timers, disconnects cross-widget Effects Stack ↔ Canvas callbacks, and prevents late playhead, autosave, paint, or property updates from running during Qt child destruction.
- Added explicit Canvas GPU teardown before widget destruction, including OBS display callbacks/textures and the editor GPU render session, with shutdown guards on queued title/playhead updates and draw callbacks.
- Deduplicated effect canvas-handle publication and Canvas ingestion, avoiding repeated JSON rebuild-driven repaints and GPU invalidations when evaluated handle positions have not changed.
- Kept both possible Effects Stack/Canvas construction orders safe with unique signal connections, preventing duplicate handle updates.
- Moved dock removal and frontend callback cleanup into the OBS-supported shutdown window and prevented frontend API calls after `OBS_FRONTEND_EVENT_EXIT`.
- Added deterministic shutdown of the asynchronous title-store save worker before the final synchronous save, preventing stale writes and plugin code execution during module unload.
- Added regression coverage for shutdown guards, timer/signal teardown, GPU resource release, frontend lifecycle rules, save-worker joining, and handle-update deduplication.

## Development Version 085 — Stable Effect Handle Dragging and Keyframed Positions

- Preserved the active effect canvas handle across live Effects Stack refreshes, preventing a point drag from falling through to the layer resize/scale interaction after the first mouse movement.
- Re-evaluated and republished effect canvas-handle positions whenever the playhead changes, so native and extension point controls follow their keyframes in real time.
- Restored the original unrestricted radial Fill Gradient center and radius editing. Only the focal point remains constrained just inside the active radial circle, while a non-zero radius prevents inverted or malformed gradients.
- Added regression coverage for active-handle identity during drag, playhead-driven handle refresh, unrestricted center/radius motion, and focal-circle safety.

## Development Version 084 — Effect Handle Wiring, Vector Editing and Stable Stack Selection

- Connected the Effects Stack dock to the already-created canvas in the editor's real construction order, restoring visible and draggable on-canvas controls for native and extension effect positioning properties.
- Added independent draggable **X** and **Y** labels to extension point/vector controls, so both components support the same drag-to-edit workflow and undo grouping.
- Prevented transient `currentRowChanged(-1)` notifications during Effects Stack rebuilding from replacing the selected effect with the first stack item after an edit.
- Constrained radial Fill Gradient controllers: the center stays inside the fill bounds, the radius handle cannot move beyond the bounds in its active direction, and the focal handle remains inside the radial circle to avoid malformed or inverted rendering.
- Added regression coverage for late canvas/dock signal wiring, vector-axis drag labels, stable effect selection, and radial handle constraints.

## Development Version 083 — Unified Effect Keyframes and Canvas Position Handles

- Unified built-in and extension effect diamonds with the editor's standard keyframe behavior: inactive/animated/active states, click-to-toggle at the playhead, automatic insertion while editing an animated property, right-click **Delete All Keyframes**, and timeline easing/interpolation support.
- Added native and extension effect-property tracks to the layer timeline with unique per-effect-instance identities, including copy, cut, paste, multi-selection, drag retiming, easing changes, and keyframe deletion.
- Grouped native effect color channels into a single timeline color track, so moving, copying, deleting, or changing easing updates Alpha, Red, Green, and Blue keyframes together.
- Added a confirmation modal when removing an effect that contains native or extension keyframes. Confirming removes the effect and all of its associated timeline tracks; cancelling preserves both.
- Added transform-aware on-canvas handles for every extension point/position parameter and for native Lens Flare/Vignette centers plus Background Color gradient center and focal point. Handles follow layer rotation, scale, parenting, and group transforms and respect auto-key animation.
- Applied extension keyframe easing consistently in both the GPU presentation path and compatibility compositor, including Linear, Ease In, Ease Out, Easy Ease, custom Bezier, and Hold interpolation.
- Added regression coverage for effect-instance timeline tracks, grouped color animation, deletion warnings, extension clipboard operations, easing, and layer-local canvas-handle conversion.

## Development Version 082 — Content-Clipped 4-Color Gradient and Visible Effect Keyframes

- Changed **4-Color Gradient** to use the original layer artwork alpha as its matte, so the generated colors are clipped to text glyphs, transparent images, shapes, and composited group content instead of filling the layer bounding rectangle.
- Fixed effect keyframe controls that were backed by animation data but rendered as font-dependent Unicode glyphs; they now use the same reliable SVG diamond icons as the main Properties panel.
- Replaced font-dependent Unicode diamond characters with the editor's actual active/inactive keyframe SVG icons across built-in, extension, color, point, enum, boolean, and compound-element effect properties.
- Preserved direct keyframe toggling, playhead-state updates, animated-value editing, accessibility labels, and extension keyframe persistence.

## Development Version 081 — Effects Cleanup, Keyframes and 4-Color Gradient

- Removed obsolete effect controls that were still visible although the GPU renderer ignored them, including legacy blur-type selectors, nonfunctional blend-mode selectors, and unsupported outline/background corner variants. Backward-compatible serialized fields remain readable but are no longer exposed as misleading controls.
- Added diamond keyframe controls beside every effect option backed by an animation property, including the new generated-gradient points, colors, Blend, Jitter, and Opacity controls.
- Repaired **Background Color** so it uses the layer raster bounds and animated padding/corner/gradient values while always compositing the original layer artwork above the generated background.
- Rebuilt **Emboss** as a directional relief effect driven by alpha and luminance, with functional Depth, Height, Angle, Softness, and Opacity controls.
- Added **Built-in → Generate → 4-Color Gradient** with four movable points, four independently keyframeable colors, Blend, Jitter, Opacity, blending modes, and on-canvas point handles.
- Added regression coverage for the effect registry, extension schema, cleaned UI contract, shader uniforms, Background Color compositing, Emboss relief, and 4-Color Gradient animation bindings.

## Development Version 080 — Restored Consistent Canvas Scaling

- Restored the unified pre-074 scaling contract for every canvas object after it regressed in Development Version 079.
- Normal, Shift-constrained, snapped, rotated, and Alt centre-resize now all derive from the same pointer-defined local target rectangle.
- Scale-backed Text, Group, and Asset Layers receive the required position correction on every resize, not only while Alt is held, so the grabbed handle stays under the pointer and the opposite handle remains fixed.
- Size-backed shapes, images, and other layers preserve the animated origin as a fraction of the new dimensions instead of reusing stale absolute local offsets.
- Removed the competing second Alt-only position correction that could pull the object away from both the pointer and its intended anchor.
- Added a regression contract that rejects both the absolute-offset path and modifier-specific anchor correction while preserving the fixed animated Asset Layer envelope introduced in Version 079.

## Development Version 079 — Static Animated Asset Bounds and Direct Asset Editing

- Animated Asset Layers now calculate a fixed envelope from the union of the source composition across its complete animation, including keyframed transforms, animated size/origin/free-transform corners, nested asset content, timed visibility, and general slide/scale transitions. The selection box no longer changes with the playhead; the animation runs inside one stable bounding box spanning its spatial extremes.
- The Assets library context menu now includes **Edit Asset** alongside Insert and Delete.
- Right-clicking a single Asset Layer on the canvas now exposes **Edit Asset** for its linked source asset.
- When the current title has unsaved changes, Edit Asset opens a modal with **Save**, **Discard**, and **Cancel** before switching the editor to the source asset. Saving the asset preserves its asset identity, so it remains hidden from Titles & Graphics and available in Libraries → Assets.

## Development Version 078 — Independent Asset Playback Controls

- Asset Layers now expose synchronized/independent playback only when their nested composition contains real timeline animation: keyframes, transitions, timed visibility, animated effects, or another animated Asset Layer.
- Independent Asset Layers use a per-instance monotonic runtime clock in both the editor and OBS output, so scrubbing or stopping the parent title playhead no longer changes their animation time.
- Assets saved with Pause playback expose **Pause for** with an `HH:MM:SS:FF` timecode plus a complete-animation Loop toggle.
- Assets saved with Loop playback expose **Loop _N_ times** plus a complete-animation Loop toggle; restart and ping-pong loop areas are both preserved.
- Source duration, pause marker, loop area, loop type, playback mode, pause duration, and loop count are stored in each Asset Layer snapshot for reliable offline/nested playback.
- Static assets no longer show the synchronized/independent selector or any playback-only controls.

## Development Version 076 — Unified Libraries Dock

The former Styles dock is now named **Libraries** and contains a single ordered tab set: **Color Swatches**, **Gradients**, **Text Styles**, and **Assets**. Color Swatches has been moved into this tab set and its standalone dock/window action has been removed. The separate Animated Assets library has also been removed; the unified Assets tab now lists both static and animated asset titles while preserving each asset layer’s synchronized or independent playback behavior. Saved editor layouts are migrated to the new dock schema.

## Development Version 075 — Assets and Animated Assets

Complete titles can now be saved from **File → Save as Asset** and reused inside other titles as dedicated **Asset Layers**. The Styles dock includes searchable, categorized **Assets** and **Animated Assets** libraries with double-click insertion and drag-and-drop directly onto the canvas. Asset instances preserve exposed text/image overrides, effects, mattes, nested composition, group-style content bounds, and either synchronized title-time playback or independent ticker-style animation. Asset records are intentionally hidden from the Titles & Graphics dock and OBS title-source selector.

> **Alpha software:** This build is intended for development and testing. Keep backups of important projects and templates. File formats, UI behavior, performance characteristics, and internal APIs may still change before the stable release.

## Development Version 074 — Consistent Group Canvas Manipulation

- Group transform handles and transform calculations now use the same descendant-derived local bounds.
- Move, resize, rotate, snapping, Shift-constrained resize, and Alt centre-resize no longer operate from a synthetic centred rectangle.
- Asymmetric and nested groups manipulate from the exact canvas outline shown to the user.
- Group duplication drag keeps the copied hierarchy as the active gesture target.
- Development Version 074 is exposed through the editor title, dock/About UI, build metadata, and plugin log.

## Development Version 073 — Authoritative Duplicate Drag Target

- Alt+drag now keeps the duplicated layer IDs as the authoritative gesture target for every mouse-move event until release.
- Synchronous layer-list or timeline selection refreshes can no longer redirect the drag back to the original artwork.
- The duplicated layers remain selected when the drag completes.
- Development Version 073 is exposed through the editor title, dock/About UI, build metadata, and plugin log.

## Development Version 072 — Continuous Alt+Drag Duplication

- Alt+drag now transfers the active canvas gesture directly to the duplicated layer IDs.
- The duplicate continues following the pointer immediately after it is created; the original remains at its starting position.
- The behavior is shared by single layers, multi-selection, groups, and nested groups.
- Synchronous layer-list and timeline refreshes can no longer redirect the in-progress drag back to the original selection.
- Development Version 072 is exposed through the editor title, dock/About UI, build metadata, and plugin log.

## Development Version 071 — Group Alt+Drag Duplication

- Alt+drag on the canvas now duplicates complete group layers, including all nested child layers and nested groups.
- Internal group membership, parenting, transform-parent, and matte references are remapped to the duplicated hierarchy.
- Only the duplicated root selection is moved during the drag, preventing child transforms from being applied twice.
- Locked children inside an unlocked duplicated group are preserved as part of the copied container.
- Development Version 071 is exposed through the editor title, dock/About UI, build metadata, and plugin log.

## Development Version 070 — Clipping Matte Shape Coverage Fix

- Changed **Clipping Matte** extraction to use the matte source artwork shape at full compositor opacity instead of behaving like an Alpha Matte.
- Animated layer opacity, transition opacity, and parent/group compositor opacity no longer fade or disable the clipping region, including at 0% opacity.
- Preserved intrinsic image transparency and anti-aliased text/shape edges so the clipping boundary remains smooth and follows the actual artwork geometry.
- Separated clipping-shape textures from ordinary matte/artwork textures in the GPU cache. A visible clipping base still renders with its normal opacity, while its clipping shape remains opacity-independent.
- Applied the same behavior to ordinary layers, nested mattes, grouped layers, and Groups used as clipping sources.
- Development Version 070 is exposed through the editor title, dock/About UI, build metadata, and plugin log.

## Development Version 069 — Clipping Matte Artwork Visibility Fix

- Fixed **Clipping Matte** sources ignoring the three-state matte visibility control.
- **Hidden artwork, active as matte** now keeps the clipping source fully functional as the target mask without compositing the source artwork into the frame.
- **Visible artwork and active as matte** continues to composite the clipping source once as the base beneath the linked clipped target.
- The behavior is identical for root layers, grouped children, and Groups used as clipping sources.
- Development Version 069 is exposed through the editor title, dock/About UI, build metadata, and plugin log.

## Development Version 068 — Clipping Matte Composition and Toggle-State Fix

- Fixed **Clipping Matte** so the matte source is composited once as the visible base immediately before the first linked clipping target, independent of either layer's position in the stack.
- The linked target is then alpha-clipped against the completed matte source, including Group matte sources and Group targets.
- Fixed the Alpha/Luma/Clipping type toggle leaking `VisibleAndMatte` state after passing through Clipping. Alpha and Luma now resume correctly immediately, without disabling and re-enabling the target.
- Multiple layers may share one clipping source without repeatedly compositing the base layer.
- Development Version 068 is exposed through the editor title, dock/About UI, build metadata and plugin log.

## Development Version 066 — Group as Track Matte Source Fix

- Fixed the asymmetric GPU matte path where rendering a Group matte reused and overwrote the target layer's shared foreground texture before masking.
- Group matte sources now preserve the consumer texture, composite all descendants, publish the completed Group matte, and only then apply alpha/luma masking.
- The same protected path is used for normal layers, grouped children, adjustment coverage and affect-behind silhouettes.
- Development Version 066 is exposed through the editor title, dock/About UI, build metadata and plugin log.

## Development Version 064 — Group Target Matte GPU Feedback Fix

- Fixed a GPU read/write feedback hazard when a matted group result reused the shared mask render target after one of its children had already been masked.
- Mask inputs are now snapshotted into independent full-canvas textures whenever they alias the destination mask target, so the completed group can be masked reliably.
- The protection applies to group targets, child mattes and nested matte chains without changing the already-working group-as-matte-source path.
- Group effect order continues to follow the existing `Effects -> Matte` / `Matte -> Effects` setting.
- Development Version 064 is exposed through the editor title, dock/About UI, build metadata and plugin log.


## Development Version 059 — Unified Gradient Preset Swatches

- Qt 6 dialog capture compile fix: gradient preset naming now captures the popup by reference, avoiding const-parent conversion and deleted `QDialog` copy-constructor errors.

Gradient presets now use one persistent library across the entire editor. The Gradient Styles dock and every fill/stroke gradient popup display the same compact, color-swatch-style previews, including gradient type, opacity, intermediate stops, spread, angle, and transparency. Presets can be created directly from the active shape fill, text fill, layer stroke, or inline rich-text fill/stroke; applying a swatch immediately updates the current target. User presets can also be removed from the swatch context menu, while built-in presets remain protected. Changes are synchronized live between open editor surfaces, and import/export continues to use the shared Styles library.

## Development Version 057 — Refined Group Editing, Independent Parenting, and Matte Controls

Group and parenting workflows are now clearly separated: transform parenting no longer creates or modifies Groups, while parent/unparent operations preserve the layer's world-space appearance. Newly created Groups start collapsed, collapsed or keyframe-only Groups behave as single selectable containers, and fully expanded Groups expose their children for direct canvas and Effects-panel editing. Child effect stacks continue to render correctly inside Groups, including mask-aware ordering and affect-behind compositing. The update also prevents parents and Groups from snapping to their own descendants, adds three-state track-matte visibility, introduces three-state Group timeline expansion, aligns hierarchy-aware layer-list columns, and standardizes Parent and Matte selectors with numbered layer names.

## Development Version 128 — Auto Styling Merge API Compile Fix

- Declares `rich_text_auto_style_rules_equivalent` and `rich_text_merge_auto_style_rules` in the public rich-text header.
- Fixes the MSVC C3861 errors in `property-synchronization.inc` when loading, appending, or learning rules.
- Keeps rule merging and deduplication in the canonical rich-text model instead of duplicating logic in the properties panel.

## Development Version 127 — Rule Deduplication and Smart Generalization

- Prevents duplicate learned rules and duplicate rules during rule-set Append/Replace loading.
- Promotes equivalent examples to one reusable all-matches rule instead of creating repeated entries.
- Adds per-rule generalization policy: Smart merge, Exact structure only, or Always keep separate.
- Adds explicit duplicate-prevention and multiple-case application controls in Advanced options.
- Persists the new controls in title JSON and portable `.gsp-auto-style.json` rule sets.

## Development Version 126 — Smart Text Analysis and Learned Styles

- Generalizes formatted examples into semantic rules for time, date, numbers, email addresses and URLs.
- Infers fields at paragraph starts, between delimiters and after structural separators.
- Learned rules carry a complete inline style snapshot, so fill, stroke, font and other formatting remain portable without an existing preset.
- Added Clear All Rules actions to the main Auto Styling panel and Rule Editor, with destructive-action confirmation.
