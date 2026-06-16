#include "cache-manager.h"
#include "title-source.h"
#include "title-preferences.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QBuffer>
#include <QDataStream>
#include <QMutexLocker>
#include <QCoreApplication>
#include <QFile>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
double cache_obs_frame_rate()
{
    obs_video_info info = {};
    if (obs_get_video_info(&info) && info.fps_den > 0 && info.fps_num > 0)
        return (double)info.fps_num / (double)info.fps_den;
    return 60.0;
}

int cache_last_frame_for_title(const Title &title, double fps)
{
    return std::max(0, (int)std::ceil(std::max(0.0, title.duration) * std::max(1.0, fps)) - 1);
}

constexpr quint32 kRawFrameMagic = 0x4f475346; // OGSF
constexpr quint16 kRawFrameVersion = 1;
constexpr QImage::Format kDiskFrameFormat = QImage::Format_ARGB32_Premultiplied;
}

QString CacheFrameKey::toString() const
{
    return QStringLiteral("%1-%2-%3-%4x%5")
        .arg(title_id, content_hash)
        .arg(frame)
        .arg(width)
        .arg(height);
}

bool CacheFrameKey::operator==(const CacheFrameKey &other) const
{
    return title_id == other.title_id &&
           content_hash == other.content_hash &&
           frame == other.frame &&
           width == other.width &&
           height == other.height;
}

uint qHash(const CacheFrameKey &key, uint seed)
{
    return qHash(key.toString(), seed);
}

RamFrameCache::RamFrameCache(QObject *parent) : QObject(parent) {}

bool RamFrameCache::get(const CacheFrameKey &key, QImage &image) const
{
    QMutexLocker lock(&mutex_);
    auto it = frames_.find(key);
    if (it == frames_.end())
        return false;
    image = it.value();
    return !image.isNull();
}

void RamFrameCache::put(const CacheFrameKey &key, const QImage &image)
{
    if (image.isNull()) return;
    QMutexLocker lock(&mutex_);
    frames_.insert(key, image);
}

void RamFrameCache::remove(const CacheFrameKey &key)
{
    QMutexLocker lock(&mutex_);
    frames_.remove(key);
}

void RamFrameCache::clear()
{
    QMutexLocker lock(&mutex_);
    frames_.clear();
}

qsizetype RamFrameCache::count() const
{
    QMutexLocker lock(&mutex_);
    return frames_.size();
}

DiskFrameCache::DiskFrameCache(QObject *parent) : QObject(parent)
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    if (base.isEmpty())
        base = QDir::tempPath() + QStringLiteral("/OBSGraphicsStudioPro");
    cache_dir_ = QDir(base).filePath(QStringLiteral("frame-cache"));
    QDir().mkpath(cache_dir_);
}

QString DiskFrameCache::pathForKey(const CacheFrameKey &key) const
{
    const QByteArray digest = QCryptographicHash::hash(key.toString().toUtf8(), QCryptographicHash::Sha1).toHex();
    return QDir(cache_dir_).filePath(QString::fromLatin1(digest) + QStringLiteral(".ogsf"));
}

bool DiskFrameCache::get(const CacheFrameKey &key, QImage &image) const
{
    const QString path = pathForKey(key);
    if (!QFileInfo::exists(path))
        return false;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;

    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_5_15);
    stream.setByteOrder(QDataStream::LittleEndian);

    quint32 magic = 0;
    quint16 version = 0;
    quint16 format = 0;
    quint32 width = 0;
    quint32 height = 0;
    quint32 row_bytes = 0;
    stream >> magic >> version >> format >> width >> height >> row_bytes;
    if (stream.status() != QDataStream::Ok ||
        magic != kRawFrameMagic ||
        version != kRawFrameVersion ||
        format != (quint16)kDiskFrameFormat ||
        width == 0 || height == 0 ||
        width != (quint32)key.width ||
        height != (quint32)key.height ||
        row_bytes != width * 4)
        return false;

    QImage loaded((int)width, (int)height, kDiskFrameFormat);
    if (loaded.isNull())
        return false;

    for (quint32 y = 0; y < height; ++y) {
        const qint64 read = file.read(reinterpret_cast<char *>(loaded.scanLine((int)y)), row_bytes);
        if (read != (qint64)row_bytes)
            return false;
    }

    image = loaded;
    return true;
}

void DiskFrameCache::put(const CacheFrameKey &key, const QImage &image)
{
    if (image.isNull()) return;
    QDir().mkpath(cache_dir_);
    const QString path = pathForKey(key);
    const QString temp_path = path + QStringLiteral(".tmp");
    QFile::remove(temp_path);

    QFile file(temp_path);
    if (!file.open(QIODevice::WriteOnly)) {
        QFile::remove(temp_path);
        return;
    }

    const QImage frame = image.format() == kDiskFrameFormat
        ? image
        : image.convertToFormat(kDiskFrameFormat);
    if (frame.width() != key.width || frame.height() != key.height) {
        file.close();
        QFile::remove(temp_path);
        return;
    }
    const quint32 row_bytes = (quint32)frame.width() * 4;

    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_5_15);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << kRawFrameMagic
           << kRawFrameVersion
           << (quint16)kDiskFrameFormat
           << (quint32)frame.width()
           << (quint32)frame.height()
           << row_bytes;
    if (stream.status() != QDataStream::Ok) {
        file.close();
        QFile::remove(temp_path);
        return;
    }

    for (int y = 0; y < frame.height(); ++y) {
        if (file.write(reinterpret_cast<const char *>(frame.constScanLine(y)), row_bytes) != (qint64)row_bytes) {
            file.close();
            QFile::remove(temp_path);
            return;
        }
    }
    file.flush();
    file.close();

    QFile::remove(path);
    if (!QFile::rename(temp_path, path))
        QFile::remove(temp_path);
}

void DiskFrameCache::remove(const CacheFrameKey &key)
{
    QFile::remove(pathForKey(key));
}

void DiskFrameCache::clear()
{
    QDir dir(cache_dir_);
    if (!dir.exists()) return;
    for (const QFileInfo &file : dir.entryInfoList(QStringList() << QStringLiteral("*.ogsf") << QStringLiteral("*.tmp"), QDir::Files))
        QFile::remove(file.absoluteFilePath());
}

CacheStateTracker::CacheStateTracker(QObject *parent) : QObject(parent) {}

FrameCacheState CacheStateTracker::state(const CacheFrameKey &key) const
{
    QMutexLocker lock(&mutex_);
    return states_.value(key, FrameCacheState::NotCached);
}

FrameCacheState CacheStateTracker::stateForFrame(const QString &title_id, int frame) const
{
    QMutexLocker lock(&mutex_);
    FrameCacheState best = FrameCacheState::NotCached;
    for (auto it = states_.cbegin(); it != states_.cend(); ++it) {
        if (it.key().title_id == title_id && it.key().frame == frame) {
            const FrameCacheState state = it.value();
            if (state == FrameCacheState::Rendering) return state;
            if (state == FrameCacheState::Queued) best = state;
            else if (state == FrameCacheState::CachedRam) best = state;
            else if (best != FrameCacheState::CachedRam && state == FrameCacheState::CachedDisk) best = state;
            else if (best == FrameCacheState::NotCached && state == FrameCacheState::Stale) best = state;
        }
    }
    return best;
}

void CacheStateTracker::setState(const CacheFrameKey &key, FrameCacheState state)
{
    {
        QMutexLocker lock(&mutex_);
        auto it = states_.find(key);
        if (it != states_.end() && it.value() == state)
            return;
        states_[key] = state;
    }
    emit stateChanged(key.title_id, key.frame, key.frame);
}

void CacheStateTracker::markRange(const QString &title_id, int first_frame, int last_frame, FrameCacheState state)
{
    if (last_frame < first_frame) std::swap(first_frame, last_frame);
    {
        QMutexLocker lock(&mutex_);
        for (auto it = states_.begin(); it != states_.end(); ++it) {
            if (it.key().title_id == title_id && it.key().frame >= first_frame && it.key().frame <= last_frame)
                it.value() = state;
        }
    }
    emit stateChanged(title_id, first_frame, last_frame);
}

void CacheStateTracker::clearTitle(const QString &title_id)
{
    int first = std::numeric_limits<int>::max();
    int last = -1;
    {
        QMutexLocker lock(&mutex_);
        for (auto it = states_.begin(); it != states_.end();) {
            if (it.key().title_id == title_id) {
                first = std::min(first, it.key().frame);
                last = std::max(last, it.key().frame);
                it = states_.erase(it);
            } else {
                ++it;
            }
        }
    }
    if (last >= 0)
        emit stateChanged(title_id, first, last);
}

void CacheStateTracker::clear()
{
    {
        QMutexLocker lock(&mutex_);
        states_.clear();
    }
    emit stateChanged(QString(), 0, std::numeric_limits<int>::max());
}

QHash<int, FrameCacheState> CacheStateTracker::titleStates(const QString &title_id) const
{
    QMutexLocker lock(&mutex_);
    QHash<int, FrameCacheState> out;
    for (auto it = states_.cbegin(); it != states_.cend(); ++it) {
        if (it.key().title_id == title_id)
            out[it.key().frame] = it.value();
    }
    return out;
}

RenderQueueManager::RenderQueueManager(QObject *parent) : QObject(parent) {}

void RenderQueueManager::enqueue(const Job &job)
{
    if (!job.title) return;
    const QString key = job.key.toString();
    {
        QMutexLocker lock(&mutex_);
        if (!accepting_jobs_)
            return;
        if (queued_keys_.contains(key))
            return;
        jobs_.push_back(job);
        queued_keys_.insert(key);
        std::sort(jobs_.begin(), jobs_.end(), [](const Job &a, const Job &b) {
            if (a.priority != b.priority) return a.priority < b.priority;
            return a.key.frame < b.key.frame;
        });
    }
    emit queueChanged();
}

void RenderQueueManager::setAcceptingJobs(bool accepting)
{
    {
        QMutexLocker lock(&mutex_);
        accepting_jobs_ = accepting;
        if (!accepting) {
            jobs_.clear();
            queued_keys_.clear();
        }
    }
    emit queueChanged();
}

void RenderQueueManager::cancelTitle(const QString &title_id)
{
    {
        QMutexLocker lock(&mutex_);
        for (auto it = jobs_.begin(); it != jobs_.end();) {
            if (it->key.title_id == title_id) {
                queued_keys_.remove(it->key.toString());
                it = jobs_.erase(it);
            } else {
                ++it;
            }
        }
    }
    emit queueChanged();
}

void RenderQueueManager::cancelRange(const QString &title_id, int first_frame, int last_frame)
{
    if (last_frame < first_frame) std::swap(first_frame, last_frame);
    {
        QMutexLocker lock(&mutex_);
        for (auto it = jobs_.begin(); it != jobs_.end();) {
            if (it->key.title_id == title_id && it->key.frame >= first_frame && it->key.frame <= last_frame) {
                queued_keys_.remove(it->key.toString());
                it = jobs_.erase(it);
            } else {
                ++it;
            }
        }
    }
    emit queueChanged();
}

void RenderQueueManager::reprioritizeAround(const QString &title_id, int current_frame)
{
    {
        QMutexLocker lock(&mutex_);
        for (Job &job : jobs_) {
            if (job.key.title_id != title_id || job.live_cue) continue;
            const int delta = job.key.frame - current_frame;
            if (delta == 0) job.priority = 0;
            else if (delta > 0) job.priority = 100 + delta;
            else job.priority = 1000 + std::abs(delta);
        }
        std::sort(jobs_.begin(), jobs_.end(), [](const Job &a, const Job &b) {
            if (a.priority != b.priority) return a.priority < b.priority;
            return a.key.frame < b.key.frame;
        });
    }
    emit queueChanged();
}

bool RenderQueueManager::takeNext(Job &job)
{
    QMutexLocker lock(&mutex_);
    if (jobs_.empty())
        return false;
    job = jobs_.takeFirst();
    queued_keys_.remove(job.key.toString());
    emit queueChanged();
    return true;
}

bool RenderQueueManager::contains(const CacheFrameKey &key) const
{
    QMutexLocker lock(&mutex_);
    return queued_keys_.contains(key.toString());
}

int RenderQueueManager::queuedCount() const
{
    QMutexLocker lock(&mutex_);
    return jobs_.size();
}

void RenderQueueManager::clear()
{
    {
        QMutexLocker lock(&mutex_);
        jobs_.clear();
        queued_keys_.clear();
    }
    emit queueChanged();
}

CacheInvalidationManager::CacheInvalidationManager(QObject *parent) : QObject(parent) {}

int CacheInvalidationManager::frameForTime(double t) const
{
    return std::max(0, (int)std::round(t * cache_obs_frame_rate()));
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
      invalidation_(this)
{
    cache_enabled_ = TitlePreferences::cache_enabled();
    queue_.setAcceptingJobs(cache_enabled_);
    worker_timer_.setInterval(1);
    connect(&worker_timer_, &QTimer::timeout, this, &CacheManager::processNextJob);
    connect(&queue_, &RenderQueueManager::queueChanged, this, &CacheManager::queueChanged);
    connect(&state_tracker_, &CacheStateTracker::stateChanged, this, &CacheManager::cacheStatesChanged);
    connect(&invalidation_, &CacheInvalidationManager::rangeInvalidated, this,
            [this](const QString &title_id, int first, int last) {
                queue_.cancelRange(title_id, first, last);
                state_tracker_.markRange(title_id, first, last, FrameCacheState::Stale);
            });
    worker_timer_.start();
}

double CacheManager::effectiveFrameRate() const
{
    return cache_obs_frame_rate();
}

int CacheManager::frameForTime(double t) const
{
    return std::max(0, (int)std::round(t * effectiveFrameRate()));
}

double CacheManager::timeForFrame(int frame) const
{
    return (double)frame / std::max(1.0, effectiveFrameRate());
}

QString CacheManager::contentHash(const Title &title) const
{
    QCryptographicHash hash(QCryptographicHash::Sha1);
    auto add = [&](const auto &value) {
        QByteArray bytes;
        QDataStream stream(&bytes, QIODevice::WriteOnly);
        stream << value;
        hash.addData(bytes);
    };
    add(QString::fromStdString(title.id));
    add(QString::fromStdString(title.name));
    add(title.width);
    add(title.height);
    add(title.duration);
    add(title.loop_start);
    add(title.loop_end);
    add(title.playback_mode);
    add(title.loop_type);
    add((quint32)title.bg_color);
    add((quint64)title.layers.size());
    for (const auto &layer : title.layers) {
        if (!layer) continue;
        add(QString::fromStdString(layer->id));
        add(QString::fromStdString(layer->name));
        add((int)layer->type);
        add(layer->visible);
        add(layer->in_time);
        add(layer->out_time);
        add(QString::fromStdString(layer->text_content));
        add(QString::fromStdString(layer->image_path));
        add(layer->pos_x.static_value);
        add(layer->pos_y.static_value);
        add(layer->scale_x.static_value);
        add(layer->scale_y.static_value);
        add(layer->rotation.static_value);
        add(layer->opacity.static_value);
        add(layer->box_width.static_value);
        add(layer->box_height.static_value);
        add((quint32)layer->text_color);
        add((quint32)layer->fill_color);
        add((quint32)layer->stroke_color);
        add((int)layer->effects.size());
    }
    return QString::fromLatin1(hash.result().toHex());
}

CacheFrameKey CacheManager::keyForFrame(const Title &title, int frame) const
{
    const int clamped_frame = std::clamp(frame, 0, cache_last_frame_for_title(title, effectiveFrameRate()));
    return {QString::fromStdString(title.id), contentHash(title), clamped_frame, title.width, title.height};
}

CacheFrameKey CacheManager::keyForTime(const Title &title, double time) const
{
    return keyForFrame(title, frameForTime(std::clamp(time, 0.0, std::max(0.0, title.duration))));
}

QImage CacheManager::requestFrame(const std::shared_ptr<Title> &title, double time, bool cached_only)
{
    if (!title) return QImage();
    if (interactive_bypass_ || !cache_enabled_ || titleCacheability(title) == TitleCacheability::NonCacheable) {
        Q_UNUSED(cached_only);
        return render_title_to_image(*title, time);
    }
    const CacheFrameKey key = keyForTime(*title, time);
    const FrameCacheState existing_state = state_tracker_.state(key);
    QImage image;
    if (existing_state != FrameCacheState::Stale && ram_cache_.get(key, image)) {
        state_tracker_.setState(key, FrameCacheState::CachedRam);
        return image;
    }
    if (existing_state != FrameCacheState::Stale && disk_cache_.get(key, image)) {
        ram_cache_.put(key, image);
        state_tracker_.setState(key, FrameCacheState::CachedDisk);
        return image;
    }

    if (cached_only) {
        queueFrame(title, time, RenderQueueManager::PriorityBand::Visible);
        return QImage();
    }

    queue_.cancelRange(key.title_id, key.frame, key.frame);
    state_tracker_.setState(key, FrameCacheState::Rendering);
    image = render_title_to_image(*title, timeForFrame(key.frame));
    if (!image.isNull()) {
        ram_cache_.put(key, image);
        state_tracker_.setState(key, FrameCacheState::CachedRam);
    }
    return image;
}

void CacheManager::queueFrame(const std::shared_ptr<Title> &title, double time, RenderQueueManager::PriorityBand band)
{
    if (!title) return;
    if (interactive_bypass_ || !cache_enabled_ || titleCacheability(title) == TitleCacheability::NonCacheable)
        return;
    if (time < 0.0 || time > std::max(0.0, title->duration))
        return;
    const CacheFrameKey key = keyForTime(*title, time);
    QImage ignored;
    const FrameCacheState existing_state = state_tracker_.state(key);
    if (existing_state != FrameCacheState::Stale &&
        (ram_cache_.get(key, ignored) || disk_cache_.get(key, ignored)))
        return;
    if (existing_state == FrameCacheState::Rendering || queue_.contains(key))
        return;
    const int band_offset = (int)band * 100000;
    queue_.enqueue({key, std::make_shared<Title>(*title), timeForFrame(key.frame), band_offset + std::abs(key.frame), false, -1});
    state_tracker_.setState(key, FrameCacheState::Queued);
}

void CacheManager::queueRange(const std::shared_ptr<Title> &title, double start, double end, RenderQueueManager::PriorityBand band)
{
    if (!title) return;
    if (interactive_bypass_ || !cache_enabled_ || titleCacheability(title) == TitleCacheability::NonCacheable)
        return;
    if (end < start) std::swap(start, end);
    const int max_frame = cache_last_frame_for_title(*title, effectiveFrameRate());
    const int first = std::clamp(frameForTime(std::clamp(start, 0.0, title->duration)), 0, max_frame);
    const int last = std::clamp(frameForTime(std::clamp(end, 0.0, title->duration)), 0, max_frame);
    for (int frame = first; frame <= last; ++frame)
        queueFrame(title, timeForFrame(frame), band);
}

void CacheManager::queueWholeTimeline(const std::shared_ptr<Title> &title)
{
    if (!title) return;
    if (interactive_bypass_ || !cache_enabled_ || titleCacheability(title) == TitleCacheability::NonCacheable)
        return;
    const int max_frame = cache_last_frame_for_title(*title, effectiveFrameRate());
    const QString title_id = QString::fromStdString(title->id);
    const int start_frame = last_reprioritize_title_id_ == title_id
        ? std::clamp(last_reprioritize_frame_, 0, max_frame)
        : 0;
    for (int frame = start_frame; frame <= max_frame; ++frame)
        queueFrame(title, timeForFrame(frame), RenderQueueManager::PriorityBand::FullTimeline);
    for (int frame = 0; frame < start_frame; ++frame)
        queueFrame(title, timeForFrame(frame), RenderQueueManager::PriorityBand::FullTimeline);
}

void CacheManager::queueWorkArea(const std::shared_ptr<Title> &title)
{
    if (!title) return;
    queueRange(title, title->loop_start, title->loop_end, RenderQueueManager::PriorityBand::WorkArea);
}

void CacheManager::reprioritize(const std::shared_ptr<Title> &title, double current_time)
{
    if (!title) return;
    if (interactive_bypass_ || !cache_enabled_ || titleCacheability(title) == TitleCacheability::NonCacheable)
        return;
    const int max_frame = cache_last_frame_for_title(*title, effectiveFrameRate());
    const int current = std::clamp(frameForTime(current_time), 0, max_frame);
    const QString title_id = QString::fromStdString(title->id);
    if (last_reprioritize_title_id_ == title_id && last_reprioritize_frame_ == current)
        return;
    last_reprioritize_title_id_ = title_id;
    last_reprioritize_frame_ = current;
    queue_.reprioritizeAround(title_id, current);
    queueFrame(title, current_time, RenderQueueManager::PriorityBand::Visible);
    const int lookahead = std::min(6, std::max(2, (int)std::round(effectiveFrameRate() / 4.0)));
    const int lookbehind = 2;
    for (int i = 1; i <= lookahead; ++i)
        if (current + i <= max_frame)
            queueFrame(title, timeForFrame(current + i), RenderQueueManager::PriorityBand::AfterCurrent);
    for (int i = 1; i <= lookbehind; ++i) {
        if (current - i >= 0)
            queueFrame(title, timeForFrame(current - i), RenderQueueManager::PriorityBand::BeforeCurrent);
    }
}

void CacheManager::clearRam()
{
    ram_cache_.clear();
    state_tracker_.clear();
}

void CacheManager::clearDisk()
{
    disk_cache_.clear();
    state_tracker_.clear();
}

void CacheManager::clearAll()
{
    queue_.clear();
    ram_cache_.clear();
    disk_cache_.clear();
    state_tracker_.clear();
    live_cue_states_.clear();
}

void CacheManager::pausePrerender()
{
    paused_ = true;
}

void CacheManager::setCacheEnabled(bool enabled)
{
    if (cache_enabled_ == enabled)
        return;
    cache_enabled_ = enabled;
    TitlePreferences::set_cache_enabled(enabled);
    queue_.setAcceptingJobs(enabled && !interactive_bypass_);
    if (!enabled)
        paused_ = true;
    else
        paused_ = false;
    state_tracker_.clear();
    emit cacheEnabledChanged(enabled);
    emit diagnosticsChanged();
}

void CacheManager::setInteractiveBypass(bool bypass)
{
    if (interactive_bypass_ == bypass)
        return;
    interactive_bypass_ = bypass;
    queue_.setAcceptingJobs(cache_enabled_ && !interactive_bypass_);
}

TitleCacheability CacheManager::titleCacheability(const std::shared_ptr<Title> &title) const
{
    if (!title)
        return TitleCacheability::NonCacheable;
    for (const auto &layer : title->layers) {
        if (!layer) continue;
        if (layer->type == LayerType::Clock || layer->type == LayerType::Ticker)
            return TitleCacheability::NonCacheable;
    }
    return TitleCacheability::Cacheable;
}

QString CacheManager::titleCacheabilityMessage(const std::shared_ptr<Title> &title) const
{
    if (!cache_enabled_)
        return QStringLiteral("Caching is disabled.");
    if (titleCacheability(title) == TitleCacheability::NonCacheable)
        return QStringLiteral("Prerender unavailable for dynamic real-time content.");
    return QString();
}

void CacheManager::resumePrerender()
{
    paused_ = false;
    if (!worker_timer_.isActive())
        worker_timer_.start();
}

void CacheManager::invalidateAll(const std::shared_ptr<Title> &title)
{
    invalidation_.invalidateAll(title);
    invalidateLiveCues(title);
}

void CacheManager::invalidateRange(const std::shared_ptr<Title> &title, double start, double end)
{
    invalidation_.invalidateRange(title, start, end);
}

void CacheManager::invalidateLayer(const std::shared_ptr<Title> &title, const std::string &layer_id)
{
    invalidation_.invalidateLayer(title, layer_id);
}

std::shared_ptr<Title> CacheManager::titleWithCueApplied(const std::shared_ptr<Title> &title, int row) const
{
    if (!title || row < 0 || row >= (int)title->live_text_rows.size())
        return nullptr;
    auto cue_title = std::make_shared<Title>(*title);
    cue_title->layers.clear();
    cue_title->layers.reserve(title->layers.size());
    for (const auto &layer : title->layers)
        if (layer) cue_title->layers.push_back(std::make_shared<Layer>(*layer));

    std::vector<std::shared_ptr<Layer>> exposed;
    for (const auto &layer : cue_title->layers) {
        if (!layer) continue;
        if ((layer->type == LayerType::Text || layer->type == LayerType::Ticker) && layer->expose_text)
            exposed.push_back(layer);
    }
    if (!cue_title->live_text_column_order.empty()) {
        std::vector<std::shared_ptr<Layer>> ordered;
        ordered.reserve(exposed.size());
        for (const auto &layer_id : cue_title->live_text_column_order) {
            auto it = std::find_if(exposed.begin(), exposed.end(),
                                   [&](const std::shared_ptr<Layer> &layer) {
                                       return layer && layer->id == layer_id;
                                   });
            if (it != exposed.end())
                ordered.push_back(*it);
        }
        for (const auto &layer : exposed) {
            auto it = std::find_if(ordered.begin(), ordered.end(),
                                   [&](const std::shared_ptr<Layer> &candidate) {
                                       return candidate && layer && candidate->id == layer->id;
                                   });
            if (it == ordered.end())
                ordered.push_back(layer);
        }
        exposed = std::move(ordered);
    }
    for (int col = 0; col < (int)exposed.size() && col < (int)cue_title->live_text_rows[row].size(); ++col)
        exposed[col]->text_content = cue_title->live_text_rows[row][col];
    cue_title->current_cue_row = row;
    return cue_title;
}

CacheFrameKey CacheManager::liveCueKey(const std::shared_ptr<Title> &title, int row) const
{
    auto cue_title = titleWithCueApplied(title, row);
    if (!cue_title) return {};
    CacheFrameKey key = keyForTime(*cue_title, std::clamp(cue_title->pause_time, 0.0, cue_title->duration));
    key.content_hash += QStringLiteral("-cue-%1").arg(row);
    return key;
}

void CacheManager::queueLiveCue(const std::shared_ptr<Title> &title, int row)
{
    if (!cache_enabled_)
        return;
    auto cue_title = titleWithCueApplied(title, row);
    if (!cue_title) return;
    CacheFrameKey key = liveCueKey(title, row);
    const QString cue_key = key.toString();
    QImage ignored;
    const FrameCacheState existing_state = live_cue_states_.value(cue_key, FrameCacheState::NotCached);
    if (existing_state != FrameCacheState::Stale &&
        (ram_cache_.get(key, ignored) || disk_cache_.get(key, ignored))) {
        const FrameCacheState ready_state = ram_cache_.get(key, ignored)
            ? FrameCacheState::CachedRam
            : FrameCacheState::CachedDisk;
        if (existing_state != ready_state) {
            live_cue_states_[cue_key] = ready_state;
            emit liveCueStateChanged(key.title_id, row);
        }
        return;
    }
    if (existing_state == FrameCacheState::Rendering || queue_.contains(key))
        return;
    ++live_cue_stats_.misses;
    emit diagnosticsChanged();
    if (live_cue_states_.value(cue_key, FrameCacheState::NotCached) != FrameCacheState::Queued) {
        live_cue_states_[cue_key] = FrameCacheState::Queued;
        emit liveCueStateChanged(key.title_id, row);
    }
    queue_.enqueue({key, cue_title, std::clamp(cue_title->pause_time, 0.0, cue_title->duration), -1000000 + row, true, row});
}

void CacheManager::preloadLiveCues(const std::shared_ptr<Title> &title, int current_row, int nearby_count)
{
    if (!cache_enabled_) return;
    if (!title || title->live_text_rows.empty()) return;
    const int count = (int)title->live_text_rows.size();
    const int current = std::clamp(current_row, 0, count - 1);
    queueLiveCue(title, current);
    for (int i = 1; i <= std::max(1, nearby_count); ++i) {
        if (current - i >= 0) queueLiveCue(title, current - i);
        if (current + i < count) queueLiveCue(title, current + i);
    }
}

FrameCacheState CacheManager::liveCueState(const std::shared_ptr<Title> &title, int row) const
{
    if (!cache_enabled_)
        return FrameCacheState::Disabled;
    if (!title || row < 0 || row >= (int)title->live_text_rows.size())
        return FrameCacheState::NotCached;
    const CacheFrameKey key = liveCueKey(title, row);
    const FrameCacheState tracked_state = live_cue_states_.value(key.toString(), FrameCacheState::NotCached);
    if (tracked_state == FrameCacheState::Stale || tracked_state == FrameCacheState::Queued ||
        tracked_state == FrameCacheState::Rendering)
        return tracked_state;
    QImage ignored;
    if (ram_cache_.get(key, ignored))
        return FrameCacheState::CachedRam;
    if (disk_cache_.get(key, ignored))
        return FrameCacheState::CachedDisk;
    return tracked_state;
}

bool CacheManager::isLiveCueReady(const std::shared_ptr<Title> &title, int row)
{
    const FrameCacheState state = liveCueState(title, row);
    const bool ready = state == FrameCacheState::CachedRam || state == FrameCacheState::CachedDisk;
    if (ready) {
        ++live_cue_stats_.hits;
        ++live_cue_stats_.reuses;
        emit diagnosticsChanged();
    }
    return ready;
}

void CacheManager::invalidateLiveCue(const std::shared_ptr<Title> &title, int row)
{
    if (!title) return;
    const CacheFrameKey key = liveCueKey(title, row);
    ++live_cue_stats_.invalidations;
    live_cue_states_[key.toString()] = FrameCacheState::Stale;
    emit liveCueStateChanged(QString::fromStdString(title->id), row);
    emit diagnosticsChanged();
    queueLiveCue(title, row);
}

void CacheManager::invalidateLiveCues(const std::shared_ptr<Title> &title)
{
    if (!title) return;
    for (int row = 0; row < (int)title->live_text_rows.size(); ++row) {
        const CacheFrameKey key = liveCueKey(title, row);
        ++live_cue_stats_.invalidations;
        live_cue_states_[key.toString()] = FrameCacheState::Stale;
        emit liveCueStateChanged(QString::fromStdString(title->id), row);
    }
    emit diagnosticsChanged();
}

void CacheManager::setPlaybackSettings(const CachePlaybackSettings &settings)
{
    playback_settings_ = settings;
}

void CacheManager::processNextJob()
{
    if (paused_ || interactive_bypass_ || !cache_enabled_) return;
    RenderQueueManager::Job job;
    if (!queue_.takeNext(job))
        return;

    const bool was_stale = job.live_cue
        ? live_cue_states_.value(job.key.toString(), FrameCacheState::NotCached) == FrameCacheState::Stale
        : state_tracker_.state(job.key) == FrameCacheState::Stale;

    if (job.live_cue) {
        if (live_cue_states_.value(job.key.toString(), FrameCacheState::NotCached) != FrameCacheState::Rendering) {
            live_cue_states_[job.key.toString()] = FrameCacheState::Rendering;
            emit liveCueStateChanged(job.key.title_id, job.cue_row);
        }
    } else {
        state_tracker_.setState(job.key, FrameCacheState::Rendering);
    }

    QImage image;
    if (was_stale || (!ram_cache_.get(job.key, image) && !disk_cache_.get(job.key, image)))
        image = render_title_to_image(*job.title, job.time);
    if (!image.isNull()) {
        ram_cache_.put(job.key, image);
        disk_cache_.put(job.key, image);
        if (job.live_cue) {
            if (live_cue_states_.value(job.key.toString(), FrameCacheState::NotCached) != FrameCacheState::CachedRam) {
                live_cue_states_[job.key.toString()] = FrameCacheState::CachedRam;
                emit liveCueStateChanged(job.key.title_id, job.cue_row);
            }
        } else {
            state_tracker_.setState(job.key, FrameCacheState::CachedRam);
            emit frameReady(job.key.title_id, job.key.frame);
        }
    }
}
