# Built-in Effects Extension Migration

The built-in effect set is now registered through the same versioned effect catalog used by third-party extensions.

## Canonical identity

Every built-in effect has a stable namespaced ID (`bgl.builtin.*`). New effects, presets and project saves always carry this ID. Legacy project files containing only the numeric `LayerEffectType` are migrated in memory during loading and are written back with the canonical ID on the next save.

## Unified discovery and UI

`BglEffectExtensionCatalog` registers core effects before scanning manifest and native plugin roots. The Add Effect menu is generated from catalog metadata for both built-in and external providers, including hierarchical categories, provider identity and version.

## Unified execution

The GPU renderer resolves effect execution by stable ID through `TitleEffectRegistry::compile(const std::string &)`. Core IDs resolve to the corresponding built-in implementation, while external IDs resolve to manifest/native-provider shader assets. Effects that intentionally share the Gaussian implementation still resolve the implementation ID through the registry rather than bypassing it.

## Compatibility

The numeric type remains as a compatibility/runtime adapter for the current specialized property and compositing code. It is no longer the persisted identity or the source of the Add Effect catalog. Unknown external IDs and their raw parameter JSON continue to round-trip even when their provider is missing.
