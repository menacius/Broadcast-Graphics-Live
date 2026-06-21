#pragma once

#include "cache-manager.h"

#include <QWidget>
#include <memory>

class QCheckBox;
class QComboBox;
class QLabel;
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

    std::shared_ptr<Title> title_;
    double playhead_ = 0.0;
    QLabel *status_ = nullptr;
    QComboBox *start_mode_ = nullptr;
    QComboBox *playback_mode_ = nullptr;
    QCheckBox *cached_only_ = nullptr;
    QPushButton *pause_resume_ = nullptr;
    QPushButton *cache_work_area_ = nullptr;
    QPushButton *cache_timeline_ = nullptr;
    QLabel *diagnostics_ = nullptr;
};
