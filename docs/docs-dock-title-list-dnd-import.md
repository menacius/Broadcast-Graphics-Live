# Dock Title List Drag & Drop Import

Implemented external drag-and-drop import support for the dock title list. The title list now accepts one or more `.obgt` files dropped from the OS/file manager and routes them through the existing `TitleDataStore::import_title` pipeline.

Behavior:
- Single and multi-file drops are supported.
- Unsupported files, directories, unreadable files, and non-local URLs are ignored.
- Imports preserve the incoming drop order after duplicate file paths are removed.
- Individual failures do not stop the remaining imports.
- The last successfully imported title is selected after a multi-file drop.
- The existing Import Title menu action now reuses the same shared import helper.
- The title list shows a lightweight OBS-style highlight while a valid `.obgt` drag is over it.
