#include <cassert>
#include <iostream>
#include <string>

#include "source_bundle_reader.h"

namespace {

void require(const std::string &source, const char *needle)
{
    if (source.find(needle) == std::string::npos) {
        std::cerr << "missing: " << needle << '\n';
        assert(false);
    }
}

void require_before(const std::string &source, const char *first,
                    const char *second)
{
    const std::size_t first_pos = source.find(first);
    const std::size_t second_pos = source.find(second);
    if (first_pos == std::string::npos || second_pos == std::string::npos ||
        first_pos >= second_pos) {
        std::cerr << "expected order: " << first << " before " << second << '\n';
        assert(false);
    }
}

void shader_contract(const std::string &shader, const char *pixel_shader)
{
    require(shader, "uniform float4x4 ViewProj;");
    require(shader, "uniform texture2d image;");
    require(shader, "technique Draw");
    require(shader, "vertex_shader = VSDefault(v_in);");
    require(shader, pixel_shader);
}
} // namespace

int main(int argc, char **argv)
{
    assert(argc == 8);
    const std::string registry = read_file(argv[1]);
    const std::string title_source = read_file(argv[2]);
    const std::string cache_manager = read_file(argv[3]);
    const std::string lens = read_file(argv[4]);
    const std::string vignette = read_file(argv[5]);
    const std::string noise = read_file(argv[6]);
    const std::string roughen = read_file(argv[7]);

    require(registry, "kEmbeddedLensFlareEffect");
    require(registry, "kEmbeddedVignetteEffect");
    require(registry, "kEmbeddedNoiseEffect");
    require(registry, "kEmbeddedRoughenEdgesEffect");
    require(registry, "embedded_effect_source(type)");
    require(registry, "gs_effect_create(");
    require(registry, "trying installed asset");
    require(registry, "gs_effect_create_from_file(path, &errors)");
    require_before(registry, "embedded_effect_source(type)",
                   "obs_module_file(def->relative_path)");
    require(registry, "Compiled embedded procedural effect");
    require(registry, "Effect asset path could not be resolved for");

    require(title_source, "gpu-effects-v8-lens-flare-dx11-keyword-fix");
    require(cache_manager, "gpu-renderer-v31-lens-flare-dx11-keyword-fix");

    shader_contract(lens, "pixel_shader = PSLensFlare(v_in);");
    require(lens, "float flare_disc");
    require(lens, "float flare_halo");
    require(lens, "uniform float angle;");
    require(lens, "uniform float falloff;");
    require(lens, "uniform float ghostCount;");
    require(lens, "float output_alpha = saturate(base.a + flare_alpha * (1.0 - base.a));");
    require(lens, "float3 output_rgb = saturate(base.rgb + flare_color * flare_alpha);");
    require(title_source, "set_effect_float_param(effect, \"ghostCount\"");
    shader_contract(vignette, "pixel_shader = PSVignette(v_in);");
    require(vignette, "uniform float roundness;");
    shader_contract(noise, "pixel_shader = PSNoise(v_in);");
    require(noise, "uniform int animatedNoise;");
    require(noise, "float layered_noise");
    shader_contract(roughen, "pixel_shader = PSRoughen(v_in);");
    require(roughen, "uniform float2 texelSize;");
    require(roughen, "float rough_layers");

    std::cout << "embedded procedural effect runtime contract: PASS\n";
    return 0;
}
