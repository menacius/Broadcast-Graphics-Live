#pragma once

#include "title-data.h"

#include <QObject>
#include <QImage>
#include <QHash>
#include <QSet>
#include <QTimer>
#include <QMutex>
#include <QDateTime>

#include <memory>
#include <functional>
#include <vector>

enum class FrameCacheState {
    NotCached,
    Queued,
    Rendering,
    CachedRam,
    CachedDisk,
    Stale,
    Disabled
};

enum class TitleCacheability {
    Cacheable,
    PartiallyCacheable,
    NonCacheable
};

enum class CachePlaybackMode {
    Loop,
    PingPong,
    PlayOnce
};

struct CachePlaybackSettings {
    bool from_beginning = false;
    bool loop = true;
    bool ping_pong = false;
    bool cached_frames_only = false;
    bool play_every_frame = false;
    int skip_frames = 0;
    double speed_percent = 100.0;
};

struct LiveCueCacheStats {
    quint64 hits = 0;
    quint64 misses = 0;
    quint64 reuses = 0;
    quint64 invalidations = 0;
};

struct CacheFrameKey {
    QString title_id;
    QString content_hash;
    int frame = 0;
    int width = 0;
    int height = 0;

    QString toString() const;
    bool operator==(const CacheFrameKey &other) const;
};

uint qHash(const CacheFrameKey &key, uint seed = 0);

class RamFrameCache : public QObject {
    Q_OBJECT
public:
    explicit RamFrameCache(QObject *parent = nullptr);
    bool get(const CacheFrameKey &key, QImage &image) const;
    void put(const CacheFrameKey &key, const QImage &image);
    void remove(const CacheFrameKey &key);
    void clear();
    qsizetype count() const;

private:
    mutable QMutex mutex_;
    QHash<CacheFrameKey, QImage> frames_;
};

class DiskFrameCache : public QObject {
    Q_OBJECT
public:
    explicit DiskFrameCache(QObject *parent = nullptr);
    bool get(const CacheFrameKey &key, QImage &image) const;
    void put(const CacheFrameKey &key, const QImage &image);
    void remove(const CacheFrameKey &key);
    void clear();
    QString cacheDirectory() const { return cache_dir_; }

private:
    QString pathForKey(const CacheFrameKey &key) const;
    QString cache_dir_;
};

class CacheStateTracker : public QObject {
    Q_OBJECT
public:
    explicit CacheStateTracker(QObject *parent = nullptr);
    FrameCacheState state(const CacheFrameKey &key) const;
    FrameCacheState stateForFrame(const QString &title_id, int frame) const;
    void setState(const CacheFrameKey &key, FrameCacheState state);
    void markRange(const QString &title_id, int first_frame, int last_frame, FrameCacheState state);
    void clearTitle(const QString &title_id);
    void clear();
    QHash<int, FrameCacheState> titleStates(const QString &title_id) const;

signals:
    void stateChanged(const QString &title_id, int first_frame, int last_frame);

private:
    mutable QMutex mutex_;
    QHash<CacheFrameKey, FrameCacheState> states_;
};

class RenderQueueManager : public QObject {
    Q_OBJECT
public:
    enum class PriorityBand {
        Visible = 0,
        AfterCurrent = 1,
        BeforeCurrent = 2,
        WorkArea = 3,
        FullTimeline = 4,
        LiveCue = -1
    };

    struct Job {
        CacheFrameKey key;
        std::shared_ptr<Title> title;
        double time = 0.0;
        int priority = 0;
        bool live_cue = false;
        int cue_row = -1;
    };

    explicit RenderQueueManager(QObject *parent = nullptr);
    void enqueue(const Job &job);
    void setAcceptingJobs(bool accepting);
    void cancelTitle(const QString &title_id);
    void cancelRange(const QString &title_id, int first_frame, int last_frame);
    void reprioritizeAround(const QString &title_id, int current_frame);
    bool takeNext(Job &job);
    bool contains(const CacheFrameKey &key) const;
    int queuedCount() const;
    void clear();

signals:
    void queueChanged();

private:
    mutable QMutex mutex_;
    QVector<Job> jobs_;
    QSet<QString> queued_keys_;
    bool accepting_jobs_ = true;
};

class CacheInvalidationManager : public QObject {
    Q_OBJECT
public:
    explicit CacheInvalidationManager(QObject *parent = nullptr);
    void invalidateAll(const std::shared_ptr<Title> &title);
    void invalidateRange(const std::shared_ptr<Title> &title, double start, double end);
    void invalidateLayer(const std::shared_ptr<Title> &title, const std::string &layer_id);

signals:
    void rangeInvalidated(const QString &title_id, int first_frame, int last_frame);

private:
    int frameForTime(double t) const;
};

class CacheManager : public QObject {
    Q_OBJECT
public:
    static CacheManager &instance();

    QImage requestFrame(const std::shared_ptr<Title> &title, double time, bool cached_only = false);
    void queueFrame(const std::shared_ptr<Title> &title, double time, RenderQueueManager::PriorityBand band);
    void queueRange(const std::shared_ptr<Title> &title, double start, double end, RenderQueueManager::PriorityBand band);
    void queueWholeTimeline(const std::shared_ptr<Title> &title);
    void queueWorkArea(const std::shared_ptr<Title> &title);
    void reprioritize(const std::shared_ptr<Title> &title, double current_time);

    void clearRam();
    void clearDisk();
    void clearAll();
    void pausePrerender();
    void resumePrerender();
    bool prerenderPaused() const { return paused_; }
    bool cacheEnabled() const { return cache_enabled_; }
    void setCacheEnabled(bool enabled);
    void setInteractiveBypass(bool bypass);
    TitleCacheability titleCacheability(const std::shared_ptr<Title> &title) const;
    QString titleCacheabilityMessage(const std::shared_ptr<Title> &title) const;

    void invalidateAll(const std::shared_ptr<Title> &title);
    void invalidateRange(const std::shared_ptr<Title> &title, double start, double end);
    void invalidateLayer(const std::shared_ptr<Title> &title, const std::string &layer_id);

    void queueLiveCue(const std::shared_ptr<Title> &title, int row);
    void preloadLiveCues(const std::shared_ptr<Title> &title, int current_row, int nearby_count);
    FrameCacheState liveCueState(const std::shared_ptr<Title> &title, int row) const;
    bool isLiveCueReady(const std::shared_ptr<Title> &title, int row);
    void invalidateLiveCue(const std::shared_ptr<Title> &title, int row);
    void invalidateLiveCues(const std::shared_ptr<Title> &title);
    LiveCueCacheStats liveCueStats() const { return live_cue_stats_; }

    CacheStateTracker *stateTracker() { return &state_tracker_; }
    CachePlaybackSettings playbackSettings() const { return playback_settings_; }
    void setPlaybackSettings(const CachePlaybackSettings &settings);
    double effectiveFrameRate() const;

signals:
    void frameReady(const QString &title_id, int frame);
    void cacheStatesChanged(const QString &title_id, int first_frame, int last_frame);
    void liveCueStateChanged(const QString &title_id, int row);
    void queueChanged();
    void cacheEnabledChanged(bool enabled);
    void diagnosticsChanged();

private slots:
    void processNextJob();

private:
    explicit CacheManager(QObject *parent = nullptr);
    CacheFrameKey keyForFrame(const Title &title, int frame) const;
    CacheFrameKey keyForTime(const Title &title, double time) const;
    QString contentHash(const Title &title) const;
    std::shared_ptr<Title> titleWithCueApplied(const std::shared_ptr<Title> &title, int row) const;
    CacheFrameKey liveCueKey(const std::shared_ptr<Title> &title, int row) const;
    int frameForTime(double t) const;
    double timeForFrame(int frame) const;

    RamFrameCache ram_cache_;
    DiskFrameCache disk_cache_;
    CacheStateTracker state_tracker_;
    RenderQueueManager queue_;
    CacheInvalidationManager invalidation_;
    QTimer worker_timer_;
    bool paused_ = false;
    bool cache_enabled_ = true;
    bool interactive_bypass_ = false;
    QString last_reprioritize_title_id_;
    int last_reprioritize_frame_ = -1;
    CachePlaybackSettings playback_settings_;
    QHash<QString, FrameCacheState> live_cue_states_;
    LiveCueCacheStats live_cue_stats_;
};
