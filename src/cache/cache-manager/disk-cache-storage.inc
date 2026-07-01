    while (!manifest.atEnd()) {
        const QJsonDocument doc = QJsonDocument::fromJson(manifest.readLine().trimmed());
        if (!doc.isObject())
            continue;
        const QJsonObject obj = doc.object();
        CacheFrameKey key;
        key.title_id = obj.value(QStringLiteral("title_id")).toString();
        key.content_hash = obj.value(QStringLiteral("content_hash")).toString();
        key.frame = obj.value(QStringLiteral("frame")).toInt();
        key.width = obj.value(QStringLiteral("width")).toInt();
        key.height = obj.value(QStringLiteral("height")).toInt();
        if (key.title_id.isEmpty() || key.content_hash.isEmpty() ||
            key.frame < 0 || key.width <= 0 || key.height <= 0)
            continue;
        const QString indexed_key = key.toString();
        if (indexed_keys_.contains(indexed_key))
            continue;
        QVector<TileRef> refs;
        if (!readFrameTileRefsLocked(key, refs)) {
            QFile::remove(pathForKey(key));
            continue;
        }
        QVector<QByteArray> digests;
        bool valid = true;
        digests.reserve(refs.size());
        for (const TileRef &ref : refs) {
            if (!QFileInfo::exists(pathForTileDigest(ref.digest))) {
                valid = false;
                break;
            }
            digests.push_back(ref.digest);
        }
        if (!valid) {
            QFile::remove(pathForKey(key));
            continue;
        }
        indexed_keys_[indexed_key] = key;
        frame_tile_digests_[indexed_key] = digests;
        addTileReferencesLocked(digests);
        indexed_frame_files.insert(QFileInfo(pathForKey(key)).fileName());
    }
    manifest.close();

    QDir dir(cache_dir_);
    for (const QFileInfo &frame : dir.entryInfoList(
             QStringList() << QStringLiteral("*.ogsf"), QDir::Files)) {
        if (!indexed_frame_files.contains(frame.fileName()))
            QFile::remove(frame.absoluteFilePath());
    }
    for (const QFileInfo &tile : dir.entryInfoList(
             QStringList() << QStringLiteral("*.ogst"), QDir::Files)) {
        const QByteArray digest = QByteArray::fromHex(
            tile.completeBaseName().toLatin1());
        if (!tile_ref_counts_.contains(digest))
            QFile::remove(tile.absoluteFilePath());
    }
    rewriteManifestLocked();
}

quint64 DiskFrameCache::scanBytesUsed() const
{
    quint64 total = 0;
    QDir dir(cache_dir_);
    if (!dir.exists())
        return total;
    for (const QFileInfo &file : dir.entryInfoList(
             QStringList() << QStringLiteral("*.ogsf")
                           << QStringLiteral("*.ogst"), QDir::Files))
        total += (quint64)file.size();
    return total;
}

QString DiskFrameCache::manifestPath() const
{
    return QDir(cache_dir_).filePath(QStringLiteral("manifest.jsonl"));
}

void DiskFrameCache::appendManifestEntry(const CacheFrameKey &key) const
{
    QDir().mkpath(cache_dir_);
    QFile manifest(manifestPath());
    if (!manifest.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
        return;
    QJsonObject obj;
    obj.insert(QStringLiteral("title_id"), key.title_id);
    obj.insert(QStringLiteral("content_hash"), key.content_hash);
    obj.insert(QStringLiteral("frame"), key.frame);
    obj.insert(QStringLiteral("width"), key.width);
    obj.insert(QStringLiteral("height"), key.height);
    manifest.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
    manifest.write("\n");
}

void DiskFrameCache::rewriteManifestLocked() const
{
    QDir().mkpath(cache_dir_);
    QSaveFile manifest(manifestPath());
    if (!manifest.open(QIODevice::WriteOnly | QIODevice::Text))
        return;
    for (auto it = indexed_keys_.cbegin(); it != indexed_keys_.cend(); ++it) {
        const CacheFrameKey &key = it.value();
        QJsonObject obj;
        obj.insert(QStringLiteral("title_id"), key.title_id);
        obj.insert(QStringLiteral("content_hash"), key.content_hash);
        obj.insert(QStringLiteral("frame"), key.frame);
        obj.insert(QStringLiteral("width"), key.width);
        obj.insert(QStringLiteral("height"), key.height);
        if (manifest.write(QJsonDocument(obj).toJson(QJsonDocument::Compact)) < 0 ||
            manifest.write("\n") != 1) {
            manifest.cancelWriting();
            return;
        }
    }
    manifest.commit();
}



CacheInvalidationManager::CacheInvalidationManager(QObject *parent) : QObject(parent) {}

int CacheInvalidationManager::frameForTime(double t) const
{
    return cache_frame_index_for_time(t, cache_obs_frame_rate());
}

void CacheInvalidationManager::invalidateAll(const std::shared_ptr<Title> &title)
{
    if (!title) return;
    emit rangeInvalidated(QString::fromStdString(title->id), 0,
                          cache_last_frame_for_title(*title, cache_obs_frame_rate()));
}

void CacheInvalidationManager::invalidateRange(const std::shared_ptr<Title> &title, double start, double end)
{
    if (!title) return;
    if (end < start) std::swap(start, end);
    const int max_frame = cache_last_frame_for_title(*title, cache_obs_frame_rate());
    emit rangeInvalidated(QString::fromStdString(title->id),
                          std::clamp(frameForTime(std::clamp(start, 0.0, title->duration)), 0, max_frame),
                          std::clamp(frameForTime(std::clamp(end, 0.0, title->duration)), 0, max_frame));
}

void CacheInvalidationManager::invalidateLayer(const std::shared_ptr<Title> &title, const std::string &layer_id)
{
    if (!title) return;
    auto layer = title->find_layer(layer_id);
    if (!layer) {
        invalidateAll(title);
        return;
    }
    invalidateRange(title, layer->in_time, layer->out_time);
}

struct CacheManager::WorkerPipeline {
    struct PendingReadback {
        RenderQueueManager::Job job;
        TitleGpuReadbackTicket ticket;
        QImage previous;
        bool was_stale = false;
        bool had_previous = false;
        QString live_state_key;
        QString temporal_state_key;
    };

    std::deque<PendingReadback> pending_readbacks;
};

CacheManager &CacheManager::instance()
{
    static CacheManager manager;
    return manager;
}

CacheManager::CacheManager(QObject *parent)
    : QObject(parent),
      ram_cache_(this),
      disk_cache_(this),
      state_tracker_(this),
      queue_(this),
      invalidation_(this),
      worker_pipeline_(std::make_unique<WorkerPipeline>())
{
    cache_enabled_.store(TitlePreferences::cache_enabled());
    paused_.store(!cache_enabled_.load());
    queue_.setAcceptingJobs(cache_enabled_.load());
    connect(&queue_, &RenderQueueManager::queueChanged, this, [this]() {
        emit queueChanged();
        wakeWorker();
    });
    connect(&state_tracker_, &CacheStateTracker::stateChanged, this, &CacheManager::cacheStatesChanged);
    connect(&invalidation_, &CacheInvalidationManager::rangeInvalidated, this,
            [this](const QString &title_id, int first, int last) {
                /* An in-flight job is no longer valid after an edit. A per-title
                 * generation closes the dequeue/cancel race without touching UI
                 * objects or the renderer from the caller thread. */
                {
                    std::lock_guard<std::recursive_mutex> publication_lock(publication_mutex_);
                    bumpTitleGeneration(title_id);
                    queue_.cancelTitle(title_id);
                }
                /* cancelTitle() removes all queued work for the generation, not
                 * just the edited frame range. Relinquish every transient owner
                 * before marking the actually edited range stale, otherwise an
                 * unaffected cancelled frame (or cue row) can remain Queued
                 * forever with no worker responsible for it. */
                resetCancelledWorkState(title_id);
                state_tracker_.markRange(title_id, first, last, FrameCacheState::Stale);
                wakeWorker();
            });
    const quint64 initial_ram_budget =
        (quint64)TitlePreferences::cache_ram_limit_mb() * 1024ull * 1024ull;
    const quint64 compatibility_ram_budget = std::clamp<quint64>(
        initial_ram_budget / 8ull,
        16ull * 1024ull * 1024ull,
        64ull * 1024ull * 1024ull);
    ram_cache_.setMaxBytes(compatibility_ram_budget);
    title_gpu_frame_cache_set_budget(
        initial_ram_budget > compatibility_ram_budget
            ? initial_ram_budget - compatibility_ram_budget
            : initial_ram_budget);
    worker_thread_ = std::thread(&CacheManager::workerLoop, this);
    wakeWorker();
    BGL_LOG_INFO("Cache", QStringLiteral("CacheManager initialized enabled=%1 ramLimitMb=%2 disk=%3 worker=background")
                              .arg(cache_enabled_.load())
                              .arg(TitlePreferences::cache_ram_limit_mb())
                              .arg(disk_cache_.cacheDirectory()));
}

CacheManager::~CacheManager()
{
    shutdownWorker();
}

void CacheManager::shutdownWorker()
{
    if (worker_stop_.exchange(true)) {
        if (worker_thread_.joinable())
            worker_thread_.join();
        return;
    }
    queue_.setAcceptingJobs(false);
    worker_cv_.notify_all();
    if (worker_thread_.joinable())
        worker_thread_.join();
}

void CacheManager::wakeWorker()
{
    worker_cv_.notify_one();
}

quint64 CacheManager::titleGeneration(const QString &title_id) const
{
    std::lock_guard<std::mutex> lock(generation_mutex_);
    return title_generations_.value(title_id, 0);
}

void CacheManager::bumpTitleGeneration(const QString &title_id)
{
    if (title_id.isEmpty())
        return;
    std::lock_guard<std::mutex> lock(generation_mutex_);
    title_generations_[title_id] = title_generations_.value(title_id, 0) + 1;
}

bool CacheManager::jobIsCurrent(const RenderQueueManager::Job &job) const
{
    return cache_enabled_.load() &&
           job.cache_epoch == cache_epoch_.load() &&
           job.title_generation == titleGeneration(job.key.title_id);
}

std::shared_ptr<Title> CacheManager::snapshotForJob(const std::shared_ptr<Title> &title,
                                                    const CacheFrameKey &key)
{
    if (!title)
        return nullptr;
    const QString snapshot_key = QStringLiteral("%1|%2|%3x%4")
        .arg(key.title_id, key.content_hash)
        .arg(key.width)
        .arg(key.height);
    {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        auto it = job_snapshots_.find(snapshot_key);
        if (it != job_snapshots_.end()) {
            if (auto existing = it.value().lock())
                return existing;
            job_snapshots_.erase(it);
        }
    }

    auto snapshot = clone_title_snapshot(title);
    if (!snapshot)
        return nullptr;
    {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        /* Keep the lookup bounded across long editing sessions. Entries are
         * weak: queued/in-flight jobs own snapshots, not this index. */
        if (job_snapshots_.size() > 128) {
            for (auto it = job_snapshots_.begin(); it != job_snapshots_.end();) {
                if (it.value().expired())
                    it = job_snapshots_.erase(it);
                else
                    ++it;
            }
        }
        job_snapshots_[snapshot_key] = snapshot;
    }
    return snapshot;
}

double CacheManager::effectiveFrameRate() const
{
    return cache_obs_frame_rate();
}

QString CacheManager::contentHashForTitle(const Title &title) const
{
    return contentHash(title);
}

bool CacheManager::visualStateCurrent(const Title &title) const
{
    return visualHashUnchanged(title);
}

int CacheManager::frameForTime(double t) const
{
    return cache_frame_index_for_time(t, effectiveFrameRate());
}

double CacheManager::timeForFrame(int frame) const
{
    return cache_time_for_frame(frame, effectiveFrameRate());
}

QString CacheManager::contentHash(const Title &title) const
{
    QCryptographicHash hash(QCryptographicHash::Sha1);
    hash.addData(kGpuRendererCacheAbi);
    hash.addData("\0", 1);
    auto add = [&](const auto &value) {
        QByteArray bytes;
        QDataStream stream(&bytes, QIODevice::WriteOnly);
        stream.setVersion(QDataStream::Qt_5_15);
        stream.setByteOrder(QDataStream::LittleEndian);
        stream << value;
        hash.addData(bytes);
    };
    auto add_fill = [&](const RichTextFill &f) {
        add(f.type); add((quint32)f.color); add(f.gradient_type); add(f.gradient_spread);
        add((quint32)f.gradient_start_color); add((quint32)f.gradient_end_color);
        add(f.gradient_start_pos); add(f.gradient_end_pos);
        add(f.gradient_start_opacity); add(f.gradient_end_opacity); add(f.gradient_opacity);
        add(f.gradient_angle); add(f.gradient_center_x); add(f.gradient_center_y);
        add(f.gradient_scale); add(f.gradient_focal_x); add(f.gradient_focal_y);
    };
    auto add_stroke = [&](const RichTextStroke &stroke) {
        add(stroke.enabled); add(stroke.width); add(stroke.opacity);
        add(stroke.on_front); add(stroke.alignment); add(stroke.antialias);
        add(stroke.join_style); add_fill(stroke.fill);
    };
    auto add_char_format = [&](const RichTextCharFormat &f) {
        add(QString::fromStdString(f.font_family)); add(QString::fromStdString(f.font_style));
        add(f.font_size); add(f.bold); add(f.italic); add(f.underline); add(f.strikethrough);
        add(f.kerning); add(f.kerning_mode); add(f.manual_kerning); add(f.tracking);
        add(f.scale_x); add(f.scale_y); add(f.baseline_shift); add(f.text_style);
        add(f.ligatures); add(f.stylistic_alternates); add(f.fractions); add(f.opentype_features);
        add(QString::fromStdString(f.language)); add_fill(f.fill); add_stroke(f.stroke);
    };
    auto add_paragraph_format = [&](const RichTextParagraphFormat &f) {
        add(f.align_h); add(f.align_v); add(f.indent_left); add(f.indent_right);
        add(f.indent_first_line); add(f.line_spacing); add(f.space_before);
        add(f.space_after); add(f.hyphenate);
    };
    auto add_char_format_masked = [&](const RichTextCharFormat &f, uint32_t mask) {
        mask &= RichTextCharAll;
        if (mask & RichTextCharFontFamily) add(QString::fromStdString(f.font_family));
        if (mask & RichTextCharFontStyle) add(QString::fromStdString(f.font_style));
        if (mask & RichTextCharFontSize) add(f.font_size);
        if (mask & RichTextCharBold) add(f.bold);
        if (mask & RichTextCharItalic) add(f.italic);
        if (mask & RichTextCharUnderline) add(f.underline);
        if (mask & RichTextCharStrikethrough) add(f.strikethrough);
        if (mask & RichTextCharKerning) {
            add(f.kerning); add(f.kerning_mode); add(f.manual_kerning);
        }
        if (mask & RichTextCharTracking) add(f.tracking);
        if (mask & RichTextCharScaleX) add(f.scale_x);
        if (mask & RichTextCharScaleY) add(f.scale_y);
        if (mask & RichTextCharBaselineShift) add(f.baseline_shift);
        if (mask & RichTextCharFillColor) add_fill(f.fill);
        if (mask & RichTextCharTextStyle) add(f.text_style);
        if (mask & RichTextCharLigatures) add(f.ligatures);
        if (mask & RichTextCharStylisticAlternates) add(f.stylistic_alternates);
        if (mask & RichTextCharFractions) add(f.fractions);
        if (mask & RichTextCharOpenTypeFeatures) add(f.opentype_features);
        if (mask & RichTextCharLanguage) add(QString::fromStdString(f.language));
        if (mask & RichTextCharStroke) add_stroke(f.stroke);
    };
    auto add_paragraph_format_masked = [&](const RichTextParagraphFormat &f,
                                           uint32_t mask) {
        mask &= RichTextParagraphAll;
        if (mask & RichTextParagraphAlignH) add(f.align_h);
        if (mask & RichTextParagraphAlignV) add(f.align_v);
        if (mask & RichTextParagraphIndentLeft) add(f.indent_left);
        if (mask & RichTextParagraphIndentRight) add(f.indent_right);
        if (mask & RichTextParagraphIndentFirstLine) add(f.indent_first_line);
        if (mask & RichTextParagraphLineSpacing) add(f.line_spacing);
        if (mask & RichTextParagraphSpaceBefore) add(f.space_before);
        if (mask & RichTextParagraphSpaceAfter) add(f.space_after);
        if (mask & RichTextParagraphHyphenate) add(f.hyphenate);
    };
    auto add_anim = [&](const AnimatedProperty &p) {
        add(p.static_value); add((quint64)p.keyframes.size());
        for (const auto &k : p.keyframes) {
            add(k.time); add(k.value); add((int)k.easing); add(k.cx1); add(k.cy1); add(k.cx2); add(k.cy2);
            add((int)k.temporal_mode); add(k.incoming_influence); add(k.outgoing_influence);
            add(k.incoming_speed); add(k.outgoing_speed); add(k.temporal_tangents_linked);
            add(k.temporal_velocity_explicit);
        }
    };
    auto add_anim_vec2 = [&](const AnimatedVec2Property &p) {
        add(p.static_value.x); add(p.static_value.y); add((quint64)p.keyframes.size());
        for (const auto &k : p.keyframes) {
            add(k.time); add(k.value.x); add(k.value.y);
            add((int)k.easing); add(k.cx1); add(k.cy1); add(k.cx2); add(k.cy2);
            add((int)k.temporal_mode); add(k.incoming_influence); add(k.outgoing_influence);
            add(k.incoming_speed); add(k.outgoing_speed); add(k.temporal_tangents_linked);
            add(k.temporal_velocity_explicit);
            add(k.incoming_tangent.x); add(k.incoming_tangent.y);
            add(k.outgoing_tangent.x); add(k.outgoing_tangent.y);
            add((int)k.spatial_mode); add(k.spatial_tangents_linked);
            add(k.rove_across_time);
        }
    };
    auto add_gradient_stops = [&](const std::vector<GradientStop> &stops) {
        add((quint64)stops.size());
        for (const auto &stop : stops) { add((quint32)stop.color); add(stop.position); add(stop.opacity); }
    };
    auto add_effect = [&](const LayerEffect &e) {
        add(QString::fromStdString(e.extension_id));
        add(QString::fromStdString(e.extension_parameters_json));
        add(QString::fromStdString(e.extension_keyframes_json));
        add((int)e.type); add(e.enabled); add(e.brightness); add(e.contrast); add(e.saturation);
        add((quint32)e.tint_color); add(e.tint_amount); add((quint32)e.effect_color);
        add(e.effect_opacity); add(e.effect_size); add(e.effect_distance); add(e.effect_angle);
        add(e.effect_spread); add(e.effect_falloff); add(e.effect_blur_type); add(e.effect_samples);
        add(e.effect_centered); add(e.effect_outside_hard_alpha); add(e.effect_outside_hard_alpha_invert); add(e.affect_layers_behind); add(e.affect_layers_behind_invert); add((int)e.blend_mode);
        add(e.effect_profile); add(e.effect_animated); add(e.effect_monochrome);
        add(e.effect_invert); add(e.effect_seed); add(e.effect_amount);
        add(e.effect_scale); add(e.effect_softness); add(e.effect_roundness);
        add(e.effect_speed); add(e.effect_center_x); add(e.effect_center_y);
        add(e.effect_complexity); add(e.effect_evolution);
        add((quint32)e.effect_secondary_color);
        add(e.effect_fill_type); add(e.effect_join_style);
        add(e.effect_on_front); add(e.effect_antialias); add(e.effect_owned_style_loaded);
        add((quint32)e.effect_stroke_color); add(e.effect_stroke_width); add(e.effect_stroke_opacity);
        add(e.effect_padding_left); add(e.effect_padding_right); add(e.effect_padding_top); add(e.effect_padding_bottom);
        add(e.effect_corner_radius_tl); add(e.effect_corner_radius_tr); add(e.effect_corner_radius_br); add(e.effect_corner_radius_bl);
        add(e.effect_corner_type); add(e.effect_gradient_type); add(e.effect_gradient_spread);
        add((quint32)e.effect_gradient_start_color);
        add((quint32)e.effect_gradient_end_color); add(e.effect_gradient_start_pos); add(e.effect_gradient_end_pos);
        add(e.effect_gradient_start_opacity); add(e.effect_gradient_end_opacity); add(e.effect_gradient_opacity);
        add(e.effect_gradient_angle); add(e.effect_gradient_center_x); add(e.effect_gradient_center_y);
        add(e.effect_gradient_scale); add(e.effect_gradient_focal_x); add(e.effect_gradient_focal_y);
        add_anim(e.enabled_prop); add_anim(e.brightness_prop); add_anim(e.contrast_prop);
        add_anim(e.saturation_prop); add_anim(e.opacity_prop); add_anim(e.size_prop);
        add_anim(e.distance_prop); add_anim(e.angle_prop); add_anim(e.spread_prop);
        add_anim(e.falloff_prop);
        add_anim(e.amount_prop); add_anim(e.scale_prop); add_anim(e.softness_prop);
        add_anim(e.roundness_prop); add_anim(e.speed_prop); add_anim(e.center_x_prop);
        add_anim(e.center_y_prop); add_anim(e.complexity_prop); add_anim(e.evolution_prop);
        add_anim(e.stroke_width_prop);
        add_anim(e.stroke_opacity_prop); add_anim(e.padding_left_prop); add_anim(e.padding_right_prop);
        add_anim(e.padding_top_prop); add_anim(e.padding_bottom_prop); add_anim(e.corner_radius_tl_prop);
        add_anim(e.corner_radius_tr_prop); add_anim(e.corner_radius_br_prop); add_anim(e.corner_radius_bl_prop);
        add_anim(e.gradient_start_pos_prop); add_anim(e.gradient_end_pos_prop);
        add_anim(e.gradient_start_opacity_prop); add_anim(e.gradient_end_opacity_prop);
        add_anim(e.gradient_angle_prop); add_anim(e.gradient_center_x_prop);
        add_anim(e.gradient_center_y_prop); add_anim(e.gradient_scale_prop);
        add_anim(e.gradient_focal_x_prop); add_anim(e.gradient_focal_y_prop);
        add_anim(e.gradient_opacity_prop);
        add_anim(e.gradient_start_color_a); add_anim(e.gradient_start_color_r);
        add_anim(e.gradient_start_color_g); add_anim(e.gradient_start_color_b);
        add_anim(e.gradient_end_color_a); add_anim(e.gradient_end_color_r);
        add_anim(e.gradient_end_color_g); add_anim(e.gradient_end_color_b);
        add_anim(e.color_a); add_anim(e.color_r); add_anim(e.color_g); add_anim(e.color_b);
        add_anim(e.stroke_color_a); add_anim(e.stroke_color_r); add_anim(e.stroke_color_g); add_anim(e.stroke_color_b);
        add_anim(e.secondary_color_a); add_anim(e.secondary_color_r);
        add_anim(e.secondary_color_g); add_anim(e.secondary_color_b);
    };
    auto add_rich_text = [&](const RichTextDocument &rt) {
        add(rt.version); add(QString::fromStdString(rt.plain_text));
        add_char_format(rt.default_format); add_paragraph_format(rt.default_paragraph_format);
        /* Caret/typing format affects only future input, not the pixels of the
         * current document. Keeping it in the visual hash created duplicate
         * cache generations when the cursor moved or selection changed. */
        add((quint64)rt.blocks.size());
        for (const auto &block : rt.blocks) {
            add((quint64)block.start); add((quint64)block.length); add(block.mask);
            add_paragraph_format_masked(block.format, block.mask);
        }
        add((quint64)rt.ranges.size());
        for (const auto &r : rt.ranges) {
            add((quint64)r.start); add((quint64)r.length); add(r.mask);
            add_char_format_masked(r.format, r.mask);
        }
        add(rt.auto_style_enabled); add(QString::fromStdString(rt.auto_default_style_preset_id));
        add(rt.auto_default_style_cached_mask);
        add_char_format_masked(rt.auto_default_style_cached_format,
                               rt.auto_default_style_cached_mask);
        add((quint64)rt.auto_style_rules.size());
        for (const auto &rule : rt.auto_style_rules) {
            add(rule.enabled); add(QString::fromStdString(rule.style_preset_id));
            add(QString::fromStdString(rule.rule_id));
            /* display_name is editor metadata; rule_id remains because rule
             * exclusion references can affect the applied visual result. */
            add(QString::fromStdString(rule.conflict_mode)); add(QString::fromStdString(rule.match_mode));
            add(rule.stop_processing); add(rule.require_stop_match);
            add(rule.include_start_marker); add(rule.include_end_marker);
            add((quint64)rule.excludes_rule_ids.size());
            for (const auto &excluded_id : rule.excludes_rule_ids) add(QString::fromStdString(excluded_id));
            add(QString::fromStdString(rule.start_condition)); add(QString::fromStdString(rule.end_condition));
            add((quint64)rule.start_offset); add((quint64)rule.end_offset);
            add(QString::fromStdString(rule.condition_type));
            add((quint64)rule.start); add((quint64)rule.length);
            add(QString::fromStdString(rule.start_custom_chars)); add(QString::fromStdString(rule.end_custom_chars));
            add(rule.cached_mask);
            add_char_format_masked(rule.cached_format, rule.cached_mask);
        }
    };
    const TitleDynamicLayerAnalysis cache_analysis = analyze_title_dynamic_layers(title);
    if (cache_analysis.has_cacheable_prefix) {
        /* Keep partial-prefix payloads distinct from legacy/full-frame cache
         * entries even when the visible title content is otherwise identical. */
        add(QStringLiteral("partial-prefix-cache-v1"));
        add((quint64)cache_analysis.first_dynamic_layer);
    }

    /* Content hash intentionally tracks rendered pixels only. Title id/name are
     * metadata and must not force a prerender rebuild when the visible output
     * did not change. The title id is already part of CacheFrameKey. */
    add(title.width);
    add(title.height);
    add(title.duration);
    add(title.loop_start);
    add(title.loop_end);
    add(title.pause_time);
    add(title.playback_mode);
    add(title.loop_type);
    add(title.cue_end_behavior);
    add(title.cue_background_persistence);
    add(title.cue_text_persistence);
    add(title.cue_persistence_transition);

    /* Cache keys describe rendered pixels, not the editable cue-list model.
     * titleWithCueApplied()/apply_live_text_row() already copy the active cue
     * text into the exposed layers before a frame is hashed. Hashing the full
     * live_text_rows vector (and the numeric row indices) made an unrelated
     * add/remove/reorder operation change every existing cue key, forcing all
     * previously rendered rows and transition variants to render again.
     *
     * The renderer only needs to know whether a current cue exists and whether
     * the cue list is non-empty for Background Persistence. The exact row
     * number and pending row are playback-control state and do not alter pixels.
     */
    add(title.current_cue_row >= 0);
    add(!title.live_text_rows.empty());
    add((quint64)title.cue_persistent_text_columns.size());
    for (bool persistent_col : title.cue_persistent_text_columns) add(persistent_col);
    add((quint32)title.bg_color);
    add((quint64)title.layers.size());
    for (const auto &layer : title.layers) {
        if (!layer) continue;
        add(QString::fromStdString(layer->id));
        add((int)layer->type);
        add(layer->visible);
        add(QString::fromStdString(layer->parent_id));
        add(QString::fromStdString(layer->transform_parent_id));
        add(QString::fromStdString(layer->asset_title_id));
        add(QString::fromStdString(layer->asset_owner_id));
        add(QString::fromStdString(layer->asset_source_layer_id));
        add(layer->asset_animated);
        add(layer->asset_playback_mode);
        add(layer->asset_playback_offset);
        add(layer->asset_duration);
        add(layer->asset_source_playback_mode);
        add(layer->asset_source_loop_type);
        add(layer->asset_source_loop_start);
        add(layer->asset_source_loop_end);
        add(layer->asset_source_pause_time);
        add(layer->asset_pause_duration);
        add(layer->asset_loop_count);
        add(layer->asset_loop);
        add(QString::fromStdString(layer->mask_source_id));
        add((int)layer->mask_mode);
        add((int)layer->matte_visibility_mode);
        add((int)layer->blend_mode);
        add(layer->use_as_scene_mask);
        add(layer->effect_stack_respects_masks);
        add(layer->expose_text);
        add(layer->ignore_persistence);
        add(layer->in_time);
        add(layer->out_time);

        add_anim_vec2(layer->position);
        add_anim_vec2(layer->scale);
        add_anim(layer->rotation);
        add(layer->transform_quad_tl_x); add(layer->transform_quad_tl_y);
        add(layer->transform_quad_tr_x); add(layer->transform_quad_tr_y);
        add(layer->transform_quad_br_x); add(layer->transform_quad_br_y);
        add(layer->transform_quad_bl_x); add(layer->transform_quad_bl_y);
        add_anim(layer->opacity);
        add_anim_vec2(layer->size);
        add_anim_vec2(layer->image_size);
        add(layer->origin_x); add(layer->origin_y); add_anim_vec2(layer->origin_prop);

        add(QString::fromStdString(layer->text_content));
        add_rich_text(layer->rich_text);
        add((quint64)layer->external_bindings.size());
        for (const auto &binding : layer->external_bindings) {
            add(binding.enabled);
            add(QString::fromStdString(binding.property_path));
            add(QString::fromStdString(binding.source_id));
            add(QString::fromStdString(binding.field_path));
            add(QString::fromStdString(binding.formatter));
            add(binding.has_fallback_value);
            if (binding.has_fallback_value)
                add(QString::fromStdString(
                    external_data_value_to_string(binding.fallback_value)));
        }
        if (layer_has_external_binding(*layer, ExternalPropertyPaths::TextContent))
            add(QString::fromStdString(effective_external_string(
                *layer, ExternalPropertyPaths::TextContent, layer->text_content)));
        add(QString::fromStdString(layer->clock_format));
        add(layer->ticker_style); add(layer->ticker_speed); add(layer->ticker_line_hold); add(layer->ticker_direction);
        add(layer->ticker_playback_mode);
        add(layer->ticker_completion);
        add_anim(layer->ticker_completion_prop);
        add(QString::fromStdString(layer->font_family)); add(QString::fromStdString(layer->font_style));
        add(layer->font_size); add_anim(layer->font_size_prop);
        add(layer->font_bold); add(layer->font_italic); add(layer->font_kerning);
        add(layer->kerning_mode); add(layer->manual_kerning); add(layer->text_leading);
        add(layer->char_tracking); add_anim(layer->char_tracking_prop);
        add(layer->char_scale_x); add_anim(layer->char_scale_x_prop);
        add(layer->char_scale_y); add_anim(layer->char_scale_y_prop);
        add(layer->baseline_shift); add_anim(layer->baseline_shift_prop);
        add(layer->text_style); add(layer->text_underline); add(layer->text_strikethrough);
        add(layer->text_ligatures); add(layer->text_stylistic_alternates);
        add(layer->text_fractions); add(layer->text_opentype_features);
        add(QString::fromStdString(layer->text_language));
        add(layer->text_overflow_mode); add(layer->text_fit_min_scale);
        add(layer->text_box_width_to_text); add(layer->text_box_height_to_text);
        add(layer->max_text_box_width); add(layer->max_text_box_height);
        add((quint32)layer->text_color);
        add_anim(layer->text_color_a); add_anim(layer->text_color_r);
        add_anim(layer->text_color_g); add_anim(layer->text_color_b);

        add(layer->align_h); add(layer->align_v);
        add(layer->paragraph_indent_left); add_anim(layer->paragraph_indent_left_prop);
        add(layer->paragraph_indent_right); add_anim(layer->paragraph_indent_right_prop);
        add(layer->paragraph_indent_first_line); add_anim(layer->paragraph_indent_first_line_prop);
        add(layer->paragraph_space_before); add_anim(layer->paragraph_space_before_prop);
        add(layer->paragraph_space_after); add_anim(layer->paragraph_space_after_prop);
        add(layer->paragraph_hyphenate);

        add(layer->fill_type); add((quint32)layer->fill_color);
        add_anim(layer->fill_color_a); add_anim(layer->fill_color_r);
        add_anim(layer->fill_color_g); add_anim(layer->fill_color_b);
        add(layer->gradient_type); add(layer->gradient_spread);
        add((quint32)layer->gradient_start_color); add((quint32)layer->gradient_end_color);
        add(layer->gradient_start_pos); add(layer->gradient_end_pos);
        add(layer->gradient_start_opacity); add(layer->gradient_end_opacity); add(layer->gradient_opacity);
        add(layer->gradient_angle); add(layer->gradient_center_x); add(layer->gradient_center_y);
        add(layer->gradient_scale); add(layer->gradient_focal_x); add(layer->gradient_focal_y);
        add_gradient_stops(layer->gradient_stops);

        add(layer->outline_enabled); add(layer->stroke_fill_type); add((quint32)layer->stroke_color);
        add(layer->stroke_width); add(layer->outline_opacity); add(layer->outline_join_style);
        add(layer->outline_on_front); add(layer->outline_alignment); add(layer->outline_antialias);
        add(layer->stroke_gradient_type); add(layer->stroke_gradient_spread);
        add((quint32)layer->stroke_gradient_start_color); add((quint32)layer->stroke_gradient_end_color);
        add(layer->stroke_gradient_start_pos); add(layer->stroke_gradient_end_pos);
        add(layer->stroke_gradient_start_opacity); add(layer->stroke_gradient_end_opacity);
        add(layer->stroke_gradient_opacity); add(layer->stroke_gradient_angle);
        add(layer->stroke_gradient_center_x); add(layer->stroke_gradient_center_y);
        add(layer->stroke_gradient_scale); add(layer->stroke_gradient_focal_x); add(layer->stroke_gradient_focal_y);
        add_gradient_stops(layer->stroke_gradient_stops);

        add(layer->background_enabled); add_anim(layer->background_enabled_prop);
        add((quint32)layer->background_color); add(layer->background_opacity);
        add_anim(layer->background_opacity_prop);
        add(layer->background_padding_x); add_anim(layer->background_padding_x_prop);
        add(layer->background_padding_y); add_anim(layer->background_padding_y_prop);
        add(layer->background_padding_left); add_anim(layer->background_padding_left_prop);
        add(layer->background_padding_right); add_anim(layer->background_padding_right_prop);
        add(layer->background_padding_top); add_anim(layer->background_padding_top_prop);
        add(layer->background_padding_bottom); add_anim(layer->background_padding_bottom_prop);
        add(layer->background_corner_radius); add_anim(layer->background_corner_radius_prop);
        add(layer->background_corner_radius_tl); add_anim(layer->background_corner_radius_tl_prop);
        add(layer->background_corner_radius_tr); add_anim(layer->background_corner_radius_tr_prop);
        add(layer->background_corner_radius_br); add_anim(layer->background_corner_radius_br_prop);
        add(layer->background_corner_radius_bl); add_anim(layer->background_corner_radius_bl_prop);
        add((int)layer->background_corner_type); add(layer->background_fill_type);
        add((quint32)layer->background_stroke_color); add(layer->background_stroke_width);
        add(layer->background_stroke_opacity); add(layer->background_stroke_fill_type);
        add_anim(layer->background_stroke_width_prop); add_anim(layer->background_stroke_opacity_prop);
        add_anim(layer->background_color_a); add_anim(layer->background_color_r);
        add_anim(layer->background_color_g); add_anim(layer->background_color_b);
        add_anim(layer->background_stroke_color_a); add_anim(layer->background_stroke_color_r);
        add_anim(layer->background_stroke_color_g); add_anim(layer->background_stroke_color_b);
        add(layer->background_gradient_type); add(layer->background_gradient_spread);
        add((quint32)layer->background_gradient_start_color); add((quint32)layer->background_gradient_end_color);
        add(layer->background_gradient_start_pos); add(layer->background_gradient_end_pos);
        add(layer->background_gradient_start_opacity); add(layer->background_gradient_end_opacity);
        add(layer->background_gradient_opacity); add(layer->background_gradient_angle);
        add(layer->background_gradient_center_x); add(layer->background_gradient_center_y);
        add(layer->background_gradient_scale); add(layer->background_gradient_focal_x); add(layer->background_gradient_focal_y);
        add_gradient_stops(layer->background_gradient_stops);

        add((int)layer->shape_type); add(layer->rect_width); add(layer->rect_height);
        add(layer->path_closed); add((quint64)layer->path_points.size());
        for (const auto &point : layer->path_points) {
            add(point.x); add(point.y); add(point.in_x); add(point.in_y);
            add(point.out_x); add(point.out_y); add(point.has_in);
            add(point.has_out); add(point.smooth); add(point.starts_subpath);
            add(point.corner_radius);
        }
        add(layer->image_width); add(layer->image_height);
        add(layer->shape_points); add(layer->shape_sides);
        add(layer->shape_inner_radius); add(layer->shape_outer_radius); add(layer->shape_roundness);
        add(layer->shape_inner_roundness);
        add(layer->corner_radius); add(layer->corner_radius_tl); add(layer->corner_radius_tr);
        add(layer->corner_radius_br); add(layer->corner_radius_bl); add(layer->corner_bevel_roundness);
        add(layer->scale_stroke_with_shape); add(layer->scale_corners_with_shape);

        /* Legacy shadow fields are still consumed when older projects have not
         * yet been migrated into the stackable effects vector. */
        add(layer->shadow_enabled); add((quint32)layer->shadow_color);
        add(layer->shadow_opacity); add(layer->shadow_distance); add(layer->shadow_angle);
        add(layer->shadow_blur); add(layer->shadow_spread); add((int)layer->shadow_blur_type);
        add(layer->long_shadow_enabled); add((quint32)layer->long_shadow_color);
        add(layer->long_shadow_opacity); add(layer->long_shadow_length); add(layer->long_shadow_angle);
        add(layer->long_shadow_falloff); add((int)layer->long_shadow_blur_type); add(layer->long_shadow_blur);
        add_anim(layer->shadow_enabled_prop); add_anim(layer->shadow_opacity_prop);
        add_anim(layer->shadow_distance_prop); add_anim(layer->shadow_angle_prop);
        add_anim(layer->shadow_blur_prop); add_anim(layer->shadow_spread_prop);
        add_anim(layer->shadow_color_a); add_anim(layer->shadow_color_r);
        add_anim(layer->shadow_color_g); add_anim(layer->shadow_color_b);

        const QString image_path = QString::fromStdString(
            effective_external_string(*layer, ExternalPropertyPaths::ImagePath, layer->image_path));
        add(image_path); add((int)layer->scale_filter);
        add(layer->image_box_lock_aspect_ratio);
        add((int)layer->image_box_mode); add(layer->image_size_auto_fit);
        add(layer->image_crop_when_outside_box); add(layer->image_anchor_x); add(layer->image_anchor_y);
        if (!image_path.isEmpty()) {
            const QFileInfo image_info(image_path);
            add(image_info.exists());
            add(image_info.size());
            add(image_info.lastModified().toMSecsSinceEpoch());
        }

        add((quint64)layer->effects.size());
        for (const auto &effect : layer->effects) add_effect(effect);
        const quint64 enabled_transition_count = static_cast<quint64>(std::count_if(
            layer->transitions.begin(), layer->transitions.end(),
            [](const LayerTransition &transition) { return transition.enabled; }));
        add(enabled_transition_count);
        for (const auto &transition : layer->transitions) {
            if (!transition.enabled)
                continue;
            // IDs, preset names and display labels are editor metadata and do
            // not affect pixels. Hashing them invalidated every cached frame
            // after copy/paste or a simple rename.
            add((int)transition.kind); add((int)transition.type);
            add((int)transition.edge); add((int)transition.unit); add((int)transition.direction);
            add((int)transition.easing); add(transition.duration); add(transition.blur_amount);
            add(transition.scale_from); add(transition.offset); add(transition.stagger);
            add(transition.softness); add(transition.reverse_order);
            add(transition.blocks_columns); add(transition.blocks_rows);
            add(transition.random_seed); add(QString::fromStdString(transition.image_path));
            add(transition.image_channel); add(transition.invert); add(transition.clockwise);
            add(transition.center_x); add(transition.center_y); add(transition.rotation);
            add(transition.aspect); add(transition.profile);
            if (!transition.image_path.empty()) {
                const QFileInfo matte_info(QString::fromStdString(transition.image_path));
                add(matte_info.exists()); add(matte_info.size());
                add(matte_info.lastModified().toMSecsSinceEpoch());
            }
        }
    }
    return QString::fromLatin1(hash.result().toHex());
}


QString CacheManager::evaluatedVisualStateHash(const Title &title, double time,
                                               const QString &known_content_hash) const
{
    /* The immutable content hash identifies the title/cue variant. This second,
     * deliberately small hash identifies only the values that can change at a
     * timeline sample. Equal hashes therefore mean equal renderer input and an
     * already rendered frame can be reused without invoking Cairo again. */
    QCryptographicHash hash(QCryptographicHash::Sha1);
    hash.addData((known_content_hash.isEmpty() ? contentHash(title) : known_content_hash).toUtf8());
    auto add = [&](const auto &value) {
        QByteArray bytes;
        QDataStream stream(&bytes, QIODevice::WriteOnly);
        stream.setVersion(QDataStream::Qt_5_15);
        stream.setByteOrder(QDataStream::LittleEndian);
        stream << value;
        hash.addData(bytes);
    };
    auto add_anim = [&](const AnimatedProperty &p) {
        if (p.is_animated()) add(p.evaluate(time));
    };
    auto add_vec = [&](const AnimatedVec2Property &p) {
        if (!p.is_animated()) return;
        const Vec2Value v = p.evaluate(time);
        add(v.x); add(v.y);
    };
    auto add_effect = [&](const LayerEffect &e) {
        add_anim(e.enabled_prop); add_anim(e.opacity_prop); add_anim(e.size_prop);
        add_anim(e.distance_prop); add_anim(e.angle_prop); add_anim(e.spread_prop);
        add_anim(e.falloff_prop); add_anim(e.amount_prop); add_anim(e.scale_prop);
        add_anim(e.softness_prop); add_anim(e.roundness_prop); add_anim(e.speed_prop);
        add_anim(e.center_x_prop); add_anim(e.center_y_prop); add_anim(e.complexity_prop);
        add_anim(e.evolution_prop); add_anim(e.stroke_width_prop); add_anim(e.stroke_opacity_prop);
        add_anim(e.padding_left_prop); add_anim(e.padding_right_prop); add_anim(e.padding_top_prop);
        add_anim(e.padding_bottom_prop); add_anim(e.corner_radius_tl_prop);
        add_anim(e.corner_radius_tr_prop); add_anim(e.corner_radius_br_prop);
        add_anim(e.corner_radius_bl_prop); add_anim(e.color_a); add_anim(e.color_r);
        add_anim(e.color_g); add_anim(e.color_b); add_anim(e.stroke_color_a);
        add_anim(e.stroke_color_r); add_anim(e.stroke_color_g); add_anim(e.stroke_color_b);
        add_anim(e.secondary_color_a); add_anim(e.secondary_color_r);
        add_anim(e.secondary_color_g); add_anim(e.secondary_color_b);
        if (e.type == LayerEffectType::Noise && e.effect_animated && e.enabled)
            add(time);
    };

    for (const auto &layer : title.layers) {
        if (!layer) continue;
        /* Visibility boundaries are renderer state even when no numeric
         * property is animated. Hash the resolved active flag, not raw time. */
        add(layer->visible && time >= layer->in_time && time <= layer->out_time);
        add_vec(layer->position); add_vec(layer->scale); add_anim(layer->rotation);
        add(layer->transform_quad_tl_x); add(layer->transform_quad_tl_y);
        add(layer->transform_quad_tr_x); add(layer->transform_quad_tr_y);
        add(layer->transform_quad_br_x); add(layer->transform_quad_br_y);
        add(layer->transform_quad_bl_x); add(layer->transform_quad_bl_y);
        add_anim(layer->opacity); add_vec(layer->size); add_vec(layer->image_size); add_vec(layer->origin_prop);
        add_anim(layer->font_size_prop); add_anim(layer->char_tracking_prop);
        add_anim(layer->char_scale_x_prop); add_anim(layer->char_scale_y_prop);
        add_anim(layer->baseline_shift_prop); add_anim(layer->text_color_a);
        add_anim(layer->text_color_r); add_anim(layer->text_color_g); add_anim(layer->text_color_b);
        add_anim(layer->paragraph_indent_left_prop); add_anim(layer->paragraph_indent_right_prop);
        add_anim(layer->paragraph_indent_first_line_prop); add_anim(layer->paragraph_space_before_prop);
        add_anim(layer->paragraph_space_after_prop); add_anim(layer->fill_color_a);
        add_anim(layer->fill_color_r); add_anim(layer->fill_color_g); add_anim(layer->fill_color_b);
        add_anim(layer->background_enabled_prop); add_anim(layer->background_opacity_prop);
        add_anim(layer->background_padding_x_prop); add_anim(layer->background_padding_y_prop);
        add_anim(layer->background_padding_left_prop); add_anim(layer->background_padding_right_prop);
        add_anim(layer->background_padding_top_prop); add_anim(layer->background_padding_bottom_prop);
        add_anim(layer->background_corner_radius_prop); add_anim(layer->background_corner_radius_tl_prop);
        add_anim(layer->background_corner_radius_tr_prop); add_anim(layer->background_corner_radius_br_prop);
        add_anim(layer->background_corner_radius_bl_prop); add_anim(layer->background_stroke_width_prop);
        add_anim(layer->background_stroke_opacity_prop); add_anim(layer->background_color_a);
        add_anim(layer->background_color_r); add_anim(layer->background_color_g);
        add_anim(layer->background_color_b); add_anim(layer->background_stroke_color_a);
        add_anim(layer->background_stroke_color_r); add_anim(layer->background_stroke_color_g);
        add_anim(layer->background_stroke_color_b); add_anim(layer->shadow_enabled_prop);
        add_anim(layer->shadow_opacity_prop); add_anim(layer->shadow_distance_prop);
        add_anim(layer->shadow_angle_prop); add_anim(layer->shadow_blur_prop);
        add_anim(layer->shadow_spread_prop); add_anim(layer->shadow_color_a);
        add_anim(layer->shadow_color_r); add_anim(layer->shadow_color_g); add_anim(layer->shadow_color_b);
        for (const auto &effect : layer->effects) add_effect(effect);
        for (const auto &transition : layer->transitions) {
            if (!transition.enabled)
                continue;
            add(layer_transition_progress(transition, layer->in_time, layer->out_time, time));
        }
    }
    return QString::fromLatin1(hash.result().toHex());
}


QString CacheManager::adaptiveVisualStateHash(const Title &title, double time,
                                              const QString &known_content_hash) const
{
    /* Conservative perceptual sampling. Values are quantized below a visible
     * pixel/opacity threshold so extremely slow animation can share a sample
     * while visibility boundaries and discrete states remain exact. */
    QCryptographicHash hash(QCryptographicHash::Sha1);
    hash.addData((known_content_hash.isEmpty() ? contentHash(title) : known_content_hash).toUtf8());
    auto add = [&](const auto &value) {
        QByteArray bytes;
        QDataStream stream(&bytes, QIODevice::WriteOnly);
        stream.setVersion(QDataStream::Qt_5_15);
        stream.setByteOrder(QDataStream::LittleEndian);
        stream << value;
        hash.addData(bytes);
    };
    auto quant = [](double value, double step) -> qint64 {
        if (!std::isfinite(value) || step <= 0.0)
            return 0;
        return qRound64(value / step);
    };
    auto add_anim = [&](const AnimatedProperty &p, double step) {
        if (p.is_animated()) add(quant(p.evaluate(time), step));
    };
    auto add_vec = [&](const AnimatedVec2Property &p, double step) {
        if (!p.is_animated()) return;
        const Vec2Value v = p.evaluate(time);
        add(quant(v.x, step)); add(quant(v.y, step));
    };
    auto add_effect = [&](const LayerEffect &e) {
        add_anim(e.enabled_prop, 1.0);
        add_anim(e.opacity_prop, 0.002);
        add_anim(e.size_prop, 0.25);
        add_anim(e.distance_prop, 0.25);
        add_anim(e.angle_prop, 0.05);
        add_anim(e.spread_prop, 0.25);
        add_anim(e.falloff_prop, 0.002);
        add_anim(e.amount_prop, 0.002); add_anim(e.scale_prop, 0.002);
        add_anim(e.softness_prop, 0.002); add_anim(e.roundness_prop, 0.002);
        add_anim(e.speed_prop, 0.002); add_anim(e.center_x_prop, 0.0005);
        add_anim(e.center_y_prop, 0.0005); add_anim(e.complexity_prop, 0.05);
        add_anim(e.evolution_prop, 0.01);
        add_anim(e.stroke_width_prop, 0.25);
        add_anim(e.stroke_opacity_prop, 0.002);
        add_anim(e.padding_left_prop, 0.25); add_anim(e.padding_right_prop, 0.25);
        add_anim(e.padding_top_prop, 0.25); add_anim(e.padding_bottom_prop, 0.25);
        add_anim(e.corner_radius_tl_prop, 0.25); add_anim(e.corner_radius_tr_prop, 0.25);
        add_anim(e.corner_radius_br_prop, 0.25); add_anim(e.corner_radius_bl_prop, 0.25);
        add_anim(e.color_a, 1.0 / 255.0); add_anim(e.color_r, 1.0 / 255.0);
        add_anim(e.color_g, 1.0 / 255.0); add_anim(e.color_b, 1.0 / 255.0);
        add_anim(e.stroke_color_a, 1.0 / 255.0); add_anim(e.stroke_color_r, 1.0 / 255.0);
        add_anim(e.stroke_color_g, 1.0 / 255.0); add_anim(e.stroke_color_b, 1.0 / 255.0);
        add_anim(e.secondary_color_a, 1.0 / 255.0);
        add_anim(e.secondary_color_r, 1.0 / 255.0);
        add_anim(e.secondary_color_g, 1.0 / 255.0);
        add_anim(e.secondary_color_b, 1.0 / 255.0);
        if (e.type == LayerEffectType::Noise && e.effect_animated && e.enabled)
            add(quant(time, 1.0 / 240.0));
    };

