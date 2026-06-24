/*
 * title-data.h
 *
 * Core data model for the OBS Graphics Studio Pro plugin.
 *
 * A Title is composed of one or more Layers. Each layer has a set of
 * Properties (position, scale, opacity, colour, text …). Properties
 * can be animated over time via Keyframes that live on a Timeline.
 *
 * The TitleDataStore is a singleton that owns all titles for the active
 * scene collection and persists them to a scene-collection-specific JSON
 * file in the OBS profile directory.
 */

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <cstdint>
#include <atomic>
#include <condition_variable>
#include <thread>
#include <mutex>
#include "layer-model.h"

/* ══════════════════════════════════════════════════════════════════
 *  Title
 * ══════════════════════════════════════════════════════════════════ */
struct Title {
    std::string id;
    std::string name        = "Untitled";
    std::string description;
    std::string creator;
    std::string creation_date;
    double      duration    = 5.0;   /* total clip duration (seconds) */
    double      loop_start  = 1.0;   /* live-cue loop start (seconds) */
    double      loop_end    = 4.0;   /* live-cue loop end (seconds) */
    int         playback_mode = 0;   /* 0=play once, 1=loop in/out, 2=pause at position */
    int         loop_type     = 0;   /* 0=restart, 1=ping-pong */
    int         cue_end_behavior = 0; /* 0=show last frame, 1=show nothing, 2=show first frame */
    double      pause_time    = 0.0; /* seconds from timeline start */
    uint32_t    bg_color    = 0x00000000;  /* transparent by default */
    int         width       = 1920;
    int         height      = 1080;

    std::vector<std::shared_ptr<Layer>> layers;  /* bottom → top order */

    /* Editor defaults persisted with the title/template. These are used only
     * as the initial style for newly-created layers; they must not affect
     * rendering and must not carry an effect stack. */
    bool        editor_default_style_enabled = false;
    Layer       editor_default_layer_style;
    uint32_t    editor_default_foreground_color = 0xFF222222;
    uint32_t    editor_default_background_color = 0xFFFFFFFF;
    /* Photoshop-style recent colors for the editor color tab: stored newest-first,
     * de-duplicated, and persisted with the template rather than pushed on every drag. */
    std::vector<std::string> editor_recent_color_hexes;
    std::vector<std::vector<std::string>> live_text_rows;
    std::vector<std::string> live_text_row_ids; /* persistent stable IDs parallel to live_text_rows */
    std::vector<std::string> live_text_column_order; /* exposed text layer IDs by logical cue column */
    std::string live_text_header_state; /* base64-encoded dock header layout */
    std::string preview_screenshot_png_base64; /* manually captured title-list thumbnail */
    bool external_data_enabled = false; /* live text cue external data source toggle */
    bool playlist_loop = false; /* per-title live text playlist behavior */
    bool playlist_reverse = false;
    bool playlist_restart_on_source_active = false;
    bool playlist_stop_on_source_inactive = false;
    double playlist_hold_seconds = 5.0;
    int current_cue_row = -1; /* runtime-only active live text row */
    int pending_cue_row = -1; /* runtime-only next row waiting for outro */
    bool cue_uncue_requested = false; /* runtime-only: keep active status until the outro completes */
    uint64_t cue_revision = 0; /* runtime-only live text cue counter */
    bool playlist_active = false; /* runtime-only playlist state */
    int playlist_next_row = 0;
    int64_t playlist_next_due_ms = 0;
    bool playlist_stop_after_due = false;
    bool cue_background_persistence = false; /* runtime-only setting: enable background persistence for cue transitions */
    bool cue_text_persistence = false; /* runtime-only setting: freeze unchanged exposed text columns while cueing */
    bool cue_persistence_transition = false; /* runtime-only active persistent transition between cue rows */
    std::vector<bool> cue_persistent_text_columns; /* runtime-only exposed text columns held at pause/loop */

    /* Helpers */
    std::shared_ptr<Layer> find_layer(const std::string &layer_id) const;
    void add_layer(std::shared_ptr<Layer> l);
    void remove_layer(const std::string &layer_id);
    void move_layer(const std::string &layer_id, int delta);
};

void ensure_live_text_row_ids(Title &title);
std::string live_text_row_id(const Title &title, int row);
/* Stable fingerprint of raster-affecting layer data. Transform, visibility,
 * parenting, masks and compositing state are intentionally excluded so the
 * GPU compositor can reuse a layer texture across matrix-only edits. */
std::string layer_render_fingerprint(const Layer &layer);

struct TitleTemplateExportMetadata {
    std::string title;
    std::string description;
    std::string creator;
    std::string creation_date;
    std::string screenshot_png_base64;
};

/* ══════════════════════════════════════════════════════════════════
 *  TitleDataStore  (singleton)
 * ══════════════════════════════════════════════════════════════════ */
class TitleDataStore {
public:
    static TitleDataStore &instance();
    static std::string make_uuid();

    /* CRUD */
    std::shared_ptr<Title> create_title(const std::string &name = "New Title");
    std::shared_ptr<Title> get_title(const std::string &id) const;
    void                   delete_title(const std::string &id);
    void                   rename_title(const std::string &id,
                                        const std::string &name);
    bool                   export_title(const std::string &id,
                                        const std::string &path,
                                        std::string *error = nullptr) const;
    bool                   export_title(const std::string &id,
                                        const std::string &path,
                                        const TitleTemplateExportMetadata &metadata,
                                        std::string *error = nullptr) const;
    std::shared_ptr<Title> import_title(const std::string &path,
                                        std::string *error = nullptr);

    std::vector<std::shared_ptr<Title>> titles() const;

    /* Persistence */
    void load();
    void save() const;
    void save_async() const;

    /* Change notifications */
    using ChangeCallback = std::function<void()>;
    uint64_t on_change(ChangeCallback cb);
    void remove_change_callback(uint64_t callback_id);
    void notify_change();
    void touch_runtime_change();
    uint64_t revision() const { return revision_.load(); }

private:
    TitleDataStore();
    ~TitleDataStore();
    TitleDataStore(const TitleDataStore &) = delete;
    TitleDataStore &operator=(const TitleDataStore &) = delete;

    struct PendingSave {
        std::vector<std::shared_ptr<Title>> snapshot;
        std::string path;
        uint64_t generation = 0;
    };
    void save_worker_loop() const;
    static bool write_snapshot_atomic(const std::vector<std::shared_ptr<Title>> &snapshot,
                                      const std::string &path);
    mutable std::recursive_mutex         mutex_;
    std::vector<std::shared_ptr<Title>>  titles_;
    std::string                          loaded_path_;
    struct ChangeObserver {
        uint64_t id = 0;
        ChangeCallback callback;
    };

    std::vector<ChangeObserver>          change_cbs_;
    uint64_t                             next_change_cb_id_ = 1;
    std::atomic<uint64_t>                revision_ { 0 };

    mutable std::mutex                   save_mutex_;
    mutable std::mutex                   save_io_mutex_;
    mutable std::condition_variable      save_cv_;
    mutable std::thread                  save_thread_;
    mutable bool                         save_stop_ = false;
    mutable bool                         save_worker_started_ = false;
    mutable uint64_t                     save_generation_ = 0;
    mutable std::unique_ptr<PendingSave> pending_save_;

    static std::string data_path();
};
