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
    double      pause_time    = 0.0; /* seconds from timeline start */
    uint32_t    bg_color    = 0x00000000;  /* transparent by default */
    int         width       = 1920;
    int         height      = 1080;

    std::vector<std::shared_ptr<Layer>> layers;  /* bottom → top order */
    std::vector<std::vector<std::string>> live_text_rows;
    std::vector<std::string> live_text_column_order; /* exposed text layer IDs by logical cue column */
    std::string live_text_header_state; /* base64-encoded dock header layout */
    std::string preview_screenshot_png_base64; /* manually captured title-list thumbnail */
    bool external_data_enabled = false; /* live text cue external data source toggle */
    int current_cue_row = -1; /* runtime-only active live text row */
    int pending_cue_row = -1; /* runtime-only next row waiting for outro */
    uint64_t cue_revision = 0; /* runtime-only live text cue counter */
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

    /* Change notifications */
    using ChangeCallback = std::function<void()>;
    uint64_t on_change(ChangeCallback cb);
    void remove_change_callback(uint64_t callback_id);
    void notify_change();
    void touch_runtime_change();
    uint64_t revision() const { return revision_.load(); }

private:
    TitleDataStore() = default;
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

    static std::string data_path();
};
