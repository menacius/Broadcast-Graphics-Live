# Step 3 implementation summary

Implemented alpha-cropped cached frame payloads.

- Detects the non-zero alpha bounding rectangle after worker-side render conversion.
- Stores only cropped straight-alpha BGRA pixels in RAM.
- Persists crop origin and original canvas dimensions in disk cache format version 4.
- Compresses only the cropped payload with LZ4.
- Represents fully transparent frames as a 1x1 transparent payload.
- Preserves temporal alias sharing and unique-payload RAM accounting.
- Reconstructs a full frame only for current OBS upload compatibility and dirty-tile merge operations.
- Rejects and removes incompatible older cache files during index rebuild.

The next optimization step can remove the compatibility reconstruction by drawing the cropped GPU texture at its stored canvas offset.
