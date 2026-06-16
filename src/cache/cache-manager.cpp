#include "cache-manager.h"
#include "title-source.h"
#include "title-preferences.h"
#include "title-logger.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QBuffer>
#include <QDataStream>
#include <QMutexLocker>
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>

#include <algorithm>
#include <cmath>
#include <cstring>
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
constexpr quint16 kRawFrameVersion = 2;
constexpr quint16 kRawFrameLegacyVersion = 1;
constexpr quint16 kFrameCodecRaw = 0;
constexpr quint16 kFrameCodecLz4 = 1;
constexpr QImage::Format kDiskFrameFormat = QImage::Format_ARGB32_Premultiplied;
constexpr int kCacheTileSize = 256;

static quint64 cache_image_bytes(const QImage &image)
{
    return image.isNull() ? 0 : (quint64)image.bytesPerLine() * (quint64)image.height();
}
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
    auto *self = const_cast<RamFrameCache *>(this);
    self->lru_.removeAll(key);
    self->lru_.push_back(key);
    image = it.value();
    return !image.isNull();
}

void RamFrameCache::put(const CacheFrameKey &key, const QImage &image)
{
    if (image.isNull()) return;
    QMutexLocker lock(&mutex_);
    const quint64 bytes = imageBytes(image);
    if (frame_bytes_.contains(key))
        bytes_used_ -= frame_bytes_.value(key);
    frames_.insert(key, image);
    frame_bytes_.insert(key, bytes);
    bytes_used_ += bytes;
    lru_.removeAll(key);
    lru_.push_back(key);
    evictIfNeeded();
}

void RamFrameCache::remove(const CacheFrameKey &key)
{
    QMutexLocker lock(&mutex_);
    bytes_used_ -= frame_bytes_.value(key, 0);
    frame_bytes_.remove(key);
    lru_.removeAll(key);
    frames_.remove(key);
}

void RamFrameCache::clear()
{
    QMutexLocker lock(&mutex_);
    frames_.clear();
    frame_bytes_.clear();
    lru_.clear();
    bytes_used_ = 0;
}

void RamFrameCache::setMaxBytes(quint64 bytes)
{
    QMutexLocker lock(&mutex_);
    max_bytes_ = std::max<quint64>(64ull * 1024ull * 1024ull, bytes);
    evictIfNeeded();
}

quint64 RamFrameCache::maxBytes() const
{
    QMutexLocker lock(&mutex_);
    return max_bytes_;
}

quint64 RamFrameCache::bytesUsed() const
{
    QMutexLocker lock(&mutex_);
    return bytes_used_;
}

qsizetype RamFrameCache::count() const
{
    QMutexLocker lock(&mutex_);
    return frames_.size();
}

quint64 RamFrameCache::imageBytes(const QImage &image) const
{
    return cache_image_bytes(image);
}

void RamFrameCache::evictIfNeeded()
{
    while (bytes_used_ > max_bytes_ && !lru_.isEmpty()) {
        const CacheFrameKey victim = lru_.takeFirst();
        auto it = frames_.find(victim);
        if (it == frames_.end())
            continue;
        bytes_used_ -= frame_bytes_.value(victim, 0);
        frame_bytes_.remove(victim);
        frames_.erase(it);
    }
}

DiskFrameCache::DiskFrameCache(QObject *parent) : QObject(parent)
{
    cache_dir_ = TitlePreferences::cache_disk_location();
    QDir().mkpath(cache_dir_);
    bytes_used_ = scanBytesUsed();
}

QString DiskFrameCache::pathForKey(const CacheFrameKey &key) const
{
    const QByteArray digest = QCryptographicHash::hash(key.toString().toUtf8(), QCryptographicHash::Sha1).toHex();
    return QDir(cache_dir_).filePath(QString::fromLatin1(digest) + QStringLiteral(".ogsf"));
}

bool DiskFrameCache::contains(const CacheFrameKey &key) const
{
    return QFileInfo::exists(pathForKey(key));
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
    quint16 codec = 0;
    quint16 reserved = 0;
    quint32 width = 0;
    quint32 height = 0;
    quint32 row_bytes = 0;
    stream >> magic >> version >> format >> width >> height >> row_bytes;
    if (stream.status() != QDataStream::Ok ||
        magic != kRawFrameMagic ||
        (version != kRawFrameVersion && version != kRawFrameLegacyVersion) ||
        format != (quint16)kDiskFrameFormat ||
        width == 0 || height == 0 ||
        width != (quint32)key.width ||
        height != (quint32)key.height ||
        row_bytes != width * 4)
        return false;

    quint32 raw_size = row_bytes * height;
    quint32 payload_size = raw_size;
    if (version == kRawFrameVersion) {
        stream >> codec >> reserved >> raw_size >> payload_size;
        if (stream.status() != QDataStream::Ok || raw_size != row_bytes * height ||
            (codec != kFrameCodecRaw && codec != kFrameCodecLz4))
            return false;
        if (codec == kFrameCodecLz4)
            return false;
    }

    QImage loaded((int)width, (int)height, kDiskFrameFormat);
    if (loaded.isNull())
        return false;

    if (version == kRawFrameLegacyVersion) {
        for (quint32 y = 0; y < height; ++y) {
            const qint64 read = file.read(reinterpret_cast<char *>(loaded.scanLine((int)y)), row_bytes);
            if (read != (qint64)row_bytes)
                return false;
        }
    } else {
        const QByteArray payload = file.read(payload_size);
        if (payload.size() != (int)payload_size)
            return false;
        const QByteArray raw = payload;
        if (raw.size() != (int)raw_size)
            return false;
        for (quint32 y = 0; y < height; ++y)
            memcpy(loaded.scanLine((int)y), raw.constData() + (int)y * (int)row_bytes, row_bytes);
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
    QByteArray raw;
    raw.resize((int)row_bytes * frame.height());
    for (int y = 0; y < frame.height(); ++y)
        memcpy(raw.data() + y * (int)row_bytes, frame.constScanLine(y), row_bytes);
    const QByteArray payload = raw;
    const quint16 codec = kFrameCodecRaw;

    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_5_15);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << kRawFrameMagic
           << kRawFrameVersion
           << (quint16)kDiskFrameFormat
           << (quint32)frame.width()
           << (quint32)frame.height()
           << row_bytes
           << codec
           << (quint16)0
           << (quint32)raw.size()
           << (quint32)payload.size();
    if (stream.status() != QDataStream::Ok) {
        file.close();
        QFile::remove(temp_path);
        return;
    }

    if (file.write(payload) != payload.size()) {
        file.close();
        QFile::remove(temp_path);
        return;
    }
    file.close();

    const quint64 previous_size = QFileInfo(path).exists() ? (quint64)QFileInfo(path).size() : 0;
    QFile::remove(path);
    if (QFile::rename(temp_path, path)) {
        const quint64 new_size = (quint64)QFileInfo(path).size();
        bytes_used_ = bytes_used_ - std::min(bytes_used_, previous_size) + new_size;
        appendManifestEntry(key);
    } else {
        QFile::remove(temp_path);
    }
}

QVector<CacheFrameKey> DiskFrameCache::keysForTitle(const QString &title_id) const
{
    QVector<CacheFrameKey> keys;
    QFile manifest(manifestPath());
    if (!manifest.open(QIODevice::ReadOnly))
        return keys;
    QSet<QString> seen;
    while (!manifest.atEnd()) {
        const QJsonDocument doc = QJsonDocument::fromJson(manifest.readLine().trimmed());
        if (!doc.isObject())
            continue;
        const QJsonObject obj = doc.object();
        CacheFrameKey key;
        key.title_id = obj.value(QStringLiteral("title_id")).toString();
        if (key.title_id != title_id)
            continue;
        key.content_hash = obj.value(QStringLiteral("content_hash")).toString();
        key.frame = obj.value(QStringLiteral("frame")).toInt();
        key.width = obj.value(QStringLiteral("width")).toInt();
        key.height = obj.value(QStringLiteral("height")).toInt();
        if (key.content_hash.isEmpty() || key.width <= 0 || key.height <= 0)
            continue;
        if (!QFileInfo::exists(pathForKey(key)))
            continue;
        const QString id = key.toString();
        if (seen.contains(id))
            continue;
        seen.insert(id);
        keys.push_back(key);
    }
    return keys;
}

void DiskFrameCache::remove(const CacheFrameKey &key)
{
    const QString path = pathForKey(key);
    const quint64 previous_size = QFileInfo(path).exists() ? (quint64)QFileInfo(path).size() : 0;
    if (QFile::remove(path))
        bytes_used_ -= std::min(bytes_used_, previous_size);
}

void DiskFrameCache::clear()
{
    QDir dir(cache_dir_);
    if (!dir.exists()) return;
    for (const QFileInfo &file : dir.entryInfoList(QStringList() << QStringLiteral("*.ogsf") << QStringLiteral("*.tmp"), QDir::Files))
        QFile::remove(file.absoluteFilePath());
    QFile::remove(manifestPath());
    bytes_used_ = 0;
}

void DiskFrameCache::setCacheDirectory(const QString &path)
{
    const QString clean = QDir::cleanPath(path);
    if (clean.isEmpty() || clean == cache_dir_)
        return;
    cache_dir_ = clean;
    QDir().mkpath(cache_dir_);
    bytes_used_ = scanBytesUsed();
}

quint64 DiskFrameCache::bytesUsed() const
{
    return bytes_used_;
}

quint64 DiskFrameCache::scanBytesUsed() const
{
    quint64 total = 0;
    QDir dir(cache_dir_);
    if (!dir.exists())
        return total;
    for (const QFileInfo &file : dir.entryInfoList(QStringList() << QStringLiteral("*.ogsf"), QDir::Files))
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

bool RenderQueueManager::enqueue(const Job &job)
{
    if (!job.title) return false;
    const QString key = job.key.toString();
    {
        QMutexLocker lock(&mutex_);
        if (!accepting_jobs_)
            return false;
        if (queued_keys_.contains(key))
            return false;
        jobs_.push_back(job);
        queued_keys_.insert(key);
        std::sort(jobs_.begin(), jobs_.end(), [](const Job &a, const Job &b) {
            if (a.priority != b.priority) return a.priority < b.priority;
            return a.key.frame < b.key.frame;
        });
    }
    emit queueChanged();
    return true;
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

bool RenderQueueManager::cancelKey(const CacheFrameKey &key)
{
    bool removed = false;
    {
        QMutexLocker lock(&mutex_);
        const QString key_string = key.toString();
        for (auto it = jobs_.begin(); it != jobs_.end();) {
            if (it->key == key) {
                queued_keys_.remove(key_string);
                it = jobs_.erase(it);
                removed = true;
            } else {
                ++it;
            }
        }
    }
    if (removed)
        emit queueChanged();
    return removed;
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
    {
        QMutexLocker lock(&mutex_);
        if (jobs_.empty())
            return false;
        job = jobs_.takeFirst();
        queued_keys_.remove(job.key.toString());
    }
    emit queueChanged();
    return true;
}

bool RenderQueueManager::takeNextLiveCue(Job &job)
{
    bool found = false;
    {
        QMutexLocker lock(&mutex_);
        for (auto it = jobs_.begin(); it != jobs_.end(); ++it) {
            if (!it->live_cue)
                continue;
            job = *it;
            queued_keys_.remove(it->key.toString());
            jobs_.erase(it);
            found = true;
            break;
        }
    }
    if (found)
        emit queueChanged();
    return found;
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
    ram_cache_.setMaxBytes((quint64)TitlePreferences::cache_ram_limit_mb() * 1024ull * 1024ull);
    QThread *app_thread = QCoreApplication::instance() ? QCoreApplication::instance()->thread() : nullptr;
    if (app_thread && thread() != app_thread) {
        worker_timer_.moveToThread(app_thread);
        moveToThread(app_thread);
    }
    ensureWorkerTimerActive();
    OGS_LOG_INFO("Cache", QStringLiteral("CacheManager initialized enabled=%1 ramLimitMb=%2 disk=%3")
                              .arg(cache_enabled_)
                              .arg(TitlePreferences::cache_ram_limit_mb())
                              .arg(disk_cache_.cacheDirectory()));
}

void CacheManager::ensureWorkerTimerActive()
{
    if (QThread::currentThread() == worker_timer_.thread()) {
        if (!worker_timer_.isActive())
            worker_timer_.start();
    } else {
        QMetaObject::invokeMethod(&worker_timer_, "start", Qt::QueuedConnection);
    }
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
        add(layer->position.static_value.x);
        add(layer->position.static_value.y);
        add(layer->scale.static_value.x);
        add(layer->scale.static_value.y);
        add(layer->rotation.static_value);
        add(layer->opacity.static_value);
        add(layer->size.static_value.x);
        add(layer->size.static_value.y);
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

QImage CacheManager::renderUncachedFrame(const std::shared_ptr<Title> &title, double time) const
{
    if (!title)
        return QImage();
    return render_title_to_image(*title, std::clamp(time, 0.0, std::max(0.0, title->duration)));
}

QImage CacheManager::requestFrame(const std::shared_ptr<Title> &title, double time, bool cached_only)
{
    if (!title) return QImage();
    if (interactive_bypass_ || !cache_enabled_ || titleCacheability(title) == TitleCacheability::NonCacheable) {
        return cached_only ? QImage() : renderUncachedFrame(title, time);
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

void CacheManager::restoreDiskStates(const std::shared_ptr<Title> &title)
{
    if (!title || !cache_enabled_)
        return;
    const QString title_id = QString::fromStdString(title->id);
    const QString current_hash = contentHash(*title);
    const auto keys = disk_cache_.keysForTitle(title_id);
    for (const CacheFrameKey &key : keys) {
        if (key.content_hash == current_hash && key.width == title->width && key.height == title->height)
            state_tracker_.setState(key, FrameCacheState::CachedDisk);
    }
    for (int row = 0; row < (int)title->live_text_rows.size(); ++row) {
        const CacheFrameKey key = liveCueKey(title, row);
        const QString state_key = liveCueStateKey(key.title_id, row);
        if (disk_cache_.contains(key))
            live_cue_states_[state_key] = FrameCacheState::CachedDisk;
    }
    emit diagnosticsChanged();
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
        (ram_cache_.get(key, ignored) || disk_cache_.contains(key)))
        return;
    if (existing_state == FrameCacheState::Rendering || queue_.contains(key))
        return;
    const int band_offset = (int)band * 100000;
    if (queue_.enqueue({key, std::make_shared<Title>(*title), timeForFrame(key.frame), band_offset + std::abs(key.frame), false, -1})) {
        OGS_LOG_TRACE("Cache", QStringLiteral("Queued frame title=%1 frame=%2 time=%3 band=%4 key=%5")
                                   .arg(key.title_id)
                                   .arg(key.frame)
                                   .arg(timeForFrame(key.frame), 0, 'f', 3)
                                   .arg((int)band)
                                   .arg(key.toString()));
        state_tracker_.setState(key, FrameCacheState::Queued);
        ensureWorkerTimerActive();
    }
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
    live_cue_progress_percent_.clear();
    live_cue_rows_.clear();
    live_cue_title_ids_.clear();
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
    OGS_LOG_INFO("Cache", QStringLiteral("Cache enabled changed to %1").arg(enabled));
    TitlePreferences::set_cache_enabled(enabled);
    queue_.setAcceptingJobs(enabled);
    if (!enabled)
        paused_ = true;
    else {
        paused_ = false;
        ensureWorkerTimerActive();
    }
    state_tracker_.clear();
    emit cacheEnabledChanged(enabled);
    emit diagnosticsChanged();
}

void CacheManager::setInteractiveBypass(bool bypass)
{
    if (interactive_bypass_ == bypass)
        return;
    interactive_bypass_ = bypass;
    OGS_LOG_DEBUG("Cache", QStringLiteral("Interactive bypass changed to %1").arg(bypass));
    queue_.setAcceptingJobs(cache_enabled_);
}

void CacheManager::setRamCacheLimitMb(int megabytes)
{
    const int clamped = std::clamp(megabytes, 64, 32768);
    TitlePreferences::set_cache_ram_limit_mb(clamped);
    ram_cache_.setMaxBytes((quint64)clamped * 1024ull * 1024ull);
    emit diagnosticsChanged();
}

void CacheManager::setDiskCacheLocation(const QString &path)
{
    const QString clean = QDir::cleanPath(path);
    if (clean.isEmpty())
        return;
    TitlePreferences::set_cache_disk_location(clean);
    disk_cache_.setCacheDirectory(clean);
    OGS_LOG_INFO("Cache", QStringLiteral("Disk cache location changed to %1").arg(clean));
    state_tracker_.clear();
    emit diagnosticsChanged();
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

QVector<CacheTileRegion> CacheManager::tilesForRect(const QRect &rect, const QSize &frame_size) const
{
    QVector<CacheTileRegion> tiles;
    if (rect.isEmpty() || frame_size.isEmpty())
        return tiles;
    const QRect frame_rect(QPoint(0, 0), frame_size);
    const QRect clipped = rect.intersected(frame_rect);
    if (clipped.isEmpty())
        return tiles;
    const int first_x = clipped.left() / kCacheTileSize;
    const int last_x = clipped.right() / kCacheTileSize;
    const int first_y = clipped.top() / kCacheTileSize;
    const int last_y = clipped.bottom() / kCacheTileSize;
    for (int ty = first_y; ty <= last_y; ++ty) {
        for (int tx = first_x; tx <= last_x; ++tx) {
            const QRect tile_rect(tx * kCacheTileSize, ty * kCacheTileSize, kCacheTileSize, kCacheTileSize);
            tiles.push_back({tx, ty, tile_rect.intersected(frame_rect)});
        }
    }
    return tiles;
}

void CacheManager::resumePrerender()
{
    paused_ = false;
    ensureWorkerTimerActive();
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
    for (int col = 0; col < (int)exposed.size() && col < (int)cue_title->live_text_rows[row].size(); ++col) {
        exposed[col]->text_content = cue_title->live_text_rows[row][col];
        exposed[col]->rich_text = rich_text_document_from_layer_defaults(*exposed[col]);
        exposed[col]->rich_text_html.clear();
    }
    cue_title->current_cue_row = row;
    return cue_title;
}

CacheFrameKey CacheManager::liveCueKey(const std::shared_ptr<Title> &title, int row) const
{
    auto cue_title = titleWithCueApplied(title, row);
    if (!cue_title) return {};
    return keyForTime(*cue_title, std::clamp(cue_title->pause_time, 0.0, cue_title->duration));
}

QString CacheManager::liveCueStateKey(const QString &title_id, int row) const
{
    return QStringLiteral("%1:live-cue:%2").arg(title_id).arg(row);
}

int CacheManager::liveCueRangeProgress(const std::shared_ptr<Title> &cue_title) const
{
    if (!cue_title)
        return 0;
    const int max_frame = cache_last_frame_for_title(*cue_title, effectiveFrameRate());
    const int total = std::max(1, max_frame + 1);
    int cached = 0;
    for (int frame = 0; frame <= max_frame; ++frame) {
        const CacheFrameKey frame_key = keyForFrame(*cue_title, frame);
        QImage ignored;
        if (ram_cache_.get(frame_key, ignored) || disk_cache_.contains(frame_key))
            ++cached;
    }
    return std::clamp((cached * 100) / total, 0, 100);
}

FrameCacheState CacheManager::liveCueRangeState(const std::shared_ptr<Title> &cue_title, const QString &state_key) const
{
    if (!cue_title)
        return FrameCacheState::NotCached;

    const FrameCacheState explicit_state = live_cue_states_.value(state_key, FrameCacheState::NotCached);
    const int max_frame = cache_last_frame_for_title(*cue_title, effectiveFrameRate());
    const int total = std::max(1, max_frame + 1);
    int cached = 0;
    bool any_disk = false;
    bool any_rendering = false;
    bool any_queued = false;
    bool any_stale = explicit_state == FrameCacheState::Stale;

    for (int frame = 0; frame <= max_frame; ++frame) {
        const CacheFrameKey frame_key = keyForFrame(*cue_title, frame);
        const FrameCacheState frame_state = state_tracker_.state(frame_key);
        if (frame_state == FrameCacheState::Rendering)
            any_rendering = true;
        if (frame_state == FrameCacheState::Queued || queue_.contains(frame_key))
            any_queued = true;
        if (frame_state == FrameCacheState::Stale)
            any_stale = true;

        QImage ignored;
        if (ram_cache_.get(frame_key, ignored)) {
            ++cached;
        } else if (disk_cache_.contains(frame_key)) {
            ++cached;
            any_disk = true;
        }
    }

    if (explicit_state == FrameCacheState::Rendering || any_rendering)
        return FrameCacheState::Rendering;
    if (explicit_state == FrameCacheState::Queued || any_queued)
        return FrameCacheState::Queued;
    if (cached >= total)
        return any_disk ? FrameCacheState::CachedDisk : FrameCacheState::CachedRam;
    if (cached > 0)
        return FrameCacheState::Queued;
    if (any_stale)
        return FrameCacheState::Stale;
    return FrameCacheState::NotCached;
}

void CacheManager::queueLiveCue(const std::shared_ptr<Title> &title, int row, bool urgent)
{
    if (!cache_enabled_) {
        OGS_LOG_DEBUG("LiveCue", QStringLiteral("Skipped queueLiveCue row=%1 because cache is disabled").arg(row));
        return;
    }
    auto cue_title = titleWithCueApplied(title, row);
    if (!cue_title) {
        OGS_LOG_WARNING("LiveCue", QStringLiteral("Skipped queueLiveCue row=%1 because cue title could not be created").arg(row));
        return;
    }
    CacheFrameKey key = liveCueKey(title, row);
    const QString state_key = liveCueStateKey(key.title_id, row);
    live_cue_rows_[state_key] = row;
    live_cue_title_ids_[state_key] = key.title_id;
    const FrameCacheState existing_state = liveCueRangeState(cue_title, state_key);
    const bool busy = existing_state == FrameCacheState::Queued || existing_state == FrameCacheState::Rendering;
    OGS_LOG_DEBUG("LiveCue", QStringLiteral("queueLiveCue title=%1 row=%2 urgent=%3 state=%4 busy=%5 frameKey=%6 stateKey=%7")
                                .arg(key.title_id)
                                .arg(row)
                                .arg(urgent)
                                .arg((int)existing_state)
                                .arg(busy)
                                .arg(key.toString())
                                .arg(state_key));
    if (!urgent && (existing_state == FrameCacheState::CachedRam || existing_state == FrameCacheState::CachedDisk)) {
        live_cue_states_[state_key] = existing_state;
        live_cue_progress_percent_[state_key] = 100;
        OGS_LOG_DEBUG("LiveCue", QStringLiteral("Live cue row already fully prerendered title=%1 row=%2 readyState=%3")
                                    .arg(key.title_id).arg(row).arg((int)existing_state));
        emit liveCueStateChanged(key.title_id, row);
        return;
    }
    if (existing_state == FrameCacheState::Rendering)
        return;
    const int max_frame = cache_last_frame_for_title(*cue_title, effectiveFrameRate());
    int queued = 0;
    for (int frame = 0; frame <= max_frame; ++frame) {
        const CacheFrameKey frame_key = keyForFrame(*cue_title, frame);
        QImage ignored;
        if (!urgent && (ram_cache_.get(frame_key, ignored) || disk_cache_.contains(frame_key)))
            continue;
        if (urgent)
            queue_.cancelKey(frame_key);
        else if (queue_.contains(frame_key))
            continue;
        const int priority = urgent
            ? (std::numeric_limits<int>::min() / 2) + frame
            : -1000000 + row * 10000 + frame;
        if (queue_.enqueue({frame_key, cue_title, timeForFrame(frame), priority, true, row}))
            ++queued;
    }
    if (queued > 0) {
        ++live_cue_stats_.misses;
        emit diagnosticsChanged();
        live_cue_states_[state_key] = FrameCacheState::Queued;
        live_cue_progress_percent_[state_key] = liveCueRangeProgress(cue_title);
        OGS_LOG_INFO("LiveCue", QStringLiteral("Queued live cue row range title=%1 row=%2 urgent=%3 queuedFrames=%4 progress=%5 stateKey=%6 pauseKey=%7")
                                  .arg(key.title_id).arg(row).arg(urgent).arg(queued)
                                  .arg(live_cue_progress_percent_.value(state_key))
                                  .arg(state_key, key.toString()));
        emit liveCueStateChanged(key.title_id, row);
        ensureWorkerTimerActive();
    }
}

void CacheManager::cacheLiveCueNow(const std::shared_ptr<Title> &title, int row)
{
    if (!title) return;
    const CacheFrameKey key = liveCueKey(title, row);
    const QString state_key = liveCueStateKey(key.title_id, row);
    auto cue_title = titleWithCueApplied(title, row);
    if (cue_title) {
        const int max_frame = cache_last_frame_for_title(*cue_title, effectiveFrameRate());
        for (int frame = 0; frame <= max_frame; ++frame) {
            const CacheFrameKey frame_key = keyForFrame(*cue_title, frame);
            queue_.cancelKey(frame_key);
            ram_cache_.remove(frame_key);
            disk_cache_.remove(frame_key);
        }
    }
    OGS_LOG_INFO("LiveCue", QStringLiteral("Manual live cue cache rebuild title=%1 row=%2 stateKey=%3 frameKey=%4")
                              .arg(key.title_id).arg(row).arg(state_key, key.toString()));
    live_cue_states_[state_key] = FrameCacheState::NotCached;
    live_cue_progress_percent_[state_key] = 0;
    queueLiveCue(title, row, true);
}

QImage CacheManager::requestLiveCueFrame(const std::shared_ptr<Title> &title, int row, bool queue_if_missing)
{
    if (!cache_enabled_ || !title || row < 0 || row >= (int)title->live_text_rows.size())
        return QImage();
    const CacheFrameKey key = liveCueKey(title, row);
    const QString state_key = liveCueStateKey(key.title_id, row);
    live_cue_rows_[state_key] = row;
    live_cue_title_ids_[state_key] = key.title_id;
    const FrameCacheState existing_state = live_cue_states_.value(state_key, FrameCacheState::NotCached);
    const bool busy = existing_state == FrameCacheState::Queued || existing_state == FrameCacheState::Rendering;
    auto cue_title = titleWithCueApplied(title, row);
    QImage image;
    if (existing_state != FrameCacheState::Stale && ram_cache_.get(key, image)) {
        if (!busy) {
            const int progress = liveCueRangeProgress(cue_title);
            live_cue_states_[state_key] = progress >= 100 ? FrameCacheState::CachedRam
                                                          : liveCueRangeState(cue_title, state_key);
            live_cue_progress_percent_[state_key] = progress;
        }
        OGS_LOG_DEBUG("LiveCue", QStringLiteral("Live cue RAM hit title=%1 row=%2 busy=%3")
                                    .arg(key.title_id).arg(row).arg(busy));
        ++live_cue_stats_.hits;
        ++live_cue_stats_.reuses;
        emit diagnosticsChanged();
        return image;
    }
    if (existing_state != FrameCacheState::Stale && disk_cache_.get(key, image)) {
        ram_cache_.put(key, image);
        if (!busy) {
            const int progress = liveCueRangeProgress(cue_title);
            live_cue_states_[state_key] = progress >= 100 ? FrameCacheState::CachedRam
                                                          : liveCueRangeState(cue_title, state_key);
            live_cue_progress_percent_[state_key] = progress;
        }
        OGS_LOG_DEBUG("LiveCue", QStringLiteral("Live cue disk hit title=%1 row=%2 busy=%3")
                                    .arg(key.title_id).arg(row).arg(busy));
        ++live_cue_stats_.hits;
        ++live_cue_stats_.reuses;
        emit diagnosticsChanged();
        return image;
    }
    if (queue_if_missing)
        queueLiveCue(title, row);
    OGS_LOG_DEBUG("LiveCue", QStringLiteral("Live cue cache miss title=%1 row=%2 queue=%3")
                                .arg(key.title_id).arg(row).arg(queue_if_missing));
    return QImage();
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
    const QString state_key = liveCueStateKey(key.title_id, row);
    return liveCueRangeState(titleWithCueApplied(title, row), state_key);
}

int CacheManager::liveCueProgressPercent(const std::shared_ptr<Title> &title, int row) const
{
    if (!cache_enabled_)
        return 100;
    if (!title || row < 0 || row >= (int)title->live_text_rows.size())
        return 0;
    const CacheFrameKey key = liveCueKey(title, row);
    const QString state_key = liveCueStateKey(key.title_id, row);
    const FrameCacheState state = liveCueState(title, row);
    if (state == FrameCacheState::CachedRam || state == FrameCacheState::CachedDisk)
        return 100;
    if (state == FrameCacheState::Disabled)
        return 100;
    return std::clamp(std::max(live_cue_progress_percent_.value(state_key, 0),
                               liveCueRangeProgress(titleWithCueApplied(title, row))),
                      0, 100);
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
    const QString state_key = liveCueStateKey(key.title_id, row);
    live_cue_rows_[state_key] = row;
    live_cue_title_ids_[state_key] = key.title_id;
    auto cue_title = titleWithCueApplied(title, row);
    if (cue_title) {
        const int max_frame = cache_last_frame_for_title(*cue_title, effectiveFrameRate());
        for (int frame = 0; frame <= max_frame; ++frame) {
            const CacheFrameKey frame_key = keyForFrame(*cue_title, frame);
            queue_.cancelKey(frame_key);
            ram_cache_.remove(frame_key);
            disk_cache_.remove(frame_key);
        }
    }
    ++live_cue_stats_.invalidations;
    live_cue_states_[state_key] = FrameCacheState::NotCached;
    live_cue_progress_percent_[state_key] = 0;
    OGS_LOG_INFO("LiveCue", QStringLiteral("Invalidated live cue title=%1 row=%2 stateKey=%3")
                              .arg(key.title_id).arg(row).arg(state_key));
    emit diagnosticsChanged();
    queueLiveCue(title, row);
    emit liveCueStateChanged(QString::fromStdString(title->id), row);
}

void CacheManager::invalidateLiveCues(const std::shared_ptr<Title> &title)
{
    if (!title) return;
    for (int row = 0; row < (int)title->live_text_rows.size(); ++row) {
        const CacheFrameKey key = liveCueKey(title, row);
        const QString state_key = liveCueStateKey(key.title_id, row);
        live_cue_rows_[state_key] = row;
        live_cue_title_ids_[state_key] = key.title_id;
        ++live_cue_stats_.invalidations;
        live_cue_states_[state_key] = FrameCacheState::Stale;
        live_cue_progress_percent_[state_key] = 0;
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
    if (!cache_enabled_) return;
    if (has_active_render_job_)
        return;
    RenderQueueManager::Job job;
    const bool normal_prerender_paused = paused_ || interactive_bypass_;
    if (normal_prerender_paused) {
        if (!queue_.takeNextLiveCue(job))
            return;
    } else if (!queue_.takeNext(job)) {
        return;
    }
    OGS_LOG_DEBUG(job.live_cue ? "LiveCue" : "Cache",
                  QStringLiteral("Dequeued render job title=%1 frame=%2 liveCue=%3 row=%4 time=%5 key=%6")
                      .arg(job.key.title_id)
                      .arg(job.key.frame)
                      .arg(job.live_cue)
                      .arg(job.cue_row)
                      .arg(job.time, 0, 'f', 3)
                      .arg(job.key.toString()));

    const QString live_state_key = job.live_cue ? liveCueStateKey(job.key.title_id, job.cue_row) : QString();
    const bool was_stale = job.live_cue
        ? live_cue_states_.value(live_state_key, FrameCacheState::NotCached) == FrameCacheState::Stale
        : state_tracker_.state(job.key) == FrameCacheState::Stale;

    if (job.live_cue) {
        if (live_cue_states_.value(live_state_key, FrameCacheState::NotCached) != FrameCacheState::Rendering) {
            live_cue_states_[live_state_key] = FrameCacheState::Rendering;
            live_cue_progress_percent_[live_state_key] = 50;
            OGS_LOG_INFO("LiveCue", QStringLiteral("Live cue rendering title=%1 row=%2 stateKey=%3")
                                      .arg(job.key.title_id).arg(job.cue_row).arg(live_state_key));
            emit liveCueStateChanged(job.key.title_id, job.cue_row);
        }
    } else {
        state_tracker_.setState(job.key, FrameCacheState::Rendering);
    }

    active_render_job_ = job;
    active_render_was_stale_ = was_stale;
    has_active_render_job_ = true;
    QTimer::singleShot(16, this, &CacheManager::renderActiveJob);
}

void CacheManager::renderActiveJob()
{
    if (!has_active_render_job_)
        return;

    const RenderQueueManager::Job job = active_render_job_;
    const bool was_stale = active_render_was_stale_;
    QImage image;
    if (was_stale || (!ram_cache_.get(job.key, image) && !disk_cache_.get(job.key, image)))
        image = render_title_to_image(*job.title, job.time);
    if (!image.isNull()) {
        ram_cache_.put(job.key, image);
        disk_cache_.put(job.key, image);
        if (job.live_cue) {
            const QString live_state_key = liveCueStateKey(job.key.title_id, job.cue_row);
            const int progress = liveCueRangeProgress(job.title);
            const FrameCacheState next_state = progress >= 100
                ? FrameCacheState::CachedRam
                : liveCueRangeState(job.title, live_state_key);
            live_cue_states_[live_state_key] = next_state;
            live_cue_progress_percent_[live_state_key] = progress;
            OGS_LOG_INFO("LiveCue", QStringLiteral("Live cue frame rendered title=%1 row=%2 progress=%3 state=%4 stateKey=%5 frameKey=%6")
                                      .arg(job.key.title_id).arg(job.cue_row).arg(progress).arg((int)next_state)
                                      .arg(live_state_key, job.key.toString()));
            emit liveCueStateChanged(job.key.title_id, job.cue_row);
        } else {
            state_tracker_.setState(job.key, FrameCacheState::CachedRam);
            emit frameReady(job.key.title_id, job.key.frame);
        }
        has_active_render_job_ = false;
        emit diagnosticsChanged();
        ensureWorkerTimerActive();
        return;
    }

    if (job.live_cue) {
        const QString live_state_key = liveCueStateKey(job.key.title_id, job.cue_row);
        live_cue_states_[live_state_key] = FrameCacheState::Stale;
        live_cue_progress_percent_[live_state_key] = 0;
        OGS_LOG_WARNING("LiveCue", QStringLiteral("Live cue render failed title=%1 row=%2 stateKey=%3 frameKey=%4")
                                     .arg(job.key.title_id).arg(job.cue_row).arg(live_state_key, job.key.toString()));
        emit liveCueStateChanged(job.key.title_id, job.cue_row);
    } else {
        state_tracker_.setState(job.key, FrameCacheState::Stale);
    }
    has_active_render_job_ = false;
    emit diagnosticsChanged();
    ensureWorkerTimerActive();
}
