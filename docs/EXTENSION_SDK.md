# Broadcast Graphics Live Extension SDK v1

BGL now separates effect identity from implementation. Extension effects use globally unique string IDs and are discovered from the installed data directory and the writable OBS module configuration directory.

## Manifest-only GPU effects

Place a folder under `broadcast-graphics-live/extensions/` containing a `*.bgl-effect.json` manifest and an OBS `.effect` shader. The manifest declares the ID, UI metadata, parameter schema, defaults and techniques. This is the preferred path for portable, sandboxable GPU effects.

Supported parameter metadata in API v1: `float`, `int`, `bool`, `color`, `enum`, `texture`, `point`, and `string`. Each parameter may declare labels, ranges, steps, defaults and whether it is animatable.

## Native plugins

Native plugins are optional shared libraries loaded only from the user configuration extension directory. They export `bgl_plugin_query_v1` and return a `bgl_plugin_descriptor_v1`. The stable C ABI is in `src/extensions/bgl-plugin-api.h`; plugins must not include private C++ editor or renderer headers.

## Compatibility rules

- IDs are permanent and must be namespaced.
- API version mismatches are rejected.
- Duplicate IDs are ignored and reported.
- Project files retain the extension ID and raw parameter JSON, so missing plugins do not destroy settings.
- Built-in effects remain readable through their legacy numeric type while also gaining stable IDs.

## Security

Manifest effects are shader/data packages. Native plugins execute in the OBS process and should only be installed from trusted sources. A later process-isolated plugin tier can use the same descriptors without changing project files.
