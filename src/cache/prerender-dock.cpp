#include "prerender-dock.h"
#include "title-localization.h"

#include <algorithm>

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

namespace {
constexpr const char *kPrerenderStartModeKey = "Prerender/StartMode";
constexpr const char *kPrerenderPlaybackModeKey = "Prerender/PlaybackMode";
constexpr const char *kPrerenderPlayAfterRenderingKey = "Prerender/PlayAfterRendering";
}

PrerenderDock::PrerenderDock(QWidget *parent)
    : QWidget(parent)
{
    buildUi();
    connect(&CacheManager::instance(), &CacheManager::queueChanged, this, &PrerenderDock::updateStatus);
    connect(&CacheManager::instance(), &CacheManager::cacheStatesChanged, this, [this]() { updateStatus(); });
    connect(&CacheManager::instance(), &CacheManager::cacheEnabledChanged, this, [this](bool) { updateStatus(); });
    connect(&CacheManager::instance(), &CacheManager::diagnosticsChanged, this, &PrerenderDock::updateStatus);
}

void PrerenderDock::setTitle(std::shared_ptr<Title> title)
{
    title_ = std::move(title);
    updateStatus();
}

void PrerenderDock::setPlayhead(double time)
{
    playhead_ = time;
    Q_UNUSED(playhead_);
}

void PrerenderDock::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(8);

    auto *form = new QFormLayout();
    form->setContentsMargins(0, 0, 0, 0);

    start_mode_ = new QComboBox(this);
    start_mode_->addItems({bgl_tr("OBSTitles.FromCurrentTime"), bgl_tr("OBSTitles.FromBeginning")});
    form->addRow(bgl_tr("OBSTitles.Start"), start_mode_);

    playback_mode_ = new QComboBox(this);
    playback_mode_->addItems({bgl_tr("OBSTitles.Loop"), bgl_tr("OBSTitles.PingPongLoop"), bgl_tr("OBSTitles.PlayOnce"), bgl_tr("OBSTitles.PlaybackMode")});
    form->addRow(bgl_tr("OBSTitles.Mode"), playback_mode_);

    QSettings prerender_settings(QStringLiteral("BroadcastGraphicsLive"), QStringLiteral("Dock"));

    cached_only_ = new QCheckBox(bgl_tr("OBSTitles.PlayAfterRendering"), this);
    form->addRow(QString(), cached_only_);

    start_mode_->setCurrentIndex(std::clamp(prerender_settings.value(QString::fromUtf8(kPrerenderStartModeKey), 0).toInt(), 0, start_mode_->count() - 1));
    playback_mode_->setCurrentIndex(std::clamp(prerender_settings.value(QString::fromUtf8(kPrerenderPlaybackModeKey), 0).toInt(), 0, playback_mode_->count() - 1));
    cached_only_->setChecked(prerender_settings.value(QString::fromUtf8(kPrerenderPlayAfterRenderingKey), false).toBool());

    root->addLayout(form);

    auto *grid = new QGridLayout();
    auto add_button = [&](const QString &text, int row, int col, auto slot) {
        auto *button = new QPushButton(text, this);
        connect(button, &QPushButton::clicked, this, slot);
        grid->addWidget(button, row, col);
        return button;
    };
    add_button(bgl_tr("OBSTitles.ClearRamCache"), 0, 0, []() { CacheManager::instance().clearRam(); });
    add_button(bgl_tr("OBSTitles.ClearDiskCache"), 0, 1, []() { CacheManager::instance().clearDisk(); });
    add_button(bgl_tr("OBSTitles.ClearAllCache"), 1, 0, []() { CacheManager::instance().clearAll(); });
    pause_resume_ = add_button(bgl_tr("OBSTitles.PausePrerender"), 1, 1, [this]() {
        if (CacheManager::instance().prerenderPaused())
            CacheManager::instance().resumePrerender();
        else
            CacheManager::instance().pausePrerender();
        updateStatus();
    });
    cache_work_area_ = add_button(bgl_tr("OBSTitles.CacheWorkArea"), 2, 0, [this]() {
        applySettings();
        if (title_) CacheManager::instance().queueWorkArea(title_);
        emit cacheWorkAreaRequested();
    });
    cache_timeline_ = add_button(bgl_tr("OBSTitles.CacheEntireTimeline"), 2, 1, [this]() {
        applySettings();
        if (title_) CacheManager::instance().queueWholeTimeline(title_);
        emit cacheEntireTimelineRequested();
    });
    root->addLayout(grid);

    status_ = new QLabel(this);
    status_->setWordWrap(true);
    root->addWidget(status_);
    diagnostics_ = new QLabel(this);
    diagnostics_->setWordWrap(true);
    root->addWidget(diagnostics_);
    root->addStretch(1);

    for (auto *combo : {start_mode_, playback_mode_})
        connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &PrerenderDock::applySettings);
    connect(cached_only_, &QCheckBox::toggled, this, &PrerenderDock::applySettings);
    applySettings();
}

void PrerenderDock::applySettings()
{
    CachePlaybackSettings settings;
    settings.from_beginning = start_mode_ && start_mode_->currentIndex() == 1;
    const int playback_index = playback_mode_ ? playback_mode_->currentIndex() : 0;
    settings.mode = playback_index == 1 ? CachePlaybackMode::PingPong
                  : playback_index == 2 ? CachePlaybackMode::PlayOnce
                                        : CachePlaybackMode::Loop;
    settings.follow_title_playback_mode = playback_index == 3;
    settings.skip_frames = 0;
    settings.speed_percent = 100.0;
    settings.cached_frames_only = cached_only_ && cached_only_->isChecked();
    settings.play_every_frame = false;
    CacheManager::instance().setPlaybackSettings(settings);

    QSettings prerender_settings(QStringLiteral("BroadcastGraphicsLive"), QStringLiteral("Dock"));
    if (start_mode_) prerender_settings.setValue(QString::fromUtf8(kPrerenderStartModeKey), start_mode_->currentIndex());
    if (playback_mode_) prerender_settings.setValue(QString::fromUtf8(kPrerenderPlaybackModeKey), playback_mode_->currentIndex());
    if (cached_only_) prerender_settings.setValue(QString::fromUtf8(kPrerenderPlayAfterRenderingKey), cached_only_->isChecked());
}

void PrerenderDock::updateStatus()
{
    if (pause_resume_)
        pause_resume_->setText(CacheManager::instance().prerenderPaused()
                                   ? bgl_tr("OBSTitles.ResumePrerender")
                                   : bgl_tr("OBSTitles.PausePrerender"));
    if (!status_) return;
    if (!title_) {
        status_->setText(bgl_tr("OBSTitles.NoTitleLoaded"));
        if (diagnostics_) diagnostics_->clear();
        return;
    }
    const bool enabled = CacheManager::instance().cacheEnabled();
    const TitleCacheability cacheability = CacheManager::instance().titleCacheability(title_);
    const bool frame_prerender_available = enabled && cacheability != TitleCacheability::NonCacheable;
    if (cache_work_area_) cache_work_area_->setEnabled(frame_prerender_available);
    if (cache_timeline_) cache_timeline_->setEnabled(frame_prerender_available);
    if (pause_resume_) pause_resume_->setEnabled(enabled);

    const QString message = CacheManager::instance().titleCacheabilityMessage(title_);
    if (!message.isEmpty()) {
        status_->setText(message);
    } else {
        status_->setText(bgl_tr("OBSTitles.PrerenderQueueStatus"));
    }
    if (diagnostics_) {
        const LiveCueCacheStats stats = CacheManager::instance().liveCueStats();
        auto format_bytes = [](quint64 bytes) {
            const double mib = (double)bytes / 1024.0 / 1024.0;
            if (mib < 1024.0)
                return QStringLiteral("%1 MB").arg(mib, 0, 'f', 1);
            return QStringLiteral("%1 GB").arg(mib / 1024.0, 0, 'f', 2);
        };
        diagnostics_->setText(bgl_tr("OBSTitles.CacheUsageDiagnostics")
                                  .arg(format_bytes(CacheManager::instance().ramBytesUsed()),
                                       format_bytes(CacheManager::instance().ramBytesLimit()),
                                       format_bytes(CacheManager::instance().diskBytesUsed()))
                                  .arg(stats.hits)
                                  .arg(stats.misses)
                                  .arg(stats.reuses)
                                  .arg(stats.invalidations));
    }
}
