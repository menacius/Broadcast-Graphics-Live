# Development Version 006 — Scene-mask lifecycle fix

The configured OBS scene used by a scene-mask layer is rendered manually rather than through a normal scene item. The source therefore must explicitly maintain both OBS lifecycle references: `obs_source_inc_active()` and `obs_source_inc_showing()`, with balanced decrements.

Static titles without exposed live-text layers are now considered continuously cue-valid while their Broadcast Graphics Live source is active and shown. Titles with exposed live-text layers retain cue-dependent activation and release their scene-mask scenes after uncue. The source `show` callback also performs immediate reconciliation so Studio Preview and Program do not wait for an unrelated model or cue update.
