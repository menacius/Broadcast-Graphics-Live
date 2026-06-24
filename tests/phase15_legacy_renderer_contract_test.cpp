#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {
std::string read_file(const char *path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input.good()) {
        std::cerr << "cannot read: " << path << '\n';
        assert(false);
    }
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}
void require(const std::string &source, const char *needle)
{
    if (source.find(needle) == std::string::npos) {
        std::cerr << "missing: " << needle << '\n';
        assert(false);
    }
}
void reject(const std::string &source, const char *needle)
{
    if (source.find(needle) != std::string::npos) {
        std::cerr << "forbidden: " << needle << '\n';
        assert(false);
    }
}
}

int main(int argc, char **argv)
{
    assert(argc == 13);
    const std::string title_source = read_file(argv[1]);
    const std::string title_header = read_file(argv[2]);
    const std::string text_source = read_file(argv[3]);
    const std::string text_header = read_file(argv[4]);
    const std::string vector_source = read_file(argv[5]);
    const std::string vector_header = read_file(argv[6]);
    const std::string cache_source = read_file(argv[7]);
    const std::string cache_header = read_file(argv[8]);
    const std::string canvas_source = read_file(argv[9]);
    const std::string cmake = read_file(argv[10]);
    const std::string vcpkg = read_file(argv[11]);
    const std::string memory_source = read_file(argv[12]);

    // Mandatory GPU artwork coverage.
    require(title_source, "layer_can_use_gpu_text_raster");
    require(title_source, "layer_can_use_gpu_vector_raster");
    require(title_source, "prepare_gpu_image_raster");
    require(title_source, "render_gpu_mask_graph_texture");
    require(title_source, "apply_gpu_layer_effect_stack");
    require(title_source, "std::unique_ptr<gsp::gpu_text::Renderer>");
    require(title_source, "std::unique_ptr<gsp::gpu_vector::Renderer>");
    require(cache_source, "gpu-renderer-v23-phase15-gpu-only-artwork");
    require(text_source, "gs_texrender_begin");
    require(text_header, "ClusterVisual");
    require(vector_source, "QPainterPathStroker");
    require(vector_source, "gs_draw(GS_TRIS");
    require(vector_header, "class Renderer");

    // The cache and presentation APIs are GPU-token based.
    require(cache_source, "requestFrameGpuToken");
    require(cache_source, "title_gpu_frame_cache_store_image");
    require(cache_header, "requestLiveCueFrameGpuToken");
    require(canvas_source, "title_gpu_render_session_submit_gpu_cached_frame");
    require(memory_source, "total_physical_bytes() / (2ull * kMiB)");

    // Legacy artwork renderer and compatibility branches are gone.
    reject(title_source, "#include <QPainter>");
    reject(title_source, "#include <QTextDocument>");
    reject(title_source, "render_layer_unmasked");
    reject(title_source, "cairo_");
    reject(title_source, "pango_");
    reject(title_source, "box_blur_pixels");
    reject(title_source, "render_title_to_image");
    reject(title_source, "render_title_over_cached_frame");
    reject(title_source, "render_gpu_layer_base_raster");
    reject(title_source, "surface_effect_compatibility");
    reject(title_header, "render_title_to_image");
    reject(cache_source, "RamFrameCache");
    reject(cache_source, "ram_cache_");
    reject(cache_header, "RamFrameCache");
    reject(cache_header, "requestFrameRealtime");
    reject(cache_header, "requestLiveCueFrameRealtime");
    reject(canvas_source, "frame_image_");

    // QTextDocument may remain in editor widgets, but never in GPU artwork drawing.
    reject(text_source, "QTextDocument");
    reject(text_source, "#include <QPainter>");
    reject(text_source, "drawContents");
    reject(vector_source, "QPainter ");
    reject(vector_source, "drawPath");

    // Build/package dependencies cannot re-enable the old renderer.
    reject(cmake, "CAIRO");
    reject(cmake, "PANGO");
    reject(cmake, "cairo");
    reject(cmake, "pango");
    reject(vcpkg, "cairo");
    reject(vcpkg, "pango");

    std::cout << "Phase 15 GPU-only artwork renderer contract passed\n";
    return 0;
}
