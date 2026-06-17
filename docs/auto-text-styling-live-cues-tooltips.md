# Auto Text Styling: UI tooltips and live text cues

This update makes the Auto Styling controls easier to understand and applies the same rule engine to live text cues.

## UI improvements

- Renamed the section to **Auto Styling Rules**.
- Added a short explanatory help label under the enable checkbox.
- Added tooltips for the main enable switch, base style, rule list, preset selector, rule name, active checkbox, start/end markers, offsets, custom characters, match mode, conflict mode, excluded rules, stop/protect behavior and rule buttons.
- Increased the rule list height so multiple rules are easier to scan.
- Reworded row labels to be more action-oriented:
  - Base style
  - Apply preset
  - Start matching at
  - Stop matching at
  - Apply to
  - If rules overlap
  - Exclude rules
  - Protect match
- Each rule list item now has a tooltip showing its rule ID, preset, resolved range description, conflict mode and enabled state.

## Live text cue behavior

Live text cue application no longer rebuilds the rich text document from scalar defaults. That old path discarded auto-style rules and manual rich-text metadata whenever a cue row was applied.

The cue row now replaces only the text content while preserving:

- Auto Styling enabled state
- Base auto style preset
- Auto style rules
- Rule conflict/exclusion settings
- Manual rich text ranges
- Typing/default rich text format

The renderer then resolves auto styling from the preserved rich-text model against the cue text, so cue values are styled exactly like normal textbox content.

Updated paths:

- `src/cache/cache-manager.cpp`
- `src/obs/title-source.cpp`
- `src/editor/title-dock.cpp`
- `src/editor/title-hotkeys.cpp`
- `src/editor/properties-panel.cpp`
