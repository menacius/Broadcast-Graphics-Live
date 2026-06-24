# Effects & Transitions library

All effect presets and transition presets live directly in this directory.
GPU shader assets live under `shaders/`. Physical subfolders do not define the
browser tree.

Each preset declares its virtual location with a slash-separated `category` field, for example:

```json
"category": "Effects/Blur & Sharpen"
```

```json
"category": "Transitions/Text/Slide"
```

Supported files:

- `.ogseffect` — layer effects; the category must begin with `Effects` or `Animation Presets`.
- `.osgtranst` — text transitions; the category must begin with `Transitions/Text`.
- `.osgtransg` — general transitions; the category must begin with `Transitions/General`.

The Effects & Presets dock builds its folder tree from these metadata paths. Empty categories are not created.
