# User guide

## 1. Install and launch

Install the plugin in the standard OBS plugin layout, start OBS, and open **Docks → Titles and Graphics**. Open the editor from the dock or from the plugin entry under the OBS Tools menu.

The plugin uses its own Broadcast Graphics Live application icon for editor windows and plugin dialogs. The dock remains part of the OBS main window and follows the active OBS theme.

## 2. Create a title

1. Click **Add** in the Titles and Graphics dock.
2. Choose a blank title or a canned template.
3. Add text, shapes, images, clocks, tickers, or reusable assets from the editor toolbar.
4. Arrange layers on the canvas and animate them on the timeline.
5. Save the title, then use **Add to Scene** to create a native OBS source.

The OBS source name follows the title name. Existing title sources can be rebound to another saved title from their source controls.

## 3. Edit the canvas

- Use the Selection tool for whole-layer transforms.
- Use Direct Selection for path points and detailed vector editing.
- Use Free Transform for corner-pin and free-transform operations.
- Drag rulers to create guides. Objects, guides, origins, and selection bounds can participate in snapping.
- Alt-drag duplicates a layer and continues dragging the duplicate.
- Copy/paste or drag external text and supported image files onto the canvas to create layers.

## 4. Work with layers

Layers can be reordered from the layer list or timeline. Groups behave as composited containers with their own transform and effect result. Parenting is independent from grouping: a child follows its parent without being moved into a group.

Track mattes support Alpha, Luma, inverted variants, and Clipping Matte behavior. Matte visibility controls whether the source artwork is visible, hidden, or visible while still acting as a mask.

## 5. Animate

Properties with a diamond control can be keyframed. The timeline supports multi-selection, easing, negative values, layer strips, group expansion, and keyframe navigation. During ordinary editing and scrubbing the editor can present at the monitor refresh rate; authored playback runs at the project frame rate.

## 6. Effects

Open **Effects Settings**. Each effect is a collapsible panel and also the actual stack item. Drag the panel header to reorder the render stack, use the switch in the header to enable or disable the effect, and open the header menu to duplicate, delete, move up, or move down.

Use **Effects and Presets** for reusable effect and transition content. The bottom toolbar in Effects Settings contains **Add Effect** and **Respect Masks**.

## 7. Live text and image cues

Expose selected text or image properties to the dock. Each row can hold its own values. Cue a row to apply it to the active source; queued, active, and outgoing states use distinct visual indicators. Editing a row does not alter the already-active cue snapshot—the new values appear on the next cue.

Playback modes include Play Once, Pause, Loop, and Ping-Pong Loop. Uncue continues from the current on-air frame through the authored outro and applies the configured end behavior when the title reaches the end.

## 8. Cache and prerender

Caching can store rendered frames in RAM and optionally on disk. Titles containing real-time clock or ordinary ticker behavior are excluded unless their mode is explicitly cacheable. Cache indicators show title and cue-row progress. See [RENDERING_AND_CACHE.md](RENDERING_AND_CACHE.md) for policy and troubleshooting.

## 9. Preferences and persistent UI

Editor panel collapse states and user-defined panel order persist between sessions. UI colors follow the active OBS palette unless a property or widget explicitly defines another color. Cache location, RAM limits, cleanup behavior, and editor presentation options are available in Preferences.
