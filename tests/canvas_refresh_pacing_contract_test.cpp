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
void reject(const std::string &source, const char *needle)
{
    if (source.find(needle) != std::string::npos) {
        std::cerr << "unexpected: " << needle << '\n';
        assert(false);
    }
}
} // namespace

int main(int argc, char **argv)
{
    assert(argc == 7);
    const std::string canvas_h = read_file(argv[1]);
    const std::string canvas_view = read_file(argv[2]);
    const std::string canvas_events = read_file(argv[3]);
    const std::string editor_session = read_file(argv[4]);
    const std::string editor_events = read_file(argv[5]);
    const std::string signal_handlers = read_file(argv[6]);

    require(canvas_h, "void set_playhead(double t, bool playback_frame = false);");
    require(canvas_h, "void set_playback_active(bool active);");
    require(canvas_h, "bool present_gpu_display_if_due();");
    require(canvas_view, "gs_swapchain_t *swapchain = nullptr;");
    require(canvas_view, "gpu_display_->swapchain = gs_swapchain_create(&info);");
    require(canvas_view, "gs_load_swapchain(gpu_display_->swapchain);");
    require(canvas_view, "gs_present();");
    reject(canvas_view, "obs_display_create(");
    reject(canvas_view, "obs_display_add_draw_callback(");
    require(canvas_view, "playback_frame_pending_ = playback_frame;");
    require(canvas_view, "playback_present_pending_ = playback_frame;");
    require(canvas_view, "editing_present_pending_ = !playback_frame;");
    require(canvas_view, "std::ceil(1000.0 / std::clamp(hz, 1.0, 1000.0))");
    require(canvas_events, "const int editing_cadence_ms = std::max(display_refresh_interval_ms_,");
    require(canvas_events, "const int cadence_ms = playback_frame_pending_ ? 0 : editing_cadence_ms;");
    require(canvas_events, "render_interval_ms_ = std::clamp(cost_ms + 1, 1, 34);");
    require(canvas_events, "present_gpu_display_if_due();");

    require(editor_session, "return std::max(1.0 / 1000.0, obs_frame_duration());");
    require(editor_session, "if (playing_)\n            return;");
    reject(editor_session, "kMaxPlaybackUiHz");
    require(editor_events, "apply_playhead_change(t, false);");
    require(editor_events, "canvas_->set_playhead(t, playback_frame);");
    reject(editor_events, "canvas_->set_playhead(t, playing_);");
    require(signal_handlers, "std::ceil(1000.0 / hz)");
    require(signal_handlers, "apply_playhead_change(next_playhead, true);");
    require(canvas_view, "if (transport_playback_active_ && !playback_present_pending_ &&");
    require(canvas_view, "!editing_present_pending_");
    require(canvas_view, "playback_present_pending_ && !editing_present_pending_");
    require(canvas_view, "const int cadence_ms = project_rate_present ? 0 : display_refresh_interval_ms_;");
    require(canvas_view, "present_coalesce_timer_->start(");
    require(canvas_view, "void CanvasPreview::set_playback_active(bool active)");

    std::cout << "canvas refresh pacing contract: PASS\n";
    return 0;
}
