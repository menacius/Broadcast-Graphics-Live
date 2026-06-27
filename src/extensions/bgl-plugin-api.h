#pragma once

#include <stdint.h>

#ifdef _WIN32
#define BGL_PLUGIN_EXPORT __declspec(dllexport)
#else
#define BGL_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

#define BGL_PLUGIN_API_VERSION_1 1u
#define BGL_PLUGIN_API_VERSION_2 2u
#define BGL_PLUGIN_API_VERSION_3 3u
#define BGL_PLUGIN_API_VERSION BGL_PLUGIN_API_VERSION_3

typedef struct bgl_host_api_v1 {
    uint32_t api_version;
    void (*log)(int level, const char *component, const char *message);
} bgl_host_api_v1;

typedef struct bgl_effect_descriptor_v1 {
    const char *id;
    const char *display_name;
    const char *category;
    const char *shader_path;
    const char *manifest_json;
} bgl_effect_descriptor_v1;

typedef struct bgl_plugin_descriptor_v1 {
    uint32_t api_version;
    const char *id;
    const char *name;
    const char *version;
    uint32_t effect_count;
    const bgl_effect_descriptor_v1 *effects;
} bgl_plugin_descriptor_v1;

/* ABI v2 remains pure C and append-only. The host owns every editor widget.
 * Plugins describe compound editors, presets and asset packs as UTF-8 JSON.
 * Optional callbacks validate/migrate opaque project state without exposing Qt. */
typedef int (*bgl_validate_state_v2_fn)(const char *effect_id, const char *state_json,
                                        char *error_utf8, uint32_t error_capacity);
typedef const char *(*bgl_migrate_state_v2_fn)(const char *effect_id,
                                               uint32_t from_schema_version,
                                               const char *state_json);
typedef void (*bgl_release_string_v2_fn)(const char *value);

typedef struct bgl_effect_descriptor_v2 {
    bgl_effect_descriptor_v1 v1;
    uint32_t schema_version;
    const char *editor_schema_json; /* declarative host-owned editor */
    const char *preset_index_json;  /* categories + relative preset files */
    const char *asset_index_json;   /* textures/LUTs/icons + metadata */
    const char *capabilities_json;  /* compoundGraph, customAssets, keyframes, etc. */
    const char *animation_schema_json; /* animatable paths, interpolation/easing policy */
} bgl_effect_descriptor_v2;

/* ABI v3 adds host-rendered, host-hit-tested canvas controls. The JSON schema
 * describes handles by parameter path and coordinate space; plugins never
 * receive QWidget/QPainter pointers and remain ABI-stable across Qt versions. */
typedef struct bgl_effect_descriptor_v3 {
    bgl_effect_descriptor_v2 v2;
    const char *canvas_handles_schema_json;
} bgl_effect_descriptor_v3;

typedef struct bgl_plugin_descriptor_v2 {
    bgl_plugin_descriptor_v1 v1;
    uint32_t descriptor_size;
    uint32_t effect_v2_count;
    const bgl_effect_descriptor_v2 *effects_v2;
    bgl_validate_state_v2_fn validate_state;
    bgl_migrate_state_v2_fn migrate_state;
    bgl_release_string_v2_fn release_string;
} bgl_plugin_descriptor_v2;

typedef struct bgl_plugin_descriptor_v3 {
    bgl_plugin_descriptor_v2 v2;
    uint32_t effect_v3_count;
    const bgl_effect_descriptor_v3 *effects_v3;
} bgl_plugin_descriptor_v3;

typedef const bgl_plugin_descriptor_v1 *(*bgl_plugin_query_v1_fn)(const bgl_host_api_v1 *host);
typedef const bgl_plugin_descriptor_v2 *(*bgl_plugin_query_v2_fn)(const bgl_host_api_v1 *host);
typedef const bgl_plugin_descriptor_v3 *(*bgl_plugin_query_v3_fn)(const bgl_host_api_v1 *host);

BGL_PLUGIN_EXPORT const bgl_plugin_descriptor_v1 *bgl_plugin_query_v1(const bgl_host_api_v1 *host);
/* v2 plugins export this symbol; they may also export v1 for older BGL hosts. */
BGL_PLUGIN_EXPORT const bgl_plugin_descriptor_v2 *bgl_plugin_query_v2(const bgl_host_api_v1 *host);
BGL_PLUGIN_EXPORT const bgl_plugin_descriptor_v3 *bgl_plugin_query_v3(const bgl_host_api_v1 *host);
