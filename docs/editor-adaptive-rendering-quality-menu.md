# Editor adaptive rendering quality menu

- Replaced the canvas Adaptive text button with a lightning icon.
- The button is highlighted while enabled and toggles with a normal click.
- Holding the button for 250 ms opens an exclusive quality menu: Auto, Full, 75%, 50%, 37,5%, and 25%.
- Enabled state and selected quality are persisted independently.
- Auto measures full-quality render cost and lowers resolution only during interaction when required.
- Fixed percentage modes rasterize the editor preview directly at the selected scale, including editor-local cached frames.
- Reduced-quality frames use a private CanvasPreview cache and never enter CacheManager, ensuring OBS output, live sources, disk cache, RAM prerender, and exported frames remain full quality.
- Changing mode, title, or model content invalidates only the editor-local reduced-quality cache.
