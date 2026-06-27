# BGL Extension API / ABI v2

API v2 extends the original shader effect contract without exposing Qt or C++ ABI across the plugin boundary.

## New declarative capabilities

An effect can provide:

- `editor`: a host-owned editor schema;
- `presets`: an indexed preset library;
- `assets`: indexed textures, LUTs, icons and other reusable resources;
- `capabilities.compoundGraph`: ordered element graphs;
- `capabilities.keyframes`: host-owned animation tracks;
- validation and state-migration callbacks for native plugins.

## Animation contract

Parameters opt into animation with `"animatable": true`. Optional `interpolation` metadata lists supported modes. BGL stores tracks separately from static state:

```json
{
  "masterIntensity": [
    {"time": 0.0, "value": 0.0, "interpolation": "linear"},
    {"time": 1.0, "value": 1.5, "interpolation": "linear"}
  ],
  "elements.2.opacity": [
    {"time": 0.0, "value": 0.0, "interpolation": "hold"},
    {"time": 0.5, "value": 0.7, "interpolation": "linear"}
  ]
}
```

The host evaluates tracks at render time before binding uniforms. Scalar and vector values interpolate linearly; booleans, enums and strings use hold semantics. This keeps timeline behavior consistent across built-in, manifest and native extensions.

## Compound graph shader ABI

`elements` is flattened to uniforms:

- `elementCount`
- `element0_type`, `element0_position`, ...
- through `element15_*`

The same path names are used by animation tracks, so element properties can be keyframed without plugin-specific timeline code.
