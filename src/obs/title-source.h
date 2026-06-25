/*
 * title-source.h
 *
 * OBS source type "broadcast_graphics_live_source".
 * Renders a Title through the unified GPU compositor used by OBS live output,
 * the editor preview and final cache readback.
 */

#pragma once

#include <obs-module.h>
#include <string>
#include <cstdint>
#include <cstddef>
#include <QImage>
#include <QRect>

struct Title;

/* Registers the source type with OBS. Call once from obs_module_load(). */
void title_source_register();
void release_title_gpu_render_resources();
/* Invalidates every live source presentation at a frontend lifecycle boundary
 * (most importantly scene-collection cleanup/change).  The next video tick
 * rebuilds each source from its current title; video_render refuses to sample
 * a texture from the previous generation in the meantime. */
void title_source_invalidate_all_presentations();
void title_source_begin_scene_collection_transition();
void title_source_end_scene_collection_transition();

struct TitleGpuRenderSession;

/* Phase 14 asynchronous final-frame readback ticket. The GPU render and the
 * staging copy are submitted together; CPU mapping is deliberately deferred so
 * later GPU frames can execute while an older staging surface completes. */
struct TitleGpuReadbackTicket {
    TitleGpuRenderSession *session = nullptr;
    uint64_t serial = 0;
    QRect region;
    uint32_t canvas_width = 0;
    uint32_t canvas_height = 0;

    bool valid() const { return session != nullptr && serial != 0; }
};

TitleGpuRenderSession *title_gpu_render_session_create();
void title_gpu_render_session_destroy(TitleGpuRenderSession *session);
void title_gpu_render_session_invalidate_presentation(
    TitleGpuRenderSession *session, bool discard_model = true);
void title_gpu_render_session_update(TitleGpuRenderSession *session, const Title &title,
                                     double time, uint64_t model_revision,
                                     bool transform_only_update = false);
void title_gpu_render_session_set_preview_quality(TitleGpuRenderSession *session,
                                                   double scale, bool editor_draft);
void title_gpu_render_session_update_range(TitleGpuRenderSession *session,
                                           const Title &title, double time,
                                           uint64_t model_revision,
                                           std::size_t first_layer,
                                           std::size_t last_layer,
                                           bool transform_only_update = false);
bool title_gpu_render_session_submit_final_frame(TitleGpuRenderSession *session,
                                                 const Title &title,
                                                 const QImage &image,
                                                 uint64_t model_revision);
bool title_gpu_render_session_submit_cached_prefix(
    TitleGpuRenderSession *session, const Title &title, const QImage &cached_prefix,
    double time, std::size_t first_dynamic_layer, uint64_t model_revision);
bool title_gpu_render_session_draw(TitleGpuRenderSession *session,
                                   uint32_t output_width, uint32_t output_height);
std::string title_gpu_render_session_last_error(TitleGpuRenderSession *session);
QImage title_gpu_render_session_readback(TitleGpuRenderSession *session);

/* Submit/resolve a final-frame-only triple-buffered readback. The readback is
 * mapped once, then the completed sparse payload is published into the tiled
 * GPU RAM cache. region may be a dirty-tile union; empty means full canvas. */
bool render_title_gpu_cache_submit_readback(
    const Title &title, double time, uint64_t model_revision,
    const std::string &cache_key, const QRect &region,
    TitleGpuReadbackTicket &ticket);
bool title_gpu_render_session_resolve_readback(
    const TitleGpuReadbackTicket &ticket, QImage &image);
void title_gpu_render_session_discard_readback(
    const TitleGpuReadbackTicket &ticket);

/* Process-wide sparse GPU-resident RAM cache. Frames reference shared 128x128
 * textures; transparent tiles are omitted and identical tiles are deduplicated.
 * The token is CacheFrameKey::toString(). Source/editor playback submits it
 * directly without reconstructing a full CPU frame. */
bool title_gpu_frame_cache_contains(const std::string &cache_key);
bool title_gpu_frame_cache_store_image(
    const std::string &cache_key, const QImage &sparse_image,
    uint32_t canvas_width, uint32_t canvas_height);
void title_gpu_frame_cache_remove(const std::string &cache_key);
void title_gpu_frame_cache_remove_title(const std::string &title_id);
void title_gpu_frame_cache_clear();
void title_gpu_frame_cache_set_budget(uint64_t bytes);
uint64_t title_gpu_frame_cache_bytes_used();
bool title_gpu_render_session_submit_gpu_cached_frame(
    TitleGpuRenderSession *session, const Title &title,
    const std::string &cache_key, uint64_t model_revision);
bool title_gpu_render_session_submit_gpu_cached_prefix(
    TitleGpuRenderSession *session, const Title &title,
    const std::string &cache_key, double time,
    std::size_t first_dynamic_layer, uint64_t model_revision);

QImage render_title_gpu_frame_readback(const Title &title, double time,
                                           uint64_t model_revision = 0);
QImage render_title_to_image(const Title &title, double t,
                             uint64_t model_revision = 0);
QImage render_title_to_image_scaled(const Title &title, double t, double scale,
                                    bool editor_draft = false);
QImage render_title_region_to_image(const Title &title, double t, const QRect &region,
                                    uint64_t model_revision = 0);
QImage render_title_cache_to_image(const Title &title, double t,
                                   uint64_t model_revision = 0);
QImage render_title_cache_region_to_image(const Title &title, double t,
                                          const QRect &region,
                                          uint64_t model_revision = 0);
QImage render_title_over_cached_frame(const Title &title, double t,
                                      const QImage &cached_prefix,
                                      uint64_t model_revision = 0);

/* Source settings keys */
#define PROP_TITLE_ID      "title_id"
#define PROP_AUTO_ADVANCE  "auto_advance"
#define PROP_SCENE_MASKS_GROUP "scene_masks"
#define PROP_SCENE_MASK_PREFIX "scene_mask_"
