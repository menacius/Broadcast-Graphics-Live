# Text and live data

## Rich-text model

Text layers use a run-based rich-text model as the single source of truth. Manual formatting, auto styling, the inline editor, property panels, serialization, and rendering read and write the same runs rather than converting between an independent HTML model and legacy overrides.

Character properties include font, size, fill, stroke, horizontal and vertical scale, tracking, baseline, capitalization, and style. Paragraph properties include alignment, justification, indentation, spacing before/after, vertical alignment, wrapping, and line breaks.

## Inline editing

Clicking a text layer can enter on-canvas editing. Cursor movement and character insertion preserve the active run style and caret position. Escape or switching tools leaves text-edit mode. Multi-style selections display mixed/blank property values when no single value is authoritative.

## Auto text styling

Auto-style rules apply formatting to ranges selected by conditions such as text start/end, paragraph start/end, whitespace, newline, custom characters, word counts, and character counts. Rules can combine conditions, exclude another rule, include or omit stop characters, and avoid applying when a required stop condition is absent.

Live text cues apply auto styling when values are committed, so dock-driven text uses the same formatting logic as editor-authored text.

## Clock and ticker layers

Clock and ticker content is generated at runtime but uses the same text layout and property model. Tickers support wrapping, vertical/horizontal modes, paragraph formatting, custom completion, and independent playback where applicable. Clock and non-cacheable ticker modes remain real-time and are excluded from ordinary frame prerendering.

## Exposed properties and cues

Text and image properties can be exposed to the Titles and Graphics dock. Cue rows store user-facing values and are applied as snapshots. The currently active cue keeps its applied snapshot even when the row is edited; updated values become active on the next cue.

Cue states include inactive, queued, active, and ending/outro. Runtime counters can show elapsed or remaining time according to playback mode. Uncue continues from the current frame and reaches the authored end before applying the configured end behavior.

## External data

External data now has a provider-neutral core separate from the existing cue import/append workflow. A title can serialize named source definitions and typed fields, while `ExternalDataManager` owns runtime current values, source/field timestamps, and connection or error state. Current provider values are deliberately not written into title JSON.

Supported field types are string, integer, float, boolean, color, date/time, image/file path, and URL. A layer stores optional bindings by canonical property path. Each binding contains a source ID, field path, optional formatter, and optional binding fallback. The effective value is resolved in this order:

1. Current field value while the source is connected/updating, or the retained last-known value when that policy is enabled.
2. Binding fallback.
3. Field default.
4. The layer's authored property value.

This keeps authored content unchanged when live data arrives or a source disconnects. External updates increment only a runtime presentation revision, do not create undo commands, and do not schedule title persistence. Repeated values update receipt timestamps but do not enqueue render work, dirty the source, or change cache identity.

The initial renderer integration supports `text.content` and `image.path`. Text bindings update the editor and OBS output through a transient rich-text document, so the saved text and its authored formatting model remain intact. Image bindings use the same effective path in compatibility rendering, GPU upload paths, effects, and cache hashing. Generic string, number, boolean, and color resolvers are available for additional property paths.

### Providers

The provider layer implements a common `IExternalDataProvider` interface and supports:

- **JSON file** — nested object paths, array indexes such as `items[0].headline`, optional root paths, and periodic refresh.
- **CSV file** — selectable data row, optional first-row headers, and explicit `field.path=column` mapping.
- **HTTP/HTTPS JSON** — asynchronous requests, custom headers, optional bearer token, timeout, retries with backoff, and configurable polling.
- **WebSocket** — asynchronous JSON message parsing, automatic reconnect with bounded exponential delay, and last-known-value retention.
- **Local text file** — publishes the complete file contents to a configurable field path.
- **Manual/internal table** — typed values stored with the source definition and published through the same runtime path.

All provider objects and their timers live on a dedicated external-data worker thread. They never perform file or network work on the Qt UI thread or OBS render thread. Incoming values are rate-limited and coalesced by field before entering the manager's existing coalescing render queue. HTTP refreshes also coalesce overlapping requests.

Provider states are `Connected`, `Updating`, `Disconnected`, `Error`, and `Stale` (with `Connecting` retained for transition reporting). Provider responses automatically discover scalar JSON paths (including array indexes) and CSV columns. Discovered fields are immediately selectable in every binding popup; no manual schema entry is required. The settings dialog exposes source/provider options, an optional **Fields** override tab, manual values, and a **Bindings** tab for mapping fields to eligible text, clock, ticker, and image properties. It also displays the current state, last update time, and error message. Bindings are committed only when the dialog is saved, so Cancel never alters authored layer properties. Errors do not interrupt rendering. When **Keep last valid values** is enabled, the most recent valid field value remains effective through stale, error, reconnect, or temporary disconnect states; otherwise the binding fallback, field default, or authored value is used.

Informative state changes that cannot alter the effective value are recorded for the UI without incrementing the render revision. This prevents routine polling of unchanged data from dirtying previews or invalidating caches. `update_mock_value()` remains available as a provider-free test and internal-integration path.
### Populate Live Text Cues from a provider table

After refreshing a JSON, CSV, HTTP JSON, WebSocket, local-text, or manual source, open the **Data Sources** menu in the Live Text Cues toolbar and choose **Populate from external table…**. Select the discovered table or JSON array, then map each exposed cue column to a table field. No per-cell setup or manual field registration is required.

The mapper provides three update modes:

- **Replace rows** — the selected provider table becomes the cue list.
- **Append rows** — previously created cue rows remain and newly discovered source rows are added.
- **Synchronize rows** — source-managed rows are added, updated, reordered, or removed to match the provider snapshot; manually authored rows can be preserved.

Choose an optional stable **Row ID field** when the source contains an ID. Otherwise the provider row index is used. A start row, maximum row count, empty-row filter, formatter, fallback, and live result preview are available. Each generated cell is stored as a normal external binding, so editor preview, cue playback, OBS output, last-known-value handling, and visual binding indicators all use the same runtime path. An explicit cell binding can override a generated table cell without changing the table mapping.

Table-managed cue values use the explicit **ExternalTableManaged** cell state. They are shown in *italics* and are read-only inside the Live Text Cues table, while cue playback and OBS output remain unchanged. Unmapped cells in the same row remain normal authored cells. Right-click a managed cell and choose **Convert to editable value** to capture its current formatted value as an authored snapshot; later provider refreshes will preserve that detached cell. Choose **Restore table-managed value** to reconnect it to the row mapping. The table snapshot itself is authoritative: if a provider does not expose an identical row-specific scalar field key, the generated binding carries a transient runtime value so the cue still renders correctly while preserving authored storage and fallback rules.

### Binding UI and formatter pipeline

External Data Source Settings provides a complete source workflow: create, remove, or duplicate a source; choose the provider type; configure file/network/CSV/WebSocket options; test the connection; refresh manually; and inspect every discovered or pinned field's current value, type, timestamp, connection state, schema status, and error. Each source selects one refresh behavior: **Refresh on cue**, **Refresh continuously**, or **Refresh manually**. Polling is active only for continuous sources. The **Fields (optional)** tab is only for pinning offline schema, aliases, type overrides, CSV/custom mappings, and manual values. Selecting a discovered field in any binding automatically pins it. Unchecking or removing that override returns the field to provider-inferred discovery while retaining its last valid runtime value.

Bindable text and image properties display a chain-style data button. A highlighted button and a `D`/`FX•` layer-list badge identify externally bound content. The binding popup selects the source and field, accepts a typed fallback, and previews the raw value, formatted value, value origin, provider state, error, and update timestamp before Save. Live text and image cue cells expose the same popup from their context menu and show an orange bound-state border.

The structured formatter pipeline is shared by editor preview, live cue playback, cache identity, and OBS source rendering. Operations are applied deterministically: legacy formatter compatibility, empty-value policy, conditional replacement, date/time or numeric formatting, text case, then prefix/suffix. Supported controls are prefix, suffix, decimal places, thousands separators, uppercase/lowercase/title case, `strftime` date/time patterns, ordered exact conditional replacements with optional case sensitivity, and empty handling (keep empty, use fallback/default/authored, or replace).

Cue-cell bindings are saved by stable cue-row ID and layer ID. Cueing installs a runtime-only binding on the exposed layer, so subsequent external updates continue to reach the live output while the authored layer text, authored cue value, and undo history remain unchanged. A source configured for **Refresh on cue** is asked to refresh asynchronously immediately before the cue is applied; playback never waits for network or file I/O and uses the current last-known/fallback resolution until a new value arrives.

### External-data diagnostics logging

For provider or Live Text Cue troubleshooting, open **Broadcast Graphics Live Preferences → Logging** and enable logging. Select **Debug** for lifecycle, refresh, state, mapping, and managed-cell population events; select **Trace** when field-level update suppression, render-queue coalescing, or every cue-cell resolution must be inspected. Ensure the **External data** category is enabled. The current session file path is shown in the Logging preferences page, and optional mirroring to the OBS log remains controlled by the global logging setting.

External-data messages include a `component=` tag such as `Provider`, `Http`, `WebSocket`, `Manager`, `TableMapping`, `Dock`, or `DockCell`. Provider locations are sanitized: URL user information, query strings, and fragments are omitted. Authentication tokens and request-header values are never logged. External values are represented by their type, set/empty state, byte length, and a deterministic fingerprint rather than their raw content. Matching fingerprints identify unchanged data across stages without exposing the value.

A useful debugging sequence is: open External Data Source Settings, press **Test connection** or **Refresh now**, apply the table mapping, and rebuild the Live Text Cues view. At Debug level the log then shows provider state transitions, parsed/published counts, table snapshot changes, mapping row counts, and each `ExternalTableManaged` cell as it is inserted into the dock. This makes it possible to distinguish provider parsing failures from mapping, normalization, binding-resolution, or final widget-population failures.
For cue switching, the `CueControl`, `CuePlayback`, and `CueApply` components show the requested/current/pending row, playback mode, transition phase, source-side commit, and a privacy-safe fingerprint of the resolved value. In Loop and Pause modes the next row is committed after the outgoing segment; the source resolves table-managed values at that exact commit rather than reading the empty authored placeholder.
