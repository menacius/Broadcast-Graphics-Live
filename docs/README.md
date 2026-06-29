# Broadcast Graphics Live documentation

This directory contains the canonical documentation for `v0.8.7-alpha`. Older one-feature notes and per-development-version reports were merged into the thematic guides below so that each subject has one maintained source of truth.

| Document | Purpose |
| --- | --- |
| [USER_GUIDE.md](USER_GUIDE.md) | Installation, first launch, basic title creation, cueing, and everyday workflows. |
| [EDITOR_WORKFLOW.md](EDITOR_WORKFLOW.md) | Layers, canvas tools, panels, timeline, grouping, parenting, mattes, assets, presets, and editor interaction. |
| [TEXT_AND_LIVE_DATA.md](TEXT_AND_LIVE_DATA.md) | Rich text, inline editing, clocks, tickers, auto styling, exposed fields, live cues, and external data. |
| [RENDERING_AND_CACHE.md](RENDERING_AND_CACHE.md) | Editor/source rendering parity, GPU pipeline, RAM/disk cache, prerendering, invalidation, and performance behavior. |
| [EFFECTS_AND_EXTENSIONS.md](EFFECTS_AND_EXTENSIONS.md) | Effect stack, presets, transitions, built-in effects, extension manifests, API/ABI, and compatibility rules. |
| [ARCHITECTURE_AND_BUILD.md](ARCHITECTURE_AND_BUILD.md) | Source ownership, module boundaries, build systems, packaging, tests, audits, and contribution rules. |
| [CHANGELOG.md](CHANGELOG.md) | Consolidated development history and current revision notes. |

## Documentation rules

- Update the canonical thematic document instead of creating a new one-off note.
- Put user-visible changes in `CHANGELOG.md`.
- Put machine-readable inventories under `tools/`, not in this directory.
- Keep implementation details close to the owning source module when they only matter to maintainers of that module.
