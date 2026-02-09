# Custom Scenery Packages

Place each custom scenery add-on in its own subdirectory here.

## Package Structure

```
scenery/custom/
  my_addon/
    package.json      ← required
    manifest.json     ← tile/object index
    *.glb             ← 3D assets
```

## package.json format

```json
{
  "name": "My Custom Buildings",
  "type": "buildings",
  "version": "1.0.0",
  "author": "Your Name",
  "description": "Custom building models for Schaffhausen old town",
  "coordinate_system": "LV95",
  "conversion_origin_e": 2681968,
  "conversion_origin_n": 1282869
}
```

Supported types: `buildings`, `terrain_overlay`, `objects`
