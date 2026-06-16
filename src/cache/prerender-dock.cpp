#include "prerender-dock.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

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
    start_mode_->addItems({QStringLiteral("From current time"), QStringLiteral("From beginning")});
    form->addRow(QStringLiteral("Start"), start_mode_);

    playback_mode_ = new QComboBox(this);
    playback_mode_->addItems({QStringLiteral("Loop"), QStringLiteral("Ping-pong loop"), QStringLiteral("Play once")});
    form->addRow(QStringLiteral("Mode"), playback_mode_);

    cache_enabled_ = new QCheckBox(QStringLiteral("Enable cache"), this);
    cache_enabled_->setChecked(CacheManager::instance().cacheEnabled());
    form->addRow(QString(), cache_enabled_);

    skip_frames_ = new QSpinBox(this);
    skip_frames_->setRange(0, 30);
    form->addRow(QStringLiteral("Skip frames"), skip_frames_);

    speed_percent_ = new QDoubleSpinBox(this);
    speed_percent_->setRange(1.0, 400.0);
    speed_percent_->setValue(100.0);
    speed_percent_->setSuffix(QStringLiteral("%"));
    form->addRow(QStringLiteral("Speed"), speed_percent_);

    cached_only_ = new QCheckBox(QStringLiteral("Play cached frames only"), this);
    form->addRow(QString(), cached_only_);

    play_every_frame_ = new QCheckBox(QStringLiteral("Play every frame, even if slow"), this);
    form->addRow(QString(), play_every_frame_);
    root->addLayout(form);

    auto *grid = new QGridLayout();
    auto add_button = [&](const QString &text, int row, int col, auto slot) {
        auto *button = new QPushButton(text, this);
        connect(button, &QPushButton::clicked, this, slot);
        grid->addWidget(button, row, col);
        return button;
    };
    add_button(QStringLiteral("Clear RAM Cache"), 0, 0, []() { CacheManager::instance().clearRam(); });
    add_button(QStringLiteral("Clear Disk Cache"), 0, 1, []() { CacheManager::instance().clearDisk(); });
    add_button(QStringLiteral("Clear All Cache"), 1, 0, []() { CacheManager::instance().clearAll(); });
    pause_resume_ = add_button(QStringLiteral("Pause Prerender"), 1, 1, [this]() {
        if (CacheManager::instance().prerenderPaused())
            CacheManager::instance().resumePrerender();
        else
            CacheManager::instance().pausePrerender();
        updateStatus();
    });
    cache_work_area_ = add_button(QStringLiteral("Cache Work Area"), 2, 0, [this]() {
        applySettings();
        if (title_) CacheManager::instance().queueWorkArea(title_);
        emit cacheWorkAreaRequested();
    });
    cache_timeline_ = add_button(QStringLiteral("Cache Entire Timeline"), 2, 1, [this]() {
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
                                   ? QStringLiteral("Resume Prerender")
                                   : QStringLiteral("Pause Prerender"));
    if (!status_) return;
    if (!title_) {
        status_->setText(QStringLiteral("No title loaded."));
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
        status_->setText(QStringLiteral("Prerender queue is active. Timeline colors show queued, rendering, RAM, disk, and stale frames."));
    }
    if (diagnostics_) {
        const LiveCueCacheStats stats = CacheManager::instance().liveCueStats();
        diagnostics_->setText(QStringLiteral("Live Text Cue diagnostics\nHits: %1\nMisses: %2\nReuses: %3\nInvalidations: %4")
                                  .arg(stats.hits)
                                  .arg(stats.misses)
                                  .arg(stats.reuses)
                                  .arg(stats.invalidations));
    }
}
