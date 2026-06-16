#pragma once

#include "cache-manager.h"

#include <QWidget>
#include <memory>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QSpinBox;
class QPushButton;

class PrerenderDock : public QWidget {
    Q_OBJECT
public:
    explicit PrerenderDock(QWidget *parent = nullptr);
    void setTitle(std::shared_ptr<Title> title);
    void setPlayhead(double time);

signals:
    void cacheWorkAreaRequested();
    void cacheEntireTimelineRequested();

private:
    void buildUi();
    void applySettings();
    void updateStatus();
    void updateCacheEnabled(bool enabled);

    std::shared_ptr<Title> title_;
    double playhead_ = 0.0;
    QLabel *status_ = nullptr;
    QComboBox *start_mode_ = nullptr;
    QComboBox *playback_mode_ = nullptr;
    QSpinBox *skip_frames_ = nullptr;
    QDoubleSpinBox *speed_percent_ = nullptr;
    QCheckBox *cache_enabled_ = nullptr;
    QCheckBox *cached_only_ = nullptr;
    QCheckBox *play_every_frame_ = nullptr;
    QPushButton *pause_resume_ = nullptr;
    QPushButton *cache_work_area_ = nullptr;
    QPushButton *cache_timeline_ = nullptr;
    QLabel *diagnostics_ = nullptr;
};
