# Changelog

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

The former Styles dock is now named **Libraries** and contains a single ordered tab set: **Color Swatches**, **Gradients**, **Patterns**, **Text Styles**, and **Assets**. Color Swatches has been moved into this tab set and its standalone dock/window action has been removed. The separate Animated Assets library has also been removed; the unified Assets tab now lists both static and animated asset titles while preserving each asset layer’s synchronized or independent playback behavior. Saved editor layouts are migrated to the new dock schema.

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
