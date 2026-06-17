# Live Cue Cache Progress Icon Flicker Fix

Queued and Rendering are scheduler states within the same visible prerender phase. They now share one stable tint, and UI update coalescing normalizes both states before comparing notifications. The cache badge is therefore replaced only when its 25% progress bucket changes or when it enters a materially different state such as ready, stale, or disabled.

Progress icon mapping remains:

- 0–24%: `cache-queued-0-25.svg`
- 25–49%: `cache-progress-25-50.svg`
- 50–74%: `cache-progress-50-75.svg`
- 75–99%: `cache-progress-75-100.svg`
- 100%: `cache-ok.svg`
