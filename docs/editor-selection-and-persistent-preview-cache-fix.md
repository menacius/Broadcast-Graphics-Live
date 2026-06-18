# Editor selection and persistent preview cache fix

- Layer selection notifications are rejected before entering the modification pipeline when the title visual hash is unchanged.
- Selection no longer marks the title dirty, enables interactive bypass, saves live edits, invalidates frames, or starts a new prerender pass.
- The editor preview now promotes its currently visible frame directly from the persistent LZ4 disk cache into RAM.
- OBS realtime rendering remains disk-I/O-free through `requestFrameRealtime()`.
- Reopening the editor therefore reuses the existing disk prerender instead of rendering the timeline again while hydration catches up.
