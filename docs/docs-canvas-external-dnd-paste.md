# Canvas external drag/drop and paste

Implemented external content import directly on the editor canvas.

- The canvas now accepts drag-and-drop of local image files and text.
- The canvas now accepts paste of image data, image file URLs, and plain text from the system clipboard.
- Dropped or pasted image content creates a new Image layer at the drop point, or at the canvas viewport center for paste.
- Clipboard image data without a source file is written to a temporary PNG under the system temp folder and used as the new Image layer source; normal title save/export embedding can then preserve the asset as usual.
- Dropped or pasted text creates a new Text layer at the drop point, or at the canvas viewport center for paste.
- External paste is used when the internal layer clipboard is empty, so existing copy/paste layer behavior remains unchanged.
