# Recent Colors, Text Box Limits, and Luma Masks

- Updated the Color tab recent-colors behavior to work like a Photoshop-style MRU list: colors are added only when the selector is normally closed, eyedropper hand-offs do not add entries, duplicates are removed, and the final color is moved to the front of the list.
- Persisted the recent-colors list with the saved title/template data so the list travels with saved project/template files.
- Fixed Max Text Box Width and Max Text Box Height controls so they remain editable whenever text auto-size options are visible instead of becoming locked behind the width/height auto-fit checkboxes.
- Added Luma and Inverted Luma layer mask modes alongside Alpha and Inverted Alpha track mattes.
- Added layer-list mask choices for Luma and Inverted Luma and timeline labels for the new matte modes.
- Implemented Cairo-side luma mask compositing by converting the selected mask layer surface into a luminance-derived alpha mask before applying the matte.
