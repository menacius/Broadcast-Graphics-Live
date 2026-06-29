#include "title-effect-registry.h"
#include "extensions/effect-extension-catalog.h"

#include "title-logger.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/bmem.h>

#include <QString>

#include <algorithm>

namespace {

static constexpr const char *kEmbeddedLensFlareEffect = R"BGLFX(uniform float4x4 ViewProj;
uniform texture2d image;
uniform float2 texelSize;
uniform float4 effectColor;
uniform float4 secondaryColor;
uniform float opacity;
uniform float amount;
uniform float scale;
uniform float radius;
uniform float spread;
uniform float falloff;
uniform float2 center;
uniform float angle;
uniform float ghostCount;
uniform float profile;

sampler_state textureSampler {
    Filter = Linear;
    AddressU = Clamp;
    AddressV = Clamp;
};

struct VertDataIn {
    float4 pos : POSITION;
    float2 uv : TEXCOORD0;
};

struct VertDataOut {
    float4 pos : POSITION;
    float2 uv : TEXCOORD0;
};

VertDataOut VSDefault(VertDataIn v_in)
{
    VertDataOut vert_out;
    vert_out.pos = mul(float4(v_in.pos.xyz, 1.0), ViewProj);
    vert_out.uv = v_in.uv;
    return vert_out;
}

float flare_disc(float2 flare_pos, float discRadius, float feather)
{
    float safe_radius = max(discRadius, 0.0001);
    float safe_feather = max(feather, 0.0001);
    return 1.0 - smoothstep(safe_radius - safe_feather,
                            safe_radius + safe_feather,
                            length(flare_pos));
}

float flare_halo(float distance_value, float halo_radius, float decay)
{
    float normalized = distance_value / max(halo_radius, 0.0001);
    return 1.0 / (1.0 + normalized * normalized * max(decay, 0.05));
}

float4 PSLensFlare(VertDataOut v_in) : TARGET
{
    float4 base = image.Sample(textureSampler, v_in.uv);

    /* Keep the procedural geometry in square pixel space.  This avoids the
     * DX11/OpenGL divergence caused by deriving the flare shape directly from
     * non-square UV coordinates. */
    float aspect = max(texelSize.y / max(texelSize.x, 0.000001), 0.0001);
    float2 flare_pos = v_in.uv - center;
    flare_pos.x *= aspect;

    float safe_scale = max(scale, 0.001);
    float safe_radius = max(radius * safe_scale, 0.001);
    float distance_value = length(flare_pos);
    float safe_falloff = max(falloff, 0.05);

    float core = flare_disc(flare_pos, safe_radius * 0.38,
                            safe_radius * 0.16);
    float glow = flare_halo(distance_value, safe_radius * 1.55,
                            safe_falloff * 2.0);
    float outer_halo = flare_disc(flare_pos, safe_radius * 2.35,
                                  safe_radius * 0.28) * 0.22;

    float radians = angle * 0.01745329252;
    float2 axis = float2(cos(radians), sin(radians));
    float2 perpendicular = float2(-axis.y, axis.x);
    float along_axis = abs(dot(flare_pos, axis));
    float across_axis = abs(dot(flare_pos, perpendicular));
    float ray_width = safe_radius * 0.055;
    float ray_length = safe_radius * 8.0;
    float primary_ray = (1.0 - smoothstep(0.0, ray_width, across_axis)) *
                        (1.0 - smoothstep(safe_radius * 0.25,
                                          ray_length, along_axis));
    float secondary_ray = (1.0 - smoothstep(0.0, ray_width * 0.65,
                                             along_axis)) *
                          (1.0 - smoothstep(safe_radius * 0.25,
                                            ray_length * 0.72,
                                            across_axis));

    /* Ghosts lie on the optical axis between the flare and the frame centre.
     * They are written explicitly rather than through a dynamic loop because
     * OBS must compile the same effect for D3D11 and OpenGL. */
    float2 optical_axis = float2(0.5, 0.5) - center;
    optical_axis.x *= aspect;
    float safe_spread = max(spread, 0.0);
    float ghost1 = flare_disc(flare_pos - optical_axis * (0.55 + safe_spread * 0.30),
                              safe_radius * 0.42, safe_radius * 0.16);
    float ghost2 = flare_disc(flare_pos - optical_axis * (1.15 + safe_spread * 0.60),
                              safe_radius * 0.28, safe_radius * 0.12) *
                   step(2.5, ghostCount);
    float ghost3 = flare_disc(flare_pos - optical_axis * (1.85 + safe_spread * 0.95),
                              safe_radius * 0.36, safe_radius * 0.14) *
                   step(4.5, ghostCount);
    float ghost4 = flare_disc(flare_pos + optical_axis * (0.42 + safe_spread * 0.20),
                              safe_radius * 0.20, safe_radius * 0.10) *
                   step(6.5, ghostCount);

    float anamorphic = (1.0 - smoothstep(0.0, safe_radius * 0.045,
                                         abs(flare_pos.y))) *
                        (1.0 - smoothstep(safe_radius * 0.35,
                                          safe_radius * 11.0,
                                          abs(flare_pos.x)));

    float profile_value = profile;
    float anamorphic_profile = 1.0 - step(0.5, abs(profile_value - 1.0));
    float warm_profile = 1.0 - step(0.5, abs(profile_value - 2.0));
    float scifi_profile = 1.0 - step(0.5, abs(profile_value - 3.0));
    float subtle_profile = 1.0 - step(0.5, abs(profile_value - 4.0));

    float flare_shape = core * 1.35 + glow * 0.58 + outer_halo +
                        primary_ray * (0.34 + scifi_profile * 0.30) +
                        secondary_ray * 0.18 +
                        ghost1 * 0.42 + ghost2 * 0.30 +
                        ghost3 * 0.24 + ghost4 * 0.18 +
                        anamorphic * (anamorphic_profile * 0.85 +
                                      scifi_profile * 0.55);
    flare_shape *= lerp(1.0, 0.48, subtle_profile);

    float color_mix = saturate(distance_value /
                               max(safe_radius * 4.0, 0.001));
    float3 flare_color = lerp(effectColor.rgb,
                              secondaryColor.rgb,
                              color_mix);
    flare_color = lerp(flare_color,
                       float3(1.0, 0.48, 0.16),
                       warm_profile * 0.38);

    float alpha_scale = max(effectColor.a, secondaryColor.a);
    float flare_alpha = saturate(flare_shape * max(amount, 0.0) *
                                 clamp(opacity, 0.0, 1.0) * alpha_scale);

    /* OBS textures use premultiplied alpha.  The flare is generative, so it
     * must be able to create visible pixels even when the input is transparent
     * while still preserving the source alpha everywhere else. */
    float output_alpha = saturate(base.a + flare_alpha * (1.0 - base.a));
    float3 output_rgb = saturate(base.rgb + flare_color * flare_alpha);
    return float4(output_rgb, output_alpha);
}

technique Draw
{
    pass
    {
        vertex_shader = VSDefault(v_in);
        pixel_shader = PSLensFlare(v_in);
    }
}
)BGLFX";
static constexpr const char *kEmbeddedVignetteEffect = R"BGLFX(uniform float4x4 ViewProj;
uniform texture2d image;
uniform float2 texelSize;
uniform float4 effectColor;
uniform float opacity;
uniform float amount;
uniform float scale;
uniform float softness;
uniform float roundness;
uniform float2 center;
uniform int invert;

sampler_state textureSampler {
    Filter = Linear;
    AddressU = Clamp;
    AddressV = Clamp;
};

struct VertDataIn {
    float4 pos : POSITION;
    float2 uv : TEXCOORD0;
};

struct VertDataOut {
    float4 pos : POSITION;
    float2 uv : TEXCOORD0;
};

VertDataOut VSDefault(VertDataIn v_in)
{
    VertDataOut vert_out;
    vert_out.pos = mul(float4(v_in.pos.xyz, 1.0), ViewProj);
    vert_out.uv = v_in.uv;
    return vert_out;
}

float4 PSVignette(VertDataOut v_in) : TARGET
{
    float4 base = image.Sample(textureSampler, v_in.uv);
    float2 p = (v_in.uv - center) * 2.0;
    float aspect = max(texelSize.y / max(texelSize.x, 0.000001), 0.0001);
    float shape_mix = saturate(roundness * 0.5 + 0.5);
    p.x *= lerp(aspect, 1.0, shape_mix);

    float d = length(p) / max(scale, 0.001);
    float feather = max(softness, 0.0001);
    float mask = smoothstep(1.0 - feather, 1.0 + feather, d);
    if (invert != 0)
        mask = 1.0 - mask;

    float mix_amount = saturate(mask * max(amount, 0.0) *
                                clamp(opacity, 0.0, 1.0) * effectColor.a);
    float3 straight = base.a > 0.0001 ? base.rgb / base.a : float3(0.0, 0.0, 0.0);
    straight = lerp(straight, effectColor.rgb, mix_amount);
    return float4(clamp(straight, 0.0, 1.0) * base.a, base.a);
}

technique Draw
{
    pass
    {
        vertex_shader = VSDefault(v_in);
        pixel_shader = PSVignette(v_in);
    }
}
)BGLFX";
static constexpr const char *kEmbeddedNoiseEffect = R"BGLFX(uniform float4x4 ViewProj;
uniform texture2d image;
uniform float opacity;
uniform float amount;
uniform float scale;
uniform float softness;
uniform float complexity;
uniform float speed;
uniform float time;
uniform float evolution;
uniform float seed;
uniform int profile;
uniform int animatedNoise;
uniform int monochrome;
uniform int invert;

sampler_state textureSampler {
    Filter = Linear;
    AddressU = Clamp;
    AddressV = Clamp;
};

struct VertDataIn {
    float4 pos : POSITION;
    float2 uv : TEXCOORD0;
};

struct VertDataOut {
    float4 pos : POSITION;
    float2 uv : TEXCOORD0;
};

VertDataOut VSDefault(VertDataIn v_in)
{
    VertDataOut vert_out;
    vert_out.pos = mul(float4(v_in.pos.xyz, 1.0), ViewProj);
    vert_out.uv = v_in.uv;
    return vert_out;
}

float hash_noise(float2 p)
{
    return frac(sin(dot(p, float2(12.9898, 78.233))) * 43758.5453);
}

float smooth_noise(float2 p)
{
    float2 cell = floor(p);
    float2 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash_noise(cell);
    float b = hash_noise(cell + float2(1.0, 0.0));
    float c = hash_noise(cell + float2(0.0, 1.0));
    float d = hash_noise(cell + float2(1.0, 1.0));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

float layered_noise(float2 p, float layers)
{
    float value = smooth_noise(p) * 0.55;
    value += smooth_noise(p * 2.03 + 17.0) * 0.25 * step(1.5, layers);
    value += smooth_noise(p * 4.09 + 31.0) * 0.125 * step(2.5, layers);
    value += smooth_noise(p * 8.17 + 47.0) * 0.0625 * step(3.5, layers);
    return saturate(value / 0.9875);
}

float4 PSNoise(VertDataOut v_in) : TARGET
{
    float4 base = image.Sample(textureSampler, v_in.uv);
    float phase = evolution + seed * 17.13;
    if (animatedNoise != 0)
        phase += time * speed;
    float2 p = v_in.uv * max(scale, 0.001) * 512.0 + float2(phase, phase * 0.731);

    float value = hash_noise(floor(p));
    if (profile == 1)
        value = smooth_noise(p);
    else if (profile == 2)
        value = saturate((smooth_noise(p) + smooth_noise(p * 1.73 + 9.1) - 1.0) * 1.6 + 0.5);
    else if (profile == 3 || profile == 5)
        value = layered_noise(p, clamp(complexity, 1.0, 4.0));
    else if (profile == 4)
        value = step(0.92, hash_noise(floor(p)));

    value = lerp(value, 0.5, saturate(softness));
    if (invert != 0)
        value = 1.0 - value;

    float strength = max(amount, 0.0) * clamp(opacity, 0.0, 1.0);
    float3 noise_rgb = float3(value, value, value);
    if (monochrome == 0) {
        noise_rgb.g = hash_noise(p + float2(37.0, 17.0));
        noise_rgb.b = hash_noise(p + float2(91.0, 53.0));
    }
    float3 delta = (noise_rgb - 0.5) * strength * base.a;
    return float4(clamp(base.rgb + delta, 0.0, 1.0), base.a);
}

technique Draw
{
    pass
    {
        vertex_shader = VSDefault(v_in);
        pixel_shader = PSNoise(v_in);
    }
}
)BGLFX";
static constexpr const char *kEmbeddedRoughenEdgesEffect = R"BGLFX(uniform float4x4 ViewProj;
uniform texture2d image;
uniform float2 texelSize;
uniform float opacity;
uniform float amount;
uniform float scale;
uniform float softness;
uniform float complexity;
uniform float evolution;
uniform float seed;
uniform int invert;

sampler_state textureSampler {
    Filter = Linear;
    AddressU = Clamp;
    AddressV = Clamp;
};

struct VertDataIn {
    float4 pos : POSITION;
    float2 uv : TEXCOORD0;
};

struct VertDataOut {
    float4 pos : POSITION;
    float2 uv : TEXCOORD0;
};

VertDataOut VSDefault(VertDataIn v_in)
{
    VertDataOut vert_out;
    vert_out.pos = mul(float4(v_in.pos.xyz, 1.0), ViewProj);
    vert_out.uv = v_in.uv;
    return vert_out;
}

float rough_hash(float2 p)
{
    return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453);
}

float rough_value(float2 p)
{
    float2 cell = floor(p);
    float2 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = rough_hash(cell);
    float b = rough_hash(cell + float2(1.0, 0.0));
    float c = rough_hash(cell + float2(0.0, 1.0));
    float d = rough_hash(cell + float2(1.0, 1.0));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

float rough_layers(float2 p, float layers)
{
    float value = rough_value(p) * 0.60;
    value += rough_value(p * 2.03 + 19.0) * 0.27 * step(1.5, layers);
    value += rough_value(p * 4.11 + 37.0) * 0.13 * step(2.5, layers);
    return value;
}

float4 PSRoughen(VertDataOut v_in) : TARGET
{
    float4 original = image.Sample(textureSampler, v_in.uv);
    float2 p = v_in.uv * max(scale, 0.001) +
               float2(evolution + seed * 3.1, evolution * 0.73 + seed * 7.7);
    float n1 = rough_layers(p, clamp(complexity, 1.0, 3.0)) - 0.5;
    float n2 = rough_layers(p + float2(41.7, 23.9), clamp(complexity, 1.0, 3.0)) - 0.5;
    if (invert != 0) {
        n1 = -n1;
        n2 = -n2;
    }

    float displacement = max(amount, 0.0) * 64.0;
    float2 offset = float2(n1, n2) * texelSize * displacement;
    float4 displaced = image.Sample(textureSampler, v_in.uv + offset);

    float a0 = original.a;
    float a1 = image.Sample(textureSampler, v_in.uv + float2(texelSize.x, 0.0)).a;
    float a2 = image.Sample(textureSampler, v_in.uv - float2(texelSize.x, 0.0)).a;
    float a3 = image.Sample(textureSampler, v_in.uv + float2(0.0, texelSize.y)).a;
    float a4 = image.Sample(textureSampler, v_in.uv - float2(0.0, texelSize.y)).a;
    float edge = max(max(abs(a0 - a1), abs(a0 - a2)), max(abs(a0 - a3), abs(a0 - a4)));
    float edge_mask = smoothstep(0.0, max(softness, 0.001), edge + abs(n1) * max(amount, 0.0));
    float keep = saturate(1.0 - edge_mask * max(amount, 0.0));
    float old_alpha = displaced.a;
    displaced.a *= keep;
    if (old_alpha > 0.0001)
        displaced.rgb *= displaced.a / old_alpha;
    return lerp(original, displaced, clamp(opacity, 0.0, 1.0));
}

technique Draw
{
    pass
    {
        vertex_shader = VSDefault(v_in);
        pixel_shader = PSRoughen(v_in);
    }
}
)BGLFX";

static const char *embedded_effect_source(LayerEffectType type)
{
    switch (type) {
    case LayerEffectType::LensFlare: return kEmbeddedLensFlareEffect;
    case LayerEffectType::Vignette: return kEmbeddedVignetteEffect;
    case LayerEffectType::Noise: return kEmbeddedNoiseEffect;
    case LayerEffectType::RoughenEdges: return kEmbeddedRoughenEdgesEffect;
    default: return nullptr;
    }
}

static const char *embedded_effect_name(LayerEffectType type)
{
    switch (type) {
    case LayerEffectType::LensFlare: return "embedded-bgl-lens-flare.effect";
    case LayerEffectType::Vignette: return "embedded-bgl-vignette.effect";
    case LayerEffectType::Noise: return "embedded-bgl-noise.effect";
    case LayerEffectType::RoughenEdges: return "embedded-bgl-roughen-edges.effect";
    default: return "embedded-bgl-effect.effect";
    }
}

const std::vector<TitleEffectDefinition> kEffectDefinitions = {
    {LayerEffectType::BackgroundColor, "bgl.builtin.background-color", "background-color", "Background Color", "Built-in/Appearance", "effect-transitions/shaders/background-color/background-color.effect", false},
    {LayerEffectType::Outline, "bgl.builtin.outline", "outline", "Outline", "Built-in/Appearance", "effect-transitions/shaders/outline/outline.effect", false},
    {LayerEffectType::DropShadow, "bgl.builtin.drop-shadow", "drop-shadow", "Drop Shadow", "Built-in/Shadows & Glows", "effect-transitions/shaders/drop-shadow/drop-shadow.effect", false},
    {LayerEffectType::LongShadow, "bgl.builtin.long-shadow", "long-shadow", "Long Shadow", "Built-in/Shadows & Glows", "effect-transitions/shaders/long-shadow/long-shadow.effect", false},
    {LayerEffectType::BrightnessContrast, "bgl.builtin.brightness-contrast", "brightness-contrast", "Brightness & Contrast", "Built-in/Color", "effect-transitions/shaders/brightness-contrast/brightness-contrast.effect", false},
    {LayerEffectType::Saturation, "bgl.builtin.saturation", "saturation", "Saturation", "Built-in/Color", "effect-transitions/shaders/saturation/saturation.effect", false},
    {LayerEffectType::ColorOverlay, "bgl.builtin.color-overlay", "color-overlay", "Color Overlay", "Built-in/Color", "effect-transitions/shaders/color-overlay/color-overlay.effect", false},
    {LayerEffectType::Glow, "bgl.builtin.glow", "glow", "Glow", "Built-in/Shadows & Glows", "effect-transitions/shaders/glow/glow.effect", false},
    {LayerEffectType::InnerGlow, "bgl.builtin.inner-glow", "inner-glow", "Inner Glow", "Built-in/Shadows & Glows", "effect-transitions/shaders/inner-glow/inner-glow.effect", false},
    {LayerEffectType::InnerShadow, "bgl.builtin.inner-shadow", "inner-shadow", "Inner Shadow", "Built-in/Shadows & Glows", "effect-transitions/shaders/inner-shadow/inner-shadow.effect", false},
    {LayerEffectType::Blur, "bgl.builtin.blur", "blur", "Blur", "Built-in/Blur", "effect-transitions/shaders/blur/blur.effect", false},
    {LayerEffectType::MotionBlur, "bgl.builtin.motion-blur", "motion-blur", "Motion Blur", "Built-in/Blur", "effect-transitions/shaders/motion-blur/motion-blur.effect", false},
    {LayerEffectType::Bloom, "bgl.builtin.bloom", "bloom", "Bloom", "Built-in/Shadows & Glows", "effect-transitions/shaders/bloom/bloom.effect", false},
    {LayerEffectType::Emboss, "bgl.builtin.emboss", "emboss", "Emboss", "Built-in/Stylize", "effect-transitions/shaders/emboss/emboss.effect", false},
    {LayerEffectType::LensFlare, "bgl.builtin.lens-flare", "lens-flare", "Lens Flare", "Built-in/Generate", "effect-transitions/shaders/lens-flare/lens-flare.effect", true},
    {LayerEffectType::Vignette, "bgl.builtin.vignette", "vignette", "Vignette", "Built-in/Stylize", "effect-transitions/shaders/vignette/vignette.effect", true},
    {LayerEffectType::Noise, "bgl.builtin.noise", "noise", "Noise", "Built-in/Generate", "effect-transitions/shaders/noise/noise.effect", true},
    {LayerEffectType::RoughenEdges, "bgl.builtin.roughen-edges", "roughen-edges", "Roughen Edges", "Built-in/Stylize", "effect-transitions/shaders/roughen-edges/roughen-edges.effect", true},
    {LayerEffectType::FourColorGradient, "bgl.builtin.4-color-gradient", "4-color-gradient", "4-Color Gradient", "Built-in/Generate", "effect-transitions/shaders/4-color-gradient/4-color-gradient.effect", false},
};

} // namespace

TitleEffectRegistry::~TitleEffectRegistry()
{
    reset();
}

void TitleEffectRegistry::reset()
{
    for (CompiledEffect &compiled : compiled_) {
        if (compiled.effect)
            gs_effect_destroy(compiled.effect);
    }
    compiled_.clear();
}

gs_effect_t *TitleEffectRegistry::compile(LayerEffectType type)
{
    last_error_ = nullptr;
    const TitleEffectDefinition *def = definition(type);
    if (!def) {
        last_error_ = "Unknown effect type.";
        return nullptr;
    }

    auto existing = std::find_if(compiled_.begin(), compiled_.end(),
                                 [def](const CompiledEffect &compiled) {
                                     return compiled.stable_id == def->stable_id;
                                 });
    if (existing != compiled_.end())
        return existing->effect;

    /* The procedural effects are embedded deliberately. Users often update
     * only the plugin DLL while an older data directory remains installed;
     * in that situation file-only effects silently became no-ops. Keeping a
     * binary-owned source makes editor, live output and prerender use the same
     * implementation regardless of deployment layout. The external asset is
     * still shipped as readable reference and as a fallback for development. */
    if (const char *embedded = embedded_effect_source(type)) {
        char *errors = nullptr;
        gs_effect_t *effect = gs_effect_create(
            embedded, embedded_effect_name(type), &errors);
        if (effect) {
            if (errors)
                bfree(errors);
            BGL_LOG_INFO("Effects", QStringLiteral("Compiled embedded procedural effect %1")
                                        .arg(QString::fromUtf8(def->stable_id)));
            compiled_.push_back({type, def->stable_id, effect});
            return effect;
        }
        BGL_LOG_WARNING("Effects", QStringLiteral("Embedded procedural effect %1 failed to compile: %2; trying installed asset")
                                       .arg(QString::fromUtf8(def->stable_id),
                                            QString::fromUtf8(errors ? errors : "unknown shader error")));
        blog(LOG_WARNING,
             "[Broadcast Graphics Live] Embedded effect '%s' failed to compile: %s; trying installed asset",
             def->stable_id, errors ? errors : "unknown shader error");
        if (errors)
            bfree(errors);
    }

    char *path = obs_module_file(def->relative_path);
    if (!path) {
        BGL_LOG_WARNING("Effects", QStringLiteral("Effect asset path could not be resolved for %1 (%2)")
                                       .arg(QString::fromUtf8(def->stable_id),
                                            QString::fromUtf8(def->relative_path)));
        blog(LOG_WARNING,
             "[Broadcast Graphics Live] Effect asset path could not be resolved for '%s' (%s)",
             def->stable_id, def->relative_path);
        last_error_ = "Effect asset path could not be resolved.";
        return nullptr;
    }

    char *errors = nullptr;
    gs_effect_t *effect = gs_effect_create_from_file(path, &errors);
    if (!effect) {
        BGL_LOG_WARNING("Effects", QStringLiteral("Failed to compile effect %1 from %2: %3")
                                       .arg(QString::fromUtf8(def->stable_id),
                                            QString::fromUtf8(path),
                                            QString::fromUtf8(errors ? errors : "unknown shader error")));
        blog(LOG_WARNING,
             "[Broadcast Graphics Live] Effect '%s' failed to compile from '%s': %s",
             def->stable_id, path, errors ? errors : "unknown shader error");
        last_error_ = "Effect shader could not be compiled.";
        if (errors)
            bfree(errors);
        bfree(path);
        return nullptr;
    }

    if (errors)
        bfree(errors);
    BGL_LOG_DEBUG("Effects", QStringLiteral("Compiled effect %1 from %2")
                                 .arg(QString::fromUtf8(def->stable_id), QString::fromUtf8(path)));
    bfree(path);

    compiled_.push_back({type, def->stable_id, effect});
    return effect;
}


gs_effect_t *TitleEffectRegistry::compile(const std::string &stable_id)
{
    if (stable_id.empty()) {
        last_error_ = "Empty effect extension id.";
        return nullptr;
    }
    LayerEffectType built_in_type{};
    if (BglEffectExtensionCatalog::builtInTypeForId(QString::fromStdString(stable_id), &built_in_type))
        return compile(built_in_type);
    auto existing = std::find_if(compiled_.begin(), compiled_.end(),
                                 [&](const CompiledEffect &compiled) {
                                     return compiled.stable_id == stable_id;
                                 });
    if (existing != compiled_.end())
        return existing->effect;

    auto &catalog = BglEffectExtensionCatalog::instance();
    if (catalog.effects().empty())
        catalog.reload();
    const auto *definition = catalog.find(QString::fromStdString(stable_id));
    if (definition && definition->builtIn)
        return compile(definition->builtInType);
    if (!definition) {
        last_error_ = "Effect extension is not installed.";
        return nullptr;
    }
    const QByteArray path = definition->shaderPath.toUtf8();
    char *errors = nullptr;
    gs_effect_t *effect = gs_effect_create_from_file(path.constData(), &errors);
    if (!effect) {
        BGL_LOG_WARNING("Extensions", QStringLiteral("Failed to compile extension effect %1: %2")
                                       .arg(definition->id, QString::fromUtf8(errors ? errors : "unknown shader error")));
        if (errors) bfree(errors);
        last_error_ = "Extension shader could not be compiled.";
        return nullptr;
    }
    if (errors) bfree(errors);
    compiled_.push_back({LayerEffectType::BackgroundColor, stable_id, effect});
    BGL_LOG_INFO("Extensions", QStringLiteral("Loaded effect extension %1 from %2")
                                 .arg(definition->id, definition->shaderPath));
    return effect;
}

const std::vector<TitleEffectDefinition> &TitleEffectRegistry::definitions()
{
    return kEffectDefinitions;
}

const TitleEffectDefinition *TitleEffectRegistry::definition(LayerEffectType type)
{
    auto it = std::find_if(kEffectDefinitions.begin(), kEffectDefinitions.end(),
                           [type](const TitleEffectDefinition &def) {
                               return def.type == type;
                           });
    return it == kEffectDefinitions.end() ? nullptr : &*it;
}
