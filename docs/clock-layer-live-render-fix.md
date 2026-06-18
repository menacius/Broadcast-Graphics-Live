# Clock layer live rendering fix

Clock and ticker titles are deliberately excluded from prerendering and frame caching because their content changes continuously. The editor preview was still requesting those titles through the cached-frame API, which correctly returned no frame for a non-cacheable title and therefore made the clock text invisible.

The canvas preview now routes every title containing a Clock or Ticker layer directly through the live uncached renderer on each dynamic refresh. This keeps the cache exclusion intact while restoring visible, continuously updating clock content in the editor. An accidental duplicate `pix_h` declaration in the canvas paint path was also removed.
