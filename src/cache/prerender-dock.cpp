#include "prerender-dock.h"
#include "title-localization.h"

#include <algorithm>

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

namespace {
constexpr const char *kPrerenderStartModeKey = "Prerender/StartMode";
constexpr const char *kPrerenderPlaybackModeKey = "Prerender/PlaybackMode";
constexpr const char *kPrerenderSkipFramesKey = "Prerender/SkipFrames";
constexpr const char *kPrerenderSpeedPercentKey = "Prerender/SpeedPercent";
constexpr const char *kPrerenderPlayAfterRenderingKey = "Prerender/PlayAfterRendering";
constexpr const char *kPrerenderPlayEveryFrameKey = "Prerender/PlayEveryFrame";
}

PrerenderDock::PrerenderDock(QWidget *parent)
    : QWidget(parent)
{
    buildUi();
    connect(&CacheManager::instance(), &CacheManager::queueChanged, this, &PrerenderDock::updateStatus);
    connect(&CacheManager::instance(), &CacheManager::cacheStatesChanged, this, [this]() { updateStatus(); });
    connect(&CacheManager::instance(), &CacheManager::cacheEnabledChanged, this, &PrerenderDock::updateCacheEnabled);
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
    start_mode_->addItems({obsgs_tr("OBSTitles.FromCurrentTime"), obsgs_tr("OBSTitles.FromBeginning")});
    form->addRow(obsgs_tr("OBSTitles.Start"), start_mode_);

    playback_mode_ = new QComboBox(this);
    playback_mode_->addItems({obsgs_tr("OBSTitles.Loop"), obsgs_tr("OBSTitles.PingPongLoop"), obsgs_tr("OBSTitles.PlayOnce")});
    form->addRow(obsgs_tr("OBSTitles.Mode"), playback_mode_);

    cache_enabled_ = new QCheckBox(obsgs_tr("OBSTitles.EnableCache"), this);
    cache_enabled_->setChecked(CacheManager::instance().cacheEnabled());
    form->addRow(QString(), cache_enabled_);

    QSettings prerender_settings(QStringLiteral("OBSGraphicsStudioPro"), QStringLiteral("Dock"));

    skip_frames_ = new QSpinBox(this);
    skip_frames_->setRange(0, 30);
    form->addRow(obsgs_tr("OBSTitles.SkipFrames"), skip_frames_);

    speed_percent_ = new QDoubleSpinBox(this);
    speed_percent_->setRange(1.0, 400.0);
    speed_percent_->setValue(100.0);
    speed_percent_->setSuffix(QStringLiteral("%"));
    form->addRow(obsgs_tr("OBSTitles.Speed"), speed_percent_);

    cached_only_ = new QCheckBox(obsgs_tr("OBSTitles.PlayAfterRendering"), this);
    form->addRow(QString(), cached_only_);

    play_every_frame_ = new QCheckBox(obsgs_tr("OBSTitles.PlayEveryFrameEvenIfSlow"), this);
    form->addRow(QString(), play_every_frame_);

    start_mode_->setCurrentIndex(std::clamp(prerender_settings.value(QString::fromUtf8(kPrerenderStartModeKey), 0).toInt(), 0, start_mode_->count() - 1));
    playback_mode_->setCurrentIndex(std::clamp(prerender_settings.value(QString::fromUtf8(kPrerenderPlaybackModeKey), 0).toInt(), 0, playback_mode_->count() - 1));
    skip_frames_->setValue(std::clamp(prerender_settings.value(QString::fromUtf8(kPrerenderSkipFramesKey), 0).toInt(), skip_frames_->minimum(), skip_frames_->maximum()));
    speed_percent_->setValue(std::clamp(prerender_settings.value(QString::fromUtf8(kPrerenderSpeedPercentKey), 100.0).toDouble(), speed_percent_->minimum(), speed_percent_->maximum()));
    cached_only_->setChecked(prerender_settings.value(QString::fromUtf8(kPrerenderPlayAfterRenderingKey), false).toBool());
    play_every_frame_->setChecked(prerender_settings.value(QString::fromUtf8(kPrerenderPlayEveryFrameKey), false).toBool());

    root->addLayout(form);

    auto *grid = new QGridLayout();
    auto add_button = [&](const QString &text, int row, int col, auto slot) {
        auto *button = new QPushButton(text, this);
        connect(button, &QPushButton::clicked, this, slot);
        grid->addWidget(button, row, col);
        return button;
    };
    add_button(obsgs_tr("OBSTitles.ClearRamCache"), 0, 0, []() { CacheManager::instance().clearRam(); });
    add_button(obsgs_tr("OBSTitles.ClearDiskCache"), 0, 1, []() { CacheManager::instance().clearDisk(); });
    add_button(obsgs_tr("OBSTitles.ClearAllCache"), 1, 0, []() { CacheManager::instance().clearAll(); });
    pause_resume_ = add_button(obsgs_tr("OBSTitles.PausePrerender"), 1, 1, [this]() {
        if (CacheManager::instance().prerenderPaused())
            CacheManager::instance().resumePrerender();
        else
            CacheManager::instance().pausePrerender();
        updateStatus();
    });
    cache_work_area_ = add_button(obsgs_tr("OBSTitles.CacheWorkArea"), 2, 0, [this]() {
        applySettings();
        if (title_) CacheManager::instance().queueWorkArea(title_);
        emit cacheWorkAreaRequested();
    });
    cache_timeline_ = add_button(obsgs_tr("OBSTitles.CacheEntireTimeline"), 2, 1, [this]() {
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
    connect(skip_frames_, QOverload<int>::of(&QSpinBox::valueChanged), this, &PrerenderDock::applySettings);
    connect(speed_percent_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &PrerenderDock::applySettings);
    connect(cached_only_, &QCheckBox::toggled, this, &PrerenderDock::applySettings);
    connect(play_every_frame_, &QCheckBox::toggled, this, &PrerenderDock::applySettings);
    connect(cache_enabled_, &QCheckBox::toggled, this, [](bool enabled) {
        CacheManager::instance().setCacheEnabled(enabled);
    });
    applySettings();
}

void PrerenderDock::applySettings()
{
    const bool play_every_frame = play_every_frame_ && play_every_frame_->isChecked();
    if (skip_frames_) skip_frames_->setEnabled(!play_every_frame);
    if (speed_percent_) speed_percent_->setEnabled(!play_every_frame);

    CachePlaybackSettings settings;
    settings.from_beginning = start_mode_ && start_mode_->currentIndex() == 1;
    settings.loop = playback_mode_ && playback_mode_->currentIndex() == 0;
    settings.ping_pong = playback_mode_ && playback_mode_->currentIndex() == 1;
    settings.skip_frames = play_every_frame ? 0 : (skip_frames_ ? skip_frames_->value() : 0);
    settings.speed_percent = play_every_frame ? 100.0 : (speed_percent_ ? speed_percent_->value() : 100.0);
    settings.cached_frames_only = cached_only_ && cached_only_->isChecked();
    settings.play_every_frame = play_every_frame;
    CacheManager::instance().setPlaybackSettings(settings);

    QSettings prerender_settings(QStringLiteral("OBSGraphicsStudioPro"), QStringLiteral("Dock"));
    if (start_mode_) prerender_settings.setValue(QString::fromUtf8(kPrerenderStartModeKey), start_mode_->currentIndex());
    if (playback_mode_) prerender_settings.setValue(QString::fromUtf8(kPrerenderPlaybackModeKey), playback_mode_->currentIndex());
    if (skip_frames_) prerender_settings.setValue(QString::fromUtf8(kPrerenderSkipFramesKey), skip_frames_->value());
    if (speed_percent_) prerender_settings.setValue(QString::fromUtf8(kPrerenderSpeedPercentKey), speed_percent_->value());
    if (cached_only_) prerender_settings.setValue(QString::fromUtf8(kPrerenderPlayAfterRenderingKey), cached_only_->isChecked());
    if (play_every_frame_) prerender_settings.setValue(QString::fromUtf8(kPrerenderPlayEveryFrameKey), play_every_frame_->isChecked());
}

void PrerenderDock::updateCacheEnabled(bool enabled)
{
    if (cache_enabled_ && cache_enabled_->isChecked() != enabled) {
        QSignalBlocker blocker(cache_enabled_);
        cache_enabled_->setChecked(enabled);
    }
    updateStatus();
}

void PrerenderDock::updateStatus()
{
    if (pause_resume_)
        pause_resume_->setText(CacheManager::instance().prerenderPaused()
                                   ? obsgs_tr("OBSTitles.ResumePrerender")
                                   : obsgs_tr("OBSTitles.PausePrerender"));
    if (!status_) return;
    if (!title_) {
        status_->setText(obsgs_tr("OBSTitles.NoTitleLoaded"));
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
        status_->setText(obsgs_tr("OBSTitles.PrerenderQueueStatus"));
    }
    if (diagnostics_) {
        const LiveCueCacheStats stats = CacheManager::instance().liveCueStats();
        auto format_bytes = [](quint64 bytes) {
            const double mib = (double)bytes / 1024.0 / 1024.0;
            if (mib < 1024.0)
                return QStringLiteral("%1 MB").arg(mib, 0, 'f', 1);
            return QStringLiteral("%1 GB").arg(mib / 1024.0, 0, 'f', 2);
        };
        diagnostics_->setText(obsgs_tr("OBSTitles.CacheUsageDiagnostics")
                                  .arg(format_bytes(CacheManager::instance().ramBytesUsed()),
                                       format_bytes(CacheManager::instance().ramBytesLimit()),
                                       format_bytes(CacheManager::instance().diskBytesUsed()))
                                  .arg(stats.hits)
                                  .arg(stats.misses)
                                  .arg(stats.reuses)
                                  .arg(stats.invalidations));
    }
}
