#include "cache-manager.h"
#include "cache-frame-payload.h"
#include "cache-tile-payload.h"
#include "cache-time.h"
#include "title-cache-policy.h"
#include "title-snapshot.h"
#include "title-localization.h"
#include "title-source.h"
#include "title-preferences.h"
#include "title-logger.h"
#include "image-layer-utils.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QBuffer>
#include <QDataStream>
#include <QMutexLocker>
#include <QFile>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVector>
#include <QSet>
#include <QSettings>
#include <QStringList>

#ifdef OBS_BGS_HAVE_LZ4
#include <lz4.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <chrono>
#include <thread>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

static const char *cache_playback_state_name(FrameCacheState state)
{
    switch (state) {
    case FrameCacheState::NotCached: return "NotCached";
    case FrameCacheState::Queued: return "Queued";
    case FrameCacheState::Rendering: return "Rendering";
    case FrameCacheState::CachedRam: return "CachedRam";
    case FrameCacheState::CachedDisk: return "CachedDisk";
    case FrameCacheState::Stale: return "Stale";
    case FrameCacheState::Disabled: return "Disabled";
    }
    return "Unknown";
}

static void log_cache_playback(const QString &message)
{
    if (TitlePreferences::cache_playback_logging_enabled())
        BGL_LOG_INFO("CachePlayback", message);
}
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

constexpr quint32 kRawFrameMagic = 0x4f475346; // BGLF
constexpr quint16 kRawFrameVersion = 5;
constexpr quint32 kTilePayloadMagic = 0x4f475354; // BGLT
constexpr quint16 kTilePayloadVersion = 1;
constexpr quint16 kFrameCodecRaw = 0;
constexpr quint16 kFrameCodecLz4 = 1;
/* Cache payloads are straight-alpha BGRA, exactly as OBS expects for GS_BGRA.
 * Premultiplied Cairo frames are converted once on the worker, never during
 * realtime playback. */
constexpr QImage::Format kDiskFrameFormat = QImage::Format_ARGB32;
constexpr int kCacheTileSize = bgs::cache_tile_payload::kTileSize;
/* Bump whenever the GPU render graph, effect compositing, alpha contract,
 * bounds expansion, or cache/readback semantics change. This is part of the
 * visual cache identity and prevents stale prerenders from surviving renderer
 * fixes. */
constexpr const char *kGpuRendererCacheAbi =
    "gpu-renderer-v31-lens-flare-dx11-keyword-fix";

static bool gpu_ram_frame_contains(const CacheFrameKey &key)
{
    return title_gpu_frame_cache_contains(key.toString().toStdString());
}

static bool cache_ram_frame_contains(const RamFrameCache &cache,
                                     const CacheFrameKey &key)
{
    if (gpu_ram_frame_contains(key))
        return true;
    QImage ignored;
    return cache.get(key, ignored);
}

static uint64_t gpu_cache_model_revision(const CacheFrameKey &key)
{
    bool ok = false;
    uint64_t revision = key.content_hash.left(16).toULongLong(&ok, 16);
    if (!ok || revision == 0)
        revision = static_cast<uint64_t>(qHash(key.content_hash));
    revision ^= static_cast<uint64_t>(static_cast<uint32_t>(key.width)) << 32;
    revision ^= static_cast<uint64_t>(static_cast<uint32_t>(key.height));
    return revision == 0 ? 1 : revision;
}

static void set_sparse_frame_metadata(QImage &image, int x, int y,
                                      int canvas_width, int canvas_height)
{
    bgs::cache_frame_payload::set_placement(image, x, y,
                                            canvas_width, canvas_height);
}

static QRect alpha_bounds(const QImage &image)
{
    if (image.isNull())
        return QRect();
    const QImage argb = image.format() == QImage::Format_ARGB32
        ? image
        : image.convertToFormat(QImage::Format_ARGB32);
    int left = argb.width();
    int top = argb.height();
    int right = -1;
    int bottom = -1;
    for (int y = 0; y < argb.height(); ++y) {
        const QRgb *row = reinterpret_cast<const QRgb *>(argb.constScanLine(y));
        int row_left = argb.width();
        int row_right = -1;
        for (int x = 0; x < argb.width(); ++x) {
            if (qAlpha(row[x]) != 0) {
                row_left = x;
                break;
            }
        }
        if (row_left == argb.width())
            continue;
        for (int x = argb.width() - 1; x >= row_left; --x) {
            if (qAlpha(row[x]) != 0) {
                row_right = x;
                break;
            }
        }
        left = std::min(left, row_left);
        right = std::max(right, row_right);
        top = std::min(top, y);
        bottom = y;
    }
    return right >= left && bottom >= top
        ? QRect(left, top, right - left + 1, bottom - top + 1)
        : QRect();
}

static bool sparse_frame_metadata(const QImage &image, int &x, int &y,
                                  int &canvas_width, int &canvas_height)
{
    bgs::cache_frame_payload::Placement placement;
    if (!bgs::cache_frame_payload::read_placement(image, placement))
        return false;
    x = placement.x;
    y = placement.y;
    canvas_width = placement.canvas_width;
    canvas_height = placement.canvas_height;
    return true;
}

static QImage expand_sparse_frame(const QImage &image, int expected_width, int expected_height)
{
    int x = 0, y = 0, canvas_width = 0, canvas_height = 0;
    if (!sparse_frame_metadata(image, x, y, canvas_width, canvas_height)) {
        return image.width() == expected_width && image.height() == expected_height
            ? image
            : QImage();
    }
    if (canvas_width != expected_width || canvas_height != expected_height)
        return QImage();
    QImage full(canvas_width, canvas_height, kDiskFrameFormat);
    full.fill(Qt::transparent);
    const QImage source = image.format() == kDiskFrameFormat
        ? image : image.convertToFormat(kDiskFrameFormat);
    if (source.isNull() || x < 0 || y < 0 ||
        x + source.width() > full.width() ||
        y + source.height() > full.height())
        return QImage();
    const qsizetype row_bytes = qsizetype(source.width()) * 4;
    for (int row = 0; row < source.height(); ++row) {
        std::memcpy(full.scanLine(y + row) + qsizetype(x) * 4,
                    source.constScanLine(row), row_bytes);
    }
    return full;
}

static QImage make_sparse_frame(const QImage &full_frame, int canvas_width, int canvas_height)
{
    if (full_frame.isNull())
        return QImage();
    QImage straight = full_frame.format() == kDiskFrameFormat
        ? full_frame
        : full_frame.convertToFormat(kDiskFrameFormat);
    const QRect bounds = alpha_bounds(straight);
    if (bounds.isEmpty()) {
        QImage empty(1, 1, kDiskFrameFormat);
        empty.fill(Qt::transparent);
        set_sparse_frame_metadata(empty, 0, 0, canvas_width, canvas_height);
        return empty;
    }
    QImage cropped = straight.copy(bounds);
    set_sparse_frame_metadata(cropped, bounds.x(), bounds.y(), canvas_width, canvas_height);
    return cropped;
}

static quint64 cache_image_bytes(const QImage &image)
{
    return image.isNull() ? 0 : (quint64)image.bytesPerLine() * (quint64)image.height();
}

static void apply_persisted_live_cue_persistence(const std::shared_ptr<Title> &title)
{
    if (!title)
        return;

    QSettings settings(QStringLiteral("BroadcastGraphicsLive"), QStringLiteral("Dock"));
    settings.beginGroup(QStringLiteral("TitleDock"));
    const bool background = settings.value(QStringLiteral("backgroundPersistence"), false).toBool();
    const bool text = background && settings.value(QStringLiteral("textPersistence"), false).toBool();
    settings.endGroup();

    bool has_exposed = false;
    for (const auto &layer : title->layers) {
        if (layer && (layer->type == LayerType::Text || layer->type == LayerType::Ticker || layer->type == LayerType::Image) &&
            layer->expose_text) {
            has_exposed = true;
            break;
        }
    }

    title->cue_background_persistence = background && has_exposed;
    title->cue_text_persistence = title->cue_background_persistence && text;
    if (!title->cue_background_persistence)
        title->cue_persistence_transition = false;
    if (!title->cue_text_persistence)
        title->cue_persistent_text_columns.clear();
}

static void enter_cache_worker_background_mode()
{
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
#endif
}

static int clamp_cache_priority(qint64 priority)
{
    return static_cast<int>(std::clamp<qint64>(
        priority,
        std::numeric_limits<int>::min() + 1,
        std::numeric_limits<int>::max()));
}

static quint64 available_system_memory_bytes()
{
#ifdef _WIN32
    MEMORYSTATUSEX status = {};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status))
        return static_cast<quint64>(status.ullAvailPhys);
#endif
    return std::numeric_limits<quint64>::max();
}
}



DiskFrameCache::DiskFrameCache(QObject *parent) : QObject(parent)
{
    cache_dir_ = TitlePreferences::cache_disk_location();
    QDir().mkpath(cache_dir_);
    rebuildIndex();
    bytes_used_ = scanBytesUsed();
    const QFileInfo active_info(cache_dir_);
    QDir cache_parent(active_info.absolutePath());
    const QString obsolete_pattern =
        active_info.fileName() + QStringLiteral(".obsolete-*");
    for (const QFileInfo &obsolete : cache_parent.entryInfoList(
             QStringList() << obsolete_pattern,
             QDir::Dirs | QDir::NoDotAndDotDot))
        cleanup_queue_.push_back(obsolete.absoluteFilePath());
    writer_thread_ = std::thread(&DiskFrameCache::writerLoop, this);
    if (!cleanup_queue_.empty())
        writer_cv_.notify_one();
}

DiskFrameCache::~DiskFrameCache()
{
    {
        std::lock_guard<std::mutex> lock(writer_mutex_);
        writer_stop_ = true;
    }
    writer_cv_.notify_all();
    writer_space_cv_.notify_all();
    if (writer_thread_.joinable())
        writer_thread_.join();
}

void DiskFrameCache::enqueuePut(const CacheFrameKey &key, const QImage &image)
{
    if (image.isNull())
        return;
    WriteJob job;
    job.key = key;
    job.image = image;
    job.generation = writer_generation_.load(std::memory_order_acquire);
    job.bytes = static_cast<quint64>(
        std::max<qsizetype>(0, image.sizeInBytes()));
    {
        std::unique_lock<std::mutex> lock(writer_mutex_);
        /* Compression is intentionally asynchronous, but an unbounded queue of
         * full/sparse QImages merely moves the cache explosion from GPU memory
         * to system RAM. Keep at most four writes and ~64 MiB of payloads in
         * flight; an individual oversized frame is allowed only when alone. */
        writer_space_cv_.wait(lock, [this, &job]() {
            const bool byte_room = writer_pending_bytes_ == 0 ||
                (job.bytes <= writer_queue_budget_ &&
                 writer_pending_bytes_ <= writer_queue_budget_ - job.bytes);
            const std::size_t in_flight = writer_queue_.size() +
                (writer_active_ ? 1u : 0u);
            return writer_stop_ || (in_flight < 4 && byte_room);
        });
        if (writer_stop_)
            return;
        writer_pending_bytes_ += job.bytes;
        writer_queue_.push_back(std::move(job));
    }
    writer_cv_.notify_one();
}

void DiskFrameCache::flushWrites()
{
    std::unique_lock<std::mutex> lock(writer_mutex_);
    writer_idle_cv_.wait(lock, [this]() {
        return writer_queue_.empty() && cleanup_queue_.empty() &&
               !writer_active_;
    });
}

void DiskFrameCache::cancelQueuedWrites()
{
    writer_generation_.fetch_add(1, std::memory_order_acq_rel);
    {
        std::lock_guard<std::mutex> lock(writer_mutex_);
        for (const WriteJob &job : writer_queue_)
            writer_pending_bytes_ -= std::min(writer_pending_bytes_, job.bytes);
        writer_queue_.clear();
        if (!writer_active_)
            writer_idle_cv_.notify_all();
    }
    writer_space_cv_.notify_all();
}

void DiskFrameCache::writerLoop()
{
    for (;;) {
        WriteJob job;
        QString cleanup_dir;
        bool write_job = false;
        {
            std::unique_lock<std::mutex> lock(writer_mutex_);
            writer_cv_.wait(lock, [this]() {
                return writer_stop_ || !writer_queue_.empty() ||
                       !cleanup_queue_.empty();
            });
            if (writer_stop_ && writer_queue_.empty() &&
                cleanup_queue_.empty())
                break;
            if (!cleanup_queue_.empty()) {
                cleanup_dir = std::move(cleanup_queue_.front());
                cleanup_queue_.pop_front();
            } else {
                job = std::move(writer_queue_.front());
                writer_queue_.pop_front();
                write_job = true;
            }
            writer_active_ = true;
        }

        if (write_job)
            putForGeneration(job.key, job.image, job.generation);
        else if (!cleanup_dir.isEmpty())
            QDir(cleanup_dir).removeRecursively();

        {
            std::lock_guard<std::mutex> lock(writer_mutex_);
            if (write_job)
                writer_pending_bytes_ -= std::min(
                    writer_pending_bytes_, job.bytes);
            writer_active_ = false;
            if (writer_queue_.empty() && cleanup_queue_.empty())
                writer_idle_cv_.notify_all();
        }
        writer_space_cv_.notify_all();
    }
    {
        std::lock_guard<std::mutex> lock(writer_mutex_);
        writer_active_ = false;
        writer_idle_cv_.notify_all();
    }
}

QString DiskFrameCache::pathForKey(const CacheFrameKey &key) const
{
    const QByteArray digest = QCryptographicHash::hash(
        key.toString().toUtf8(), QCryptographicHash::Sha1).toHex();
    return QDir(cache_dir_).filePath(
        QString::fromLatin1(digest) + QStringLiteral(".ogsf"));
}

QString DiskFrameCache::pathForTileDigest(const QByteArray &digest) const
{
    return QDir(cache_dir_).filePath(
        QString::fromLatin1(digest.toHex()) + QStringLiteral(".ogst"));
}

bool DiskFrameCache::contains(const CacheFrameKey &key) const
{
    QMutexLocker lock(&mutex_);
    /* Manifest-backed membership is kept in memory. This method is called by
     * timeline/live-cue progress code and must not turn every frame query into
     * a filesystem metadata lookup. The worker validates the actual file when
     * it hydrates the payload. */
    return indexed_keys_.contains(key.toString());
}

bool DiskFrameCache::readFrameTileRefsLocked(
    const CacheFrameKey &key, QVector<TileRef> &refs) const
{
    refs.clear();
    QFile file(pathForKey(key));
    if (!file.open(QIODevice::ReadOnly))
        return false;

    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_5_15);
    stream.setByteOrder(QDataStream::LittleEndian);

    quint32 magic = 0;
    quint16 version = 0;
    quint16 format = 0;
    quint32 canvas_width = 0;
    quint32 canvas_height = 0;
    quint32 tile_size = 0;
    quint32 tile_count = 0;
    stream >> magic >> version >> format
           >> canvas_width >> canvas_height >> tile_size >> tile_count;
    const quint64 max_tiles =
        quint64((std::max(1, key.width) + bgs::cache_tile_payload::kTileSize - 1) /
                bgs::cache_tile_payload::kTileSize) *
        quint64((std::max(1, key.height) + bgs::cache_tile_payload::kTileSize - 1) /
                bgs::cache_tile_payload::kTileSize);
    if (stream.status() != QDataStream::Ok ||
        magic != kRawFrameMagic || version != kRawFrameVersion ||
        format != quint16(kDiskFrameFormat) ||
        canvas_width != quint32(key.width) ||
        canvas_height != quint32(key.height) ||
        tile_size != quint32(bgs::cache_tile_payload::kTileSize) ||
        tile_count > max_tiles)
        return false;

    const QRect canvas_rect(0, 0, key.width, key.height);
    refs.reserve(int(tile_count));
    for (quint32 index = 0; index < tile_count; ++index) {
        qint32 x = 0;
        qint32 y = 0;
        quint32 width = 0;
        quint32 height = 0;
        QByteArray digest;
        stream >> x >> y >> width >> height >> digest;
        const QRect rect(x, y, int(width), int(height));
        if (stream.status() != QDataStream::Ok || digest.size() != 32 ||
            width == 0 || height == 0 ||
            width > quint32(bgs::cache_tile_payload::kTileSize) ||
            height > quint32(bgs::cache_tile_payload::kTileSize) ||
            !canvas_rect.contains(rect))
            return false;
        refs.push_back({rect, digest});
    }
    return stream.status() == QDataStream::Ok;
}

bool DiskFrameCache::readTileLocked(const TileRef &ref, QImage &image) const
{
    image = QImage();
    QFile file(pathForTileDigest(ref.digest));
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
    quint16 codec = 0;
    quint16 reserved = 0;
    quint32 raw_size = 0;
    quint32 payload_size = 0;
    stream >> magic >> version >> format >> width >> height >> row_bytes
           >> codec >> reserved >> raw_size >> payload_size;
    const quint64 expected_row_bytes = quint64(ref.rect.width()) * 4ull;
    const quint64 expected_raw_size =
        expected_row_bytes * quint64(ref.rect.height());
    if (stream.status() != QDataStream::Ok ||
        magic != kTilePayloadMagic || version != kTilePayloadVersion ||
        format != quint16(kDiskFrameFormat) ||
        width != quint32(ref.rect.width()) ||
        height != quint32(ref.rect.height()) ||
        row_bytes != expected_row_bytes || raw_size != expected_raw_size ||
        reserved != 0 ||
        raw_size > quint32(std::numeric_limits<int>::max()) ||
        payload_size > quint32(std::numeric_limits<int>::max()) ||
        (codec != kFrameCodecRaw && codec != kFrameCodecLz4))
        return false;

    const QByteArray payload = file.read(payload_size);
    if (payload.size() != int(payload_size))
        return false;
    QByteArray raw;
    if (codec == kFrameCodecRaw) {
        raw = payload;
    } else {
#ifdef OBS_BGS_HAVE_LZ4
        raw.resize(int(raw_size));
        const int decoded = LZ4_decompress_safe(
            payload.constData(), raw.data(), int(payload.size()), int(raw.size()));
        if (decoded != raw.size())
            return false;
#else
        return false;
#endif
    }
    if (raw.size() != int(raw_size))
        return false;

    QImage loaded(ref.rect.size(), kDiskFrameFormat);
    if (loaded.isNull())
        return false;
    for (int y = 0; y < loaded.height(); ++y) {
        std::memcpy(loaded.scanLine(y),
                    raw.constData() + qsizetype(y) * row_bytes,
                    size_t(row_bytes));
    }
    if (bgs::cache_tile_payload::digest_for_tile(loaded) != ref.digest)
        return false;
    image = std::move(loaded);
    return true;
}

bool DiskFrameCache::get(const CacheFrameKey &key, QImage &image) const
{
    QMutexLocker lock(&mutex_);
    if (!indexed_keys_.contains(key.toString()))
        return false;

    QVector<TileRef> refs;
    if (!readFrameTileRefsLocked(key, refs))
        return false;

    QVector<bgs::cache_tile_payload::Tile> tiles;
    tiles.reserve(refs.size());
    for (const TileRef &ref : refs) {
        QImage tile_image;
        if (!readTileLocked(ref, tile_image))
            return false;
        tiles.push_back({ref.rect, std::move(tile_image), ref.digest});
    }
    image = bgs::cache_tile_payload::compose_sparse_tiles(
        tiles, key.width, key.height);
    return !image.isNull();
}

void DiskFrameCache::put(const CacheFrameKey &key, const QImage &image)
{
    if (image.isNull())
        return;
    QMutexLocker lock(&mutex_);
    putLocked(key, image);
}

void DiskFrameCache::putForGeneration(const CacheFrameKey &key,
                                      const QImage &image,
                                      quint64 generation)
{
    if (image.isNull())
        return;
    QMutexLocker lock(&mutex_);
    /* Re-check only after acquiring the disk-cache lock. clear(), clearFast(),
     * and setCacheDirectory() advance the generation before taking this lock,
     * so an already-dequeued stale write can neither recreate a cleared frame
     * nor land in the replacement cache directory. */
    if (generation != writer_generation_.load(std::memory_order_acquire))
        return;
    putLocked(key, image);
}

bool DiskFrameCache::writeTileLocked(const QImage &image,
                                     const QByteArray &digest,
                                     quint64 &created_bytes)
{
    if (image.isNull() || digest.size() != 32)
        return false;
    const QString path = pathForTileDigest(digest);
    if (QFileInfo::exists(path)) {
        TileRef existing_ref;
        existing_ref.rect = QRect(QPoint(0, 0), image.size());
        existing_ref.digest = digest;
        QImage existing_image;
        if (readTileLocked(existing_ref, existing_image))
            return true;
        const quint64 corrupt_size = quint64(QFileInfo(path).size());
        if (QFile::remove(path))
            bytes_used_ -= std::min(bytes_used_, corrupt_size);
    }

    const QImage frame = image.format() == kDiskFrameFormat
        ? image : image.convertToFormat(kDiskFrameFormat);
    const QByteArray raw = bgs::cache_tile_payload::raw_bgra_bytes(frame);
    if (raw.isEmpty() || raw.size() > std::numeric_limits<int>::max())
        return false;

    QByteArray payload = raw;
    quint16 codec = kFrameCodecRaw;
#ifdef OBS_BGS_HAVE_LZ4
    if (raw.size() <= LZ4_MAX_INPUT_SIZE) {
        QByteArray compressed;
        const int raw_size = int(raw.size());
        compressed.resize(LZ4_compressBound(raw_size));
        const int compressed_size = LZ4_compress_default(
            raw.constData(), compressed.data(), raw_size,
            int(compressed.size()));
        if (compressed_size > 0 && compressed_size < raw_size) {
            compressed.resize(compressed_size);
            payload = std::move(compressed);
            codec = kFrameCodecLz4;
        }
    }
#endif

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_5_15);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << kTilePayloadMagic << kTilePayloadVersion
           << quint16(kDiskFrameFormat)
           << quint32(frame.width()) << quint32(frame.height())
           << quint32(frame.width() * 4)
           << codec << quint16(0)
           << quint32(raw.size()) << quint32(payload.size());
    if (stream.status() != QDataStream::Ok ||
        file.write(payload) != payload.size()) {
        file.cancelWriting();
        return false;
    }
    if (!file.commit())
        return false;
    created_bytes += quint64(QFileInfo(path).size());
    return true;
}

void DiskFrameCache::addTileReferencesLocked(
    const QVector<QByteArray> &digests)
{
    for (const QByteArray &digest : digests)
        tile_ref_counts_[digest] = tile_ref_counts_.value(digest) + 1;
}

void DiskFrameCache::releaseTileReferencesLocked(
    const QVector<QByteArray> &digests)
{
    for (const QByteArray &digest : digests) {
        const qsizetype remaining = tile_ref_counts_.value(digest) - 1;
        if (remaining > 0) {
            tile_ref_counts_[digest] = remaining;
            continue;
        }
        tile_ref_counts_.remove(digest);
        const QString path = pathForTileDigest(digest);
        const quint64 size = QFileInfo(path).exists()
            ? quint64(QFileInfo(path).size()) : 0;
        if (QFile::remove(path))
            bytes_used_ -= std::min(bytes_used_, size);
    }
}

void DiskFrameCache::putLocked(const CacheFrameKey &key, const QImage &image)
{
    QDir().mkpath(cache_dir_);
    const QImage frame = image.format() == kDiskFrameFormat
        ? image : image.convertToFormat(kDiskFrameFormat);
    bgs::cache_frame_payload::Placement placement;
    if (!bgs::cache_frame_payload::read_placement(frame, placement) ||
        placement.canvas_width != key.width ||
        placement.canvas_height != key.height)
        return;

    const QVector<bgs::cache_tile_payload::Tile> tiles =
        bgs::cache_tile_payload::extract_nonempty_tiles(frame, placement);
    quint64 created_tile_bytes = 0;
    QVector<QByteArray> created_tile_digests;
    QVector<QByteArray> new_digests;
    new_digests.reserve(tiles.size());
    const auto discard_unreferenced_created_tiles = [&]() {
        for (const QByteArray &created : created_tile_digests) {
            if (!tile_ref_counts_.contains(created))
                QFile::remove(pathForTileDigest(created));
        }
    };
    for (const auto &tile : tiles) {
        const quint64 created_before = created_tile_bytes;
        if (!writeTileLocked(tile.image, tile.digest, created_tile_bytes)) {
            discard_unreferenced_created_tiles();
            return;
        }
        if (created_tile_bytes > created_before &&
            !tile_ref_counts_.contains(tile.digest))
            created_tile_digests.push_back(tile.digest);
        new_digests.push_back(tile.digest);
    }

    const QString indexed_key = key.toString();
    const bool already_indexed = indexed_keys_.contains(indexed_key);
    const QVector<QByteArray> previous_digests =
        frame_tile_digests_.value(indexed_key);
    const QString path = pathForKey(key);
    const quint64 previous_size = QFileInfo(path).exists()
        ? quint64(QFileInfo(path).size()) : 0;
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        discard_unreferenced_created_tiles();
        return;
    }

    QDataStream stream(&file);
    stream.setVersion(QDataStream::Qt_5_15);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << kRawFrameMagic << kRawFrameVersion
           << quint16(kDiskFrameFormat)
           << quint32(key.width) << quint32(key.height)
           << quint32(bgs::cache_tile_payload::kTileSize)
           << quint32(tiles.size());
    for (const auto &tile : tiles) {
        stream << qint32(tile.rect.x()) << qint32(tile.rect.y())
               << quint32(tile.rect.width()) << quint32(tile.rect.height())
               << tile.digest;
    }
    if (stream.status() != QDataStream::Ok) {
        file.cancelWriting();
        discard_unreferenced_created_tiles();
        return;
    }
    if (!file.commit()) {
        discard_unreferenced_created_tiles();
        return;
    }

    const quint64 new_frame_size = quint64(QFileInfo(path).size());
    bytes_used_ = bytes_used_ - std::min(bytes_used_, previous_size) +
                  new_frame_size + created_tile_bytes;
    addTileReferencesLocked(new_digests);
    releaseTileReferencesLocked(previous_digests);
    frame_tile_digests_[indexed_key] = new_digests;
    indexed_keys_[indexed_key] = key;
    if (!already_indexed)
        appendManifestEntry(key);
}

QVector<CacheFrameKey> DiskFrameCache::keysForTitle(const QString &title_id) const
{
    QMutexLocker lock(&mutex_);
    QVector<CacheFrameKey> keys;
    keys.reserve(indexed_keys_.size());
    for (auto it = indexed_keys_.cbegin(); it != indexed_keys_.cend(); ++it) {
        if (it.value().title_id == title_id)
            keys.push_back(it.value());
    }
    return keys;
}

void DiskFrameCache::remove(const CacheFrameKey &key)
{
    QMutexLocker lock(&mutex_);
    const QString indexed_key = key.toString();
    const QVector<QByteArray> digests =
        frame_tile_digests_.take(indexed_key);
    const QString path = pathForKey(key);
    const quint64 previous_size = QFileInfo(path).exists() ? (quint64)QFileInfo(path).size() : 0;
    if (QFile::remove(path))
        bytes_used_ -= std::min(bytes_used_, previous_size);
    indexed_keys_.remove(indexed_key);
    releaseTileReferencesLocked(digests);
}

void DiskFrameCache::clear()
{
    cancelQueuedWrites();
    QMutexLocker lock(&mutex_);
    QDir dir(cache_dir_);
    if (!dir.exists()) {
        indexed_keys_.clear();
        frame_tile_digests_.clear();
        tile_ref_counts_.clear();
        bytes_used_ = 0;
        return;
    }
    for (const QFileInfo &file : dir.entryInfoList(
             QStringList() << QStringLiteral("*.ogsf")
                           << QStringLiteral("*.ogst")
                           << QStringLiteral("*.tmp"), QDir::Files))
        QFile::remove(file.absoluteFilePath());
    QFile::remove(manifestPath());
    indexed_keys_.clear();
    frame_tile_digests_.clear();
    tile_ref_counts_.clear();
    bytes_used_ = 0;
    BGL_LOG_INFO("DiskCache", QStringLiteral("Cleared disk frame cache directory=%1")
        .arg(cache_dir_));
}

void DiskFrameCache::clearFast()
{
    cancelQueuedWrites();
    QMutexLocker lock(&mutex_);
    const QString old_dir = cache_dir_;
    const QString tombstone = old_dir + QStringLiteral(".obsolete-%1")
        .arg(QDateTime::currentMSecsSinceEpoch());

    QDir old(old_dir);
    if (old.exists()) {
        QDir parent(QFileInfo(old_dir).absolutePath());
        parent.rename(QFileInfo(old_dir).fileName(), QFileInfo(tombstone).fileName());
    }
    QDir().mkpath(old_dir);
    indexed_keys_.clear();
    frame_tile_digests_.clear();
    tile_ref_counts_.clear();
    bytes_used_ = 0;
    BGL_LOG_INFO("DiskCache", QStringLiteral(
        "Detached disk cache generation directory=%1 tombstone=%2")
        .arg(old_dir, tombstone));

    /* The active generation is detached synchronously, then reclaimed by the
     * owned writer thread. Unlike a detached cleanup thread, this worker is
     * joined during shutdown, so obsolete cache generations cannot accumulate
     * indefinitely or outlive Qt/OBS. */
    if (QDir(tombstone).exists()) {
        {
            std::lock_guard<std::mutex> writer_lock(writer_mutex_);
            cleanup_queue_.push_back(tombstone);
        }
        writer_cv_.notify_one();
    }
}

void DiskFrameCache::setCacheDirectory(const QString &path)
{
    cancelQueuedWrites();
    QMutexLocker lock(&mutex_);
    const QString clean = QDir::cleanPath(path);
    if (clean.isEmpty() || clean == cache_dir_)
        return;
    cache_dir_ = clean;
    QDir().mkpath(cache_dir_);
    rebuildIndex();
    bytes_used_ = scanBytesUsed();
    BGL_LOG_INFO("DiskCache", QStringLiteral(
        "Set disk cache directory=%1 indexedFrames=%2 bytes=%3")
        .arg(cache_dir_)
        .arg(indexed_keys_.size())
        .arg(bytes_used_));
}

QString DiskFrameCache::cacheDirectory() const
{
    QMutexLocker lock(&mutex_);
    return cache_dir_;
}

quint64 DiskFrameCache::bytesUsed() const
{
    QMutexLocker lock(&mutex_);
    return bytes_used_;
}


void DiskFrameCache::rebuildIndex()
{
    indexed_keys_.clear();
    frame_tile_digests_.clear();
    tile_ref_counts_.clear();
    QSet<QString> indexed_frame_files;
    QFile manifest(manifestPath());
    if (!manifest.open(QIODevice::ReadOnly)) {
        QDir dir(cache_dir_);
        for (const QFileInfo &file : dir.entryInfoList(
                 QStringList() << QStringLiteral("*.ogsf")
                               << QStringLiteral("*.ogst")
                               << QStringLiteral("*.tmp"), QDir::Files))
            QFile::remove(file.absoluteFilePath());
        return;
    }

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
        }
    };
    auto add_anim_vec2 = [&](const AnimatedVec2Property &p) {
        add(p.static_value.x); add(p.static_value.y); add((quint64)p.keyframes.size());
        for (const auto &k : p.keyframes) {
            add(k.time); add(k.value.x); add(k.value.y); add((int)k.easing); add(k.cx1); add(k.cy1); add(k.cx2); add(k.cy2);
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
        add(QString::fromStdString(layer->mask_source_id));
        add((int)layer->mask_mode);
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

        const QString image_path = QString::fromStdString(layer->image_path);
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

    for (const auto &layer : title.layers) {
        if (!layer) continue;
        add(layer->visible && time >= layer->in_time && time <= layer->out_time);
        add_vec(layer->position, 0.25);
        add_vec(layer->scale, 0.0005);
        add_anim(layer->rotation, 0.05);
        add(quant(layer->transform_quad_tl_x, 0.0005)); add(quant(layer->transform_quad_tl_y, 0.0005));
        add(quant(layer->transform_quad_tr_x, 0.0005)); add(quant(layer->transform_quad_tr_y, 0.0005));
        add(quant(layer->transform_quad_br_x, 0.0005)); add(quant(layer->transform_quad_br_y, 0.0005));
        add(quant(layer->transform_quad_bl_x, 0.0005)); add(quant(layer->transform_quad_bl_y, 0.0005));
        add_anim(layer->opacity, 0.002);
        add_vec(layer->size, 0.25);
        add_vec(layer->image_size, 0.25);
        add_vec(layer->origin_prop, 0.25);
        add_anim(layer->font_size_prop, 0.10);
        add_anim(layer->char_tracking_prop, 0.10);
        add_anim(layer->char_scale_x_prop, 0.0005);
        add_anim(layer->char_scale_y_prop, 0.0005);
        add_anim(layer->baseline_shift_prop, 0.10);
        add_anim(layer->text_color_a, 1.0 / 255.0); add_anim(layer->text_color_r, 1.0 / 255.0);
        add_anim(layer->text_color_g, 1.0 / 255.0); add_anim(layer->text_color_b, 1.0 / 255.0);
        add_anim(layer->paragraph_indent_left_prop, 0.25);
        add_anim(layer->paragraph_indent_right_prop, 0.25);
        add_anim(layer->paragraph_indent_first_line_prop, 0.25);
        add_anim(layer->paragraph_space_before_prop, 0.25);
        add_anim(layer->paragraph_space_after_prop, 0.25);
        add_anim(layer->fill_color_a, 1.0 / 255.0); add_anim(layer->fill_color_r, 1.0 / 255.0);
        add_anim(layer->fill_color_g, 1.0 / 255.0); add_anim(layer->fill_color_b, 1.0 / 255.0);
        add_anim(layer->background_enabled_prop, 1.0);
        add_anim(layer->background_opacity_prop, 0.002);
        add_anim(layer->background_padding_x_prop, 0.25);
        add_anim(layer->background_padding_y_prop, 0.25);
        add_anim(layer->background_padding_left_prop, 0.25);
        add_anim(layer->background_padding_right_prop, 0.25);
        add_anim(layer->background_padding_top_prop, 0.25);
        add_anim(layer->background_padding_bottom_prop, 0.25);
        add_anim(layer->background_corner_radius_prop, 0.25);
        add_anim(layer->background_corner_radius_tl_prop, 0.25);
        add_anim(layer->background_corner_radius_tr_prop, 0.25);
        add_anim(layer->background_corner_radius_br_prop, 0.25);
        add_anim(layer->background_corner_radius_bl_prop, 0.25);
        add_anim(layer->background_stroke_width_prop, 0.25);
        add_anim(layer->background_stroke_opacity_prop, 0.002);
        add_anim(layer->background_color_a, 1.0 / 255.0);
        add_anim(layer->background_color_r, 1.0 / 255.0);
        add_anim(layer->background_color_g, 1.0 / 255.0);
        add_anim(layer->background_color_b, 1.0 / 255.0);
        add_anim(layer->background_stroke_color_a, 1.0 / 255.0);
        add_anim(layer->background_stroke_color_r, 1.0 / 255.0);
        add_anim(layer->background_stroke_color_g, 1.0 / 255.0);
        add_anim(layer->background_stroke_color_b, 1.0 / 255.0);
        add_anim(layer->shadow_enabled_prop, 1.0);
        add_anim(layer->shadow_opacity_prop, 0.002);
        add_anim(layer->shadow_distance_prop, 0.25);
        add_anim(layer->shadow_angle_prop, 0.05);
        add_anim(layer->shadow_blur_prop, 0.25);
        add_anim(layer->shadow_spread_prop, 0.25);
        add_anim(layer->shadow_color_a, 1.0 / 255.0);
        add_anim(layer->shadow_color_r, 1.0 / 255.0);
        add_anim(layer->shadow_color_g, 1.0 / 255.0);
        add_anim(layer->shadow_color_b, 1.0 / 255.0);
        for (const auto &effect : layer->effects) add_effect(effect);
        for (const auto &transition : layer->transitions) {
            if (!transition.enabled)
                continue;
            add(quant(layer_transition_progress(transition, layer->in_time, layer->out_time, time), 0.002));
        }
    }
    return QString::fromLatin1(hash.result().toHex());
}

QString CacheManager::temporalStateKey(const CacheFrameKey &key,
                                       const QString &visual_state_hash) const
{
    return QStringLiteral("%1|%2|%3x%4|%5")
        .arg(key.title_id, key.content_hash)
        .arg(key.width).arg(key.height)
        .arg(visual_state_hash);
}

CacheFrameKey CacheManager::keyForFrame(const Title &title, int frame) const
{
    const int clamped_frame = titleHasTimelineChanges(title)
        ? std::clamp(frame, 0, cache_last_frame_for_title(title, effectiveFrameRate()))
        : 0;
    return {QString::fromStdString(title.id), contentHash(title), clamped_frame, title.width, title.height};
}

CacheFrameKey CacheManager::keyForTime(const Title &title, double time) const
{
    return keyForTime(title, time, QString());
}

CacheFrameKey CacheManager::keyForTime(const Title &title, double time,
                                        const QString &known_content_hash) const
{
    const int clamped_frame = frameIndexForTitleTime(title, time);
    return {QString::fromStdString(title.id),
            known_content_hash.isEmpty() ? contentHash(title) : known_content_hash,
            clamped_frame, title.width, title.height};
}

int CacheManager::frameIndexForTitleTime(const Title &title, double time) const
{
    const double clamped_time = std::clamp(
        time, 0.0, std::max(0.0, title.duration));
    const double frame_rate = effectiveFrameRate();
    return cache_frame_index_for_title_time(
        clamped_time, frame_rate, titleHasTimelineChanges(title),
        cache_last_frame_for_title(title, frame_rate));
}

QVector<CacheFrameKey> CacheManager::frameKeysForTitle(const Title &title) const
{
    const bool animated = titleHasTimelineChanges(title);
    const int last_frame = animated ? cache_last_frame_for_title(title, effectiveFrameRate()) : 0;
    const QString title_id = QString::fromStdString(title.id);
    const QString content_hash = contentHash(title);
    QVector<CacheFrameKey> keys;
    keys.reserve(last_frame + 1);
    for (int frame = 0; frame <= last_frame; ++frame)
        keys.push_back({title_id, content_hash, frame, title.width, title.height});
    return keys;
}

bool CacheManager::titleHasTimelineChanges(const Title &title) const
{
    for (const auto &layer : title.layers) {
        if (!layer) continue;
        if (layer->type == LayerType::Clock || layer->type == LayerType::Ticker)
            return true;
        if (std::any_of(layer->transitions.begin(), layer->transitions.end(),
                        [](const LayerTransition &transition) { return transition.enabled; }))
            return true;
        auto animated = [](const auto &prop) { return !prop.keyframes.empty(); };
        if (animated(layer->position) || animated(layer->scale) || animated(layer->rotation) ||
            animated(layer->opacity) || animated(layer->size) || animated(layer->image_size) || animated(layer->origin_prop) ||
            animated(layer->font_size_prop) || animated(layer->char_tracking_prop) ||
            animated(layer->char_scale_x_prop) || animated(layer->char_scale_y_prop) ||
            animated(layer->baseline_shift_prop) || animated(layer->paragraph_indent_left_prop) ||
            animated(layer->paragraph_indent_right_prop) || animated(layer->paragraph_indent_first_line_prop) ||
            animated(layer->paragraph_space_before_prop) || animated(layer->paragraph_space_after_prop) ||
            animated(layer->text_color_a) || animated(layer->text_color_r) ||
            animated(layer->text_color_g) || animated(layer->text_color_b) ||
            animated(layer->fill_color_a) || animated(layer->fill_color_r) ||
            animated(layer->fill_color_g) || animated(layer->fill_color_b) ||
            animated(layer->background_enabled_prop) || animated(layer->background_opacity_prop) ||
            animated(layer->background_padding_x_prop) || animated(layer->background_padding_y_prop) ||
            animated(layer->background_padding_left_prop) || animated(layer->background_padding_right_prop) ||
            animated(layer->background_padding_top_prop) || animated(layer->background_padding_bottom_prop) ||
            animated(layer->background_corner_radius_prop) || animated(layer->background_corner_radius_tl_prop) ||
            animated(layer->background_corner_radius_tr_prop) || animated(layer->background_corner_radius_br_prop) ||
            animated(layer->background_corner_radius_bl_prop) || animated(layer->background_stroke_width_prop) ||
            animated(layer->background_stroke_opacity_prop) || animated(layer->background_color_a) ||
            animated(layer->background_color_r) || animated(layer->background_color_g) ||
            animated(layer->background_color_b) || animated(layer->background_stroke_color_a) ||
            animated(layer->background_stroke_color_r) || animated(layer->background_stroke_color_g) ||
            animated(layer->background_stroke_color_b) || animated(layer->shadow_enabled_prop) ||
            animated(layer->shadow_opacity_prop) || animated(layer->shadow_distance_prop) ||
            animated(layer->shadow_angle_prop) || animated(layer->shadow_blur_prop) ||
            animated(layer->shadow_spread_prop) || animated(layer->shadow_color_a) ||
            animated(layer->shadow_color_r) || animated(layer->shadow_color_g) ||
            animated(layer->shadow_color_b))
            return true;
        for (const auto &effect : layer->effects) {
            if (animated(effect.enabled_prop) || animated(effect.opacity_prop) || animated(effect.size_prop) ||
                animated(effect.distance_prop) || animated(effect.angle_prop) || animated(effect.spread_prop) ||
                animated(effect.falloff_prop) || animated(effect.stroke_width_prop) ||
                animated(effect.stroke_opacity_prop) || animated(effect.padding_left_prop) ||
                animated(effect.padding_right_prop) || animated(effect.padding_top_prop) ||
                animated(effect.padding_bottom_prop) || animated(effect.corner_radius_tl_prop) ||
                animated(effect.corner_radius_tr_prop) || animated(effect.corner_radius_br_prop) ||
                animated(effect.corner_radius_bl_prop) || animated(effect.color_a) ||
                animated(effect.color_r) || animated(effect.color_g) || animated(effect.color_b) ||
                animated(effect.stroke_color_a) || animated(effect.stroke_color_r) ||
                animated(effect.stroke_color_g) || animated(effect.stroke_color_b) ||
                animated(effect.amount_prop) || animated(effect.scale_prop) ||
                animated(effect.softness_prop) || animated(effect.roundness_prop) ||
                animated(effect.speed_prop) || animated(effect.center_x_prop) ||
                animated(effect.center_y_prop) || animated(effect.complexity_prop) ||
                animated(effect.evolution_prop) || animated(effect.secondary_color_a) ||
                animated(effect.secondary_color_r) || animated(effect.secondary_color_g) ||
                animated(effect.secondary_color_b) ||
                (effect.type == LayerEffectType::Noise && effect.effect_animated && effect.enabled))
                return true;
        }
        if (layer->in_time > 0.0 || layer->out_time < title.duration)
            return true;
    }
    return false;
}

QImage CacheManager::requestFrame(const std::shared_ptr<Title> &title, double time, bool cached_only)
{
    if (!title) {
        log_cache_playback(QStringLiteral("request kind=editor result=null-title time=%1 cachedOnly=%2")
            .arg(time, 0, 'f', 6).arg(cached_only));
        return QImage();
    }

    const TitleCacheability cacheability = titleCacheability(title);
    if (cacheability == TitleCacheability::NonCacheable ||
        interactive_bypass_.load() || !cache_enabled_.load()) {
        log_cache_playback(QStringLiteral("request kind=editor result=bypass title=%1 time=%2 cachedOnly=%3 cacheability=%4 enabled=%5 interactive=%6")
            .arg(QString::fromStdString(title->id)).arg(time, 0, 'f', 6).arg(cached_only)
            .arg(static_cast<int>(cacheability)).arg(cache_enabled_.load()).arg(interactive_bypass_.load()));
        return QImage();
    }

    const double clamped_time = std::clamp(
        time, 0.0, std::max(0.0, title->duration));
    const auto finish_cached_frame = [&](const QImage &cached_frame) {
        return cached_frame;
    };

    const QString content_hash = contentHash(*title);
    const CacheFrameKey key = keyForTime(*title, time, content_hash);
    const FrameCacheState initial_state = state_tracker_.state(key);
    log_cache_playback(QStringLiteral("request kind=editor title=%1 time=%2 clamped=%3 frame=%4 state=%5 cachedOnly=%6 key=%7")
        .arg(key.title_id).arg(time, 0, 'f', 6).arg(clamped_time, 0, 'f', 6).arg(key.frame)
        .arg(QString::fromUtf8(cache_playback_state_name(initial_state))).arg(cached_only).arg(key.toString()));
    QImage image;
    if (initial_state != FrameCacheState::Stale && ram_cache_.get(key, image)) {
        state_tracker_.setState(key, FrameCacheState::CachedRam);
        log_cache_playback(QStringLiteral("result=ram-hit kind=editor title=%1 frame=%2 cacheKey=%3 size=%4x%5 key=%6")
            .arg(key.title_id).arg(key.frame).arg(image.cacheKey()).arg(image.width()).arg(image.height()).arg(key.toString()));
        return finish_cached_frame(image);
    }

    if (initial_state != FrameCacheState::Stale && disk_cache_.get(key, image)) {
        ram_cache_.put(key, image);
        state_tracker_.setState(key, FrameCacheState::CachedRam);
        emit diagnosticsChanged();
        log_cache_playback(QStringLiteral("result=disk-hit-promoted kind=editor title=%1 frame=%2 cacheKey=%3 size=%4x%5 key=%6")
            .arg(key.title_id).arg(key.frame).arg(image.cacheKey()).arg(image.width()).arg(image.height()).arg(key.toString()));
        return finish_cached_frame(image);
    }

    queueRealtimeJob(title, clamped_time, false, -1, QString(), content_hash);
    log_cache_playback(QStringLiteral("result=miss-queued kind=editor title=%1 frame=%2 initialState=%3 cachedOnly=%4 key=%5")
        .arg(key.title_id).arg(key.frame).arg(QString::fromUtf8(cache_playback_state_name(initial_state)))
        .arg(cached_only).arg(key.toString()));
    return QImage();
}

QString CacheManager::requestFrameGpuToken(
    const std::shared_ptr<Title> &title, double time,
    bool queue_if_missing, const QString &known_content_hash)
{
    if (!title || !cache_enabled_.load() || interactive_bypass_.load() ||
        titleCacheability(title) == TitleCacheability::NonCacheable)
        return QString();

    const double clamped_time = std::clamp(
        time, 0.0, std::max(0.0, title->duration));
    const CacheFrameKey key = keyForTime(
        *title, clamped_time, known_content_hash);
    const FrameCacheState state = state_tracker_.state(key);
    if (state != FrameCacheState::Stale &&
        title_gpu_frame_cache_contains(key.toString().toStdString())) {
        state_tracker_.setState(key, FrameCacheState::CachedRam);
        return key.toString();
    }

    if (queue_if_missing) {
        queueRealtimeJob(title, clamped_time, false, -1, QString(),
                         known_content_hash);
    }
    return QString();
}

quint64 CacheManager::ramBytesUsed() const
{
    return static_cast<quint64>(title_gpu_frame_cache_bytes_used()) +
           ram_cache_.bytesUsed();
}

void CacheManager::queueRealtimeJob(const std::shared_ptr<Title> &title, double time,
                                    bool live_cue, int cue_row, const QString &cue_state_key,
                                    const QString &known_content_hash)
{
    if (!title || !cache_enabled_.load() || interactive_bypass_.load() ||
        titleCacheability(title) == TitleCacheability::NonCacheable)
        return;
    const CacheFrameKey key = keyForTime(*title, time, known_content_hash);
    const bool force_render = state_tracker_.state(key) == FrameCacheState::Stale;
    RenderQueueManager::Job job;
    job.key = key;
    job.title = snapshotForJob(title, key);
    job.time = timeForFrame(key.frame);
    job.priority = live_cue ? static_cast<int>(RenderQueueManager::PriorityBand::LiveCue) * 100000
                            : static_cast<int>(RenderQueueManager::PriorityBand::Visible) * 100000;
    job.live_cue = live_cue;
    job.realtime = true;
    job.urgent = live_cue;
    job.force_render = force_render;
    job.cue_row = cue_row;
    job.cue_state_key = cue_state_key;
    job.cache_epoch = cache_epoch_.load();
    job.title_generation = titleGeneration(key.title_id);
    if (queue_.enqueue(job)) {
        state_tracker_.setState(key, FrameCacheState::Queued);
        wakeWorker();
    }
}

void CacheManager::rejectFramePayload(const std::shared_ptr<Title> &title,
                                      double time,
                                      const QString &known_content_hash)
{
    if (!title)
        return;

    const CacheFrameKey key = keyForTime(*title, time, known_content_hash);
    {
        std::lock_guard<std::recursive_mutex> publication_lock(publication_mutex_);
        ram_cache_.remove(key);
        title_gpu_frame_cache_remove(key.toString().toStdString());
        disk_cache_.remove(key);
        state_tracker_.setState(key, FrameCacheState::Stale);
    }
    BGL_LOG_WARNING("CachePlayback", QStringLiteral(
        "Rejected invalid cache payload title=%1 frame=%2 key=%3")
        .arg(key.title_id).arg(key.frame).arg(key.toString()));
    queueRealtimeJob(title, time, false, -1, QString(), known_content_hash);
    emit diagnosticsChanged();
}

QImage CacheManager::requestFrameRealtime(const std::shared_ptr<Title> &title, double time,
                                              const QString &known_content_hash)
{
    if (!title || !cache_enabled_.load() || interactive_bypass_.load() ||
        titleCacheability(title) == TitleCacheability::NonCacheable) {
        log_cache_playback(QStringLiteral("request kind=standard result=bypass title=%1 time=%2 enabled=%3 interactive=%4")
            .arg(title ? QString::fromStdString(title->id) : QStringLiteral("<null>"))
            .arg(time, 0, 'f', 6).arg(cache_enabled_.load()).arg(interactive_bypass_.load()));
        return QImage();
    }
    const CacheFrameKey key = keyForTime(*title, time, known_content_hash);
    const FrameCacheState initial_state = state_tracker_.state(key);
    log_cache_playback(QStringLiteral("request kind=standard title=%1 time=%2 frame=%3 state=%4 key=%5")
        .arg(key.title_id).arg(time, 0, 'f', 6).arg(key.frame)
        .arg(QString::fromUtf8(cache_playback_state_name(initial_state)), key.toString()));
    if (initial_state == FrameCacheState::Stale) {
        queueRealtimeJob(title, time, false, -1, QString(), known_content_hash);
        log_cache_playback(QStringLiteral("result=stale-queued title=%1 frame=%2 key=%3")
            .arg(key.title_id).arg(key.frame).arg(key.toString()));
        return QImage();
    }
    QImage image;
    if (ram_cache_.get(key, image)) {
        state_tracker_.setState(key, FrameCacheState::CachedRam);
        log_cache_playback(QStringLiteral("result=ram-hit title=%1 frame=%2 cacheKey=%3 size=%4x%5 key=%6")
            .arg(key.title_id).arg(key.frame).arg(image.cacheKey()).arg(image.width()).arg(image.height()).arg(key.toString()));
        return image;
    }

    if (initial_state == FrameCacheState::CachedDisk && disk_cache_.get(key, image)) {
        ram_cache_.put(key, image);
        state_tracker_.setState(key, FrameCacheState::CachedRam);
        log_cache_playback(QStringLiteral("result=disk-hit-promoted title=%1 frame=%2 cacheKey=%3 size=%4x%5 key=%6")
            .arg(key.title_id).arg(key.frame).arg(image.cacheKey()).arg(image.width()).arg(image.height()).arg(key.toString()));
        return image;
    }

    queueRealtimeJob(title, time, false, -1, QString(), known_content_hash);
    log_cache_playback(QStringLiteral("result=miss-queued title=%1 frame=%2 initialState=%3 diskAttempt=%4 key=%5")
        .arg(key.title_id).arg(key.frame)
        .arg(QString::fromUtf8(cache_playback_state_name(initial_state)))
        .arg(initial_state == FrameCacheState::CachedDisk).arg(key.toString()));
    return QImage();
}

void CacheManager::restoreDiskStates(const std::shared_ptr<Title> &title)
{
    if (!title || !cache_enabled_.load())
        return;
    if (titleCacheability(title) == TitleCacheability::NonCacheable) {
        /* Clock/ticker titles are dynamic by definition.  Remove stale cache
         * artifacts from older versions instead of publishing them as valid
         * disk states during session restore. */
        removeTitleCache(QString::fromStdString(title->id), true);
        return;
    }
    std::lock_guard<std::recursive_mutex> live_lock(live_cue_mutex_);
    const QString title_id = QString::fromStdString(title->id);
    const QString current_hash = contentHash(*title);
    const auto keys = disk_cache_.keysForTitle(title_id);
    for (const CacheFrameKey &key : keys) {
        if (key.content_hash == current_hash && key.width == title->width && key.height == title->height)
            state_tracker_.setState(key, FrameCacheState::CachedDisk);
    }

    ensure_live_text_row_ids(*title);
    for (int row = 0; row < (int)title->live_text_rows.size(); ++row) {
        const QString state_key = liveCueStateKey(title, row);
        const QString row_id = liveCueRowIdentity(title, row);
        QVector<CacheFrameKey> required_keys;
        QSet<CacheFrameKey> seen;
        for (const auto &variant : liveCueVariants(title, row)) {
            if (!variant)
                continue;
            for (const CacheFrameKey &frame_key : frameKeysForTitle(*variant)) {
                if (seen.contains(frame_key))
                    continue;
                seen.insert(frame_key);
                required_keys.push_back(frame_key);
                if (disk_cache_.contains(frame_key))
                    state_tracker_.setState(frame_key, FrameCacheState::CachedDisk);
            }
        }

        if (required_keys.isEmpty())
            continue;

        live_cue_required_keys_[state_key] = required_keys;
        live_cue_required_total_[state_key] = static_cast<int>(required_keys.size());
        live_cue_rows_[state_key] = row;
        live_cue_row_ids_[state_key] = row_id;
        live_cue_title_ids_[state_key] = title_id;
        live_cue_transition_state_keys_.remove(state_key);

        const FrameCacheState restored_state = liveCueStoredState(state_key);
        if (restored_state == FrameCacheState::CachedRam ||
            restored_state == FrameCacheState::CachedDisk) {
            live_cue_states_[state_key] = restored_state;
            live_cue_progress_percent_[state_key] = 100;
        } else {
            live_cue_required_keys_.remove(state_key);
            live_cue_required_total_.remove(state_key);
            live_cue_rows_.remove(state_key);
            live_cue_row_ids_.remove(state_key);
            live_cue_title_ids_.remove(state_key);
        }
    }
    /* Establish the editor's visual baseline even when every frame came from
     * disk. Without this, the first cue-list-only edit looked like an unknown
     * visual change and invalidateAll() discarded every live-cue state. */
    rememberVisualHash(*title, current_hash);
    emit diagnosticsChanged();
}

void CacheManager::queueFrame(const std::shared_ptr<Title> &title, double time,
                              RenderQueueManager::PriorityBand band)
{
    if (!title)
        return;
    queueFrameWithHash(title, time, band, contentHash(*title));
}

void CacheManager::queueFrameWithHash(const std::shared_ptr<Title> &title, double time,
                                      RenderQueueManager::PriorityBand band,
                                      const QString &known_content_hash)
{
    if (!title) return;
    if (interactive_bypass_.load() || !cache_enabled_.load() || titleCacheability(title) == TitleCacheability::NonCacheable)
        return;
    if (time < 0.0 || time > std::max(0.0, title->duration))
        return;
    const CacheFrameKey key = keyForTime(*title, time, known_content_hash);
    const FrameCacheState existing_state = state_tracker_.state(key);
    if (existing_state != FrameCacheState::Stale &&
        (cache_ram_frame_contains(ram_cache_, key) ||
         disk_cache_.contains(key)))
        return;
    if (existing_state == FrameCacheState::Rendering || queue_.contains(key))
        return;
    const int band_offset = (int)band * 100000;
    RenderQueueManager::Job job;
    job.key = key;
    job.title = snapshotForJob(title, key);
    job.time = timeForFrame(key.frame);
    job.priority = band_offset + std::abs(key.frame);
    job.force_render = existing_state == FrameCacheState::Stale;
    job.cache_epoch = cache_epoch_.load();
    job.title_generation = titleGeneration(key.title_id);
    if (queue_.enqueue(job)) {
        BGL_LOG_TRACE("Cache", QStringLiteral("Queued frame title=%1 frame=%2 time=%3 band=%4 key=%5")
                                   .arg(key.title_id)
                                   .arg(key.frame)
                                   .arg(timeForFrame(key.frame), 0, 'f', 3)
                                   .arg((int)band)
                                   .arg(key.toString()));
        state_tracker_.setState(key, FrameCacheState::Queued);
        wakeWorker();
    }
}

void CacheManager::queueRange(const std::shared_ptr<Title> &title, double start, double end, RenderQueueManager::PriorityBand band)
{
    if (!title) return;
    if (interactive_bypass_.load() || !cache_enabled_.load() || titleCacheability(title) == TitleCacheability::NonCacheable)
        return;
    if (end < start) std::swap(start, end);
    const int max_frame = cache_last_frame_for_title(*title, effectiveFrameRate());
    const int first = std::clamp(frameForTime(std::clamp(start, 0.0, title->duration)), 0, max_frame);
    const int last = std::clamp(frameForTime(std::clamp(end, 0.0, title->duration)), 0, max_frame);
    const QString content_hash = contentHash(*title);
    for (int frame = first; frame <= last; ++frame)
        queueFrameWithHash(title, timeForFrame(frame), band, content_hash);
}

void CacheManager::queueWholeTimeline(const std::shared_ptr<Title> &title)
{
    if (!title) return;
    if (interactive_bypass_.load() || !cache_enabled_.load() || titleCacheability(title) == TitleCacheability::NonCacheable)
        return;
    const int max_frame = cache_last_frame_for_title(*title, effectiveFrameRate());
    const QString title_id = QString::fromStdString(title->id);
    const int start_frame = last_reprioritize_title_id_ == title_id
        ? std::clamp(last_reprioritize_frame_, 0, max_frame)
        : 0;
    const QString content_hash = contentHash(*title);
    for (int frame = start_frame; frame <= max_frame; ++frame)
        queueFrameWithHash(title, timeForFrame(frame), RenderQueueManager::PriorityBand::FullTimeline, content_hash);
    for (int frame = 0; frame < start_frame; ++frame)
        queueFrameWithHash(title, timeForFrame(frame), RenderQueueManager::PriorityBand::FullTimeline, content_hash);
}

void CacheManager::queueWorkArea(const std::shared_ptr<Title> &title)
{
    if (!title) return;
    queueRange(title, title->loop_start, title->loop_end, RenderQueueManager::PriorityBand::WorkArea);
}

void CacheManager::reprioritize(const std::shared_ptr<Title> &title, double current_time)
{
    if (!title) return;
    if (interactive_bypass_.load() || !cache_enabled_.load() || titleCacheability(title) == TitleCacheability::NonCacheable)
        return;
    const int max_frame = cache_last_frame_for_title(*title, effectiveFrameRate());
    const int current = std::clamp(frameForTime(current_time), 0, max_frame);
    const QString title_id = QString::fromStdString(title->id);
    if (last_reprioritize_title_id_ == title_id && last_reprioritize_frame_ == current)
        return;
    last_reprioritize_title_id_ = title_id;
    last_reprioritize_frame_ = current;
    queue_.reprioritizeAround(title_id, current);
    const QString content_hash = contentHash(*title);
    queueFrameWithHash(title, current_time, RenderQueueManager::PriorityBand::Visible, content_hash);
    const int lookahead = std::min(6, std::max(2, (int)std::round(effectiveFrameRate() / 4.0)));
    const int lookbehind = 2;
    for (int i = 1; i <= lookahead; ++i)
        if (current + i <= max_frame)
            queueFrameWithHash(title, timeForFrame(current + i), RenderQueueManager::PriorityBand::AfterCurrent, content_hash);
    for (int i = 1; i <= lookbehind; ++i) {
        if (current - i >= 0)
            queueFrameWithHash(title, timeForFrame(current - i), RenderQueueManager::PriorityBand::BeforeCurrent, content_hash);
    }
}

void CacheManager::resetCancelledWorkState(const QString &title_id)
{
    state_tracker_.resetTransient(title_id);

    QVector<QPair<QString, int>> changed_rows;
    {
        std::lock_guard<std::recursive_mutex> live_lock(live_cue_mutex_);
        for (auto it = live_cue_states_.begin(); it != live_cue_states_.end(); ++it) {
            const QString state_key = it.key();
            if (!title_id.isEmpty() && live_cue_title_ids_.value(state_key) != title_id)
                continue;
            if (it.value() != FrameCacheState::Queued && it.value() != FrameCacheState::Rendering)
                continue;
            it.value() = FrameCacheState::NotCached;
            live_cue_progress_percent_[state_key] = liveCueStoredProgress(state_key);
            live_cue_last_emit_states_.remove(state_key);
            live_cue_last_emit_buckets_.remove(state_key);
            changed_rows.push_back({live_cue_title_ids_.value(state_key),
                                    live_cue_rows_.value(state_key, -1)});
        }
    }
    for (const auto &changed : changed_rows) {
        if (!changed.first.isEmpty() && changed.second >= 0)
            emit liveCueStateChanged(changed.first, changed.second);
    }
}

void CacheManager::clearRam()
{
    {
        std::lock_guard<std::recursive_mutex> publication_lock(publication_mutex_);
        cache_epoch_.fetch_add(1);
        queue_.clear();
        ram_cache_.clear();
        QMutexLocker dedup_lock(&temporal_dedup_mutex_);
        temporal_canonical_keys_.clear();
        adaptive_canonical_keys_.clear();
    }
    title_gpu_frame_cache_clear();
    resetCancelledWorkState();
    {
        std::lock_guard<std::recursive_mutex> live_lock(live_cue_mutex_);
        live_cue_known_keys_.clear();
    }
    state_tracker_.clear();
    wakeWorker();
    emit liveCueStateChanged(QString(), -1);
    emit diagnosticsChanged();
}

void CacheManager::clearDisk()
{
    {
        std::lock_guard<std::recursive_mutex> publication_lock(publication_mutex_);
        cache_epoch_.fetch_add(1);
        queue_.clear();
        QMutexLocker dedup_lock(&temporal_dedup_mutex_);
        temporal_canonical_keys_.clear();
        adaptive_canonical_keys_.clear();
    }
    disk_cache_.clearFast();
    resetCancelledWorkState();
    state_tracker_.clear();
    wakeWorker();
    emit liveCueStateChanged(QString(), -1);
    emit diagnosticsChanged();
}

void CacheManager::clearAll()
{
    {
        std::lock_guard<std::recursive_mutex> publication_lock(publication_mutex_);
        cache_epoch_.fetch_add(1);
        queue_.clear();
        ram_cache_.clear();
        QMutexLocker dedup_lock(&temporal_dedup_mutex_);
        temporal_canonical_keys_.clear();
        adaptive_canonical_keys_.clear();
    }
    title_gpu_frame_cache_clear();
    disk_cache_.clearFast();
    resetCancelledWorkState();
    state_tracker_.clear();
    {
        std::lock_guard<std::recursive_mutex> live_lock(live_cue_mutex_);
        live_cue_states_.clear();
        live_cue_progress_percent_.clear();
        live_cue_last_emit_states_.clear();
        live_cue_last_emit_buckets_.clear();
        live_cue_required_keys_.clear();
        live_cue_required_total_.clear();
        live_cue_rendered_since_emit_.clear();
        live_cue_total_by_key_.clear();
        live_cue_done_by_key_.clear();
        live_cue_rows_.clear();
        live_cue_row_ids_.clear();
        live_cue_title_ids_.clear();
        live_cue_transition_state_keys_.clear();
        live_cue_structure_row_ids_.clear();
        live_cue_row_fingerprints_.clear();
        live_cue_transition_signatures_.clear();
        live_cue_known_keys_.clear();
    }
    wakeWorker();
    emit liveCueStateChanged(QString(), -1);
    emit diagnosticsChanged();
}

void CacheManager::pausePrerender()
{
    paused_.store(true);
    wakeWorker();
}

void CacheManager::setCacheEnabled(bool enabled)
{
    if (cache_enabled_.load() == enabled)
        return;

    /* Invalidate every dequeued snapshot before changing externally visible
     * state. The worker may finish its local render, but jobIsCurrent() prevents
     * it from publishing into RAM/disk or touching live-cue state. */
    {
        std::lock_guard<std::recursive_mutex> publication_lock(publication_mutex_);
        cache_epoch_.fetch_add(1);
        queue_.setAcceptingJobs(false);
        cache_enabled_.store(enabled);
        paused_.store(!enabled);
        if (enabled)
            queue_.setAcceptingJobs(true);
    }
    resetCancelledWorkState();
    BGL_LOG_INFO("Cache", QStringLiteral("Cache enabled changed to %1").arg(enabled));
    TitlePreferences::set_cache_enabled(enabled);

    state_tracker_.clear();
    wakeWorker();
    emit cacheEnabledChanged(enabled);
    emit diagnosticsChanged();
}

void CacheManager::setInteractiveBypass(bool bypass)
{
    if (interactive_bypass_.load() == bypass)
        return;
    interactive_bypass_.store(bypass);
    BGL_LOG_DEBUG("Cache", QStringLiteral("Interactive bypass changed to %1").arg(bypass));
    /* Keep accepting live-cue work. The worker predicate pauses only normal
     * prerender jobs while the editor is manipulating the canvas. */
    queue_.setAcceptingJobs(cache_enabled_.load());
    wakeWorker();
}

void CacheManager::setEditorPrerenderFocus(const QString &title_id, bool active)
{
    {
        QMutexLocker lock(&editor_focus_mutex_);
        editor_focus_active_ = active && !title_id.isEmpty();
        editor_focus_title_id_ = editor_focus_active_ ? title_id : QString();
    }
    BGL_LOG_DEBUG("Cache", QStringLiteral("Editor prerender focus active=%1 title=%2")
                               .arg(active && !title_id.isEmpty())
                               .arg(title_id));
    wakeWorker();
}


void CacheManager::setRamCacheLimitMb(int megabytes)
{
    const int clamped = std::clamp(megabytes, 64, 32768);
    TitlePreferences::set_cache_ram_limit_mb(clamped);
    const quint64 bytes = (quint64)clamped * 1024ull * 1024ull;
    const quint64 compatibility_ram_budget = std::clamp<quint64>(
        bytes / 8ull,
        16ull * 1024ull * 1024ull,
        64ull * 1024ull * 1024ull);
    ram_cache_.setMaxBytes(compatibility_ram_budget);
    title_gpu_frame_cache_set_budget(
        bytes > compatibility_ram_budget
            ? bytes - compatibility_ram_budget
            : bytes);
    emit diagnosticsChanged();
}

void CacheManager::setDiskCacheLocation(const QString &path)
{
    const QString clean = QDir::cleanPath(path);
    if (clean.isEmpty())
        return;
    {
        std::lock_guard<std::recursive_mutex> publication_lock(publication_mutex_);
        cache_epoch_.fetch_add(1);
        queue_.clear();
        disk_cache_.setCacheDirectory(clean);
    }
    TitlePreferences::set_cache_disk_location(clean);
    resetCancelledWorkState();
    BGL_LOG_INFO("Cache", QStringLiteral("Disk cache location changed to %1").arg(clean));
    state_tracker_.clear();
    wakeWorker();
    emit diagnosticsChanged();
}

TitleCacheability CacheManager::titleCacheability(const std::shared_ptr<Title> &title) const
{
    if (!title)
        return TitleCacheability::NonCacheable;
    const TitleDynamicLayerAnalysis analysis = analyze_title_dynamic_layers(*title);
    if (!analysis.has_dynamic_layers)
        return TitleCacheability::Cacheable;
    return analysis.has_cacheable_prefix
        ? TitleCacheability::PartiallyCacheable
        : TitleCacheability::NonCacheable;
}

QString CacheManager::titleCacheabilityMessage(const std::shared_ptr<Title> &title) const
{
    if (!cache_enabled_.load())
        return bgl_tr("OBSTitles.CacheDisabledMessage");
    if (titleCacheability(title) == TitleCacheability::NonCacheable)
        return bgl_tr("OBSTitles.PrerenderDynamicUnavailable");
    return QString();
}

FrameCacheState CacheManager::displayStateForFrame(const std::shared_ptr<Title> &title, int frame) const
{
    if (!title)
        return FrameCacheState::NotCached;
    const QString title_id = QString::fromStdString(title->id);
    const int display_frame = titleHasTimelineChanges(*title)
        ? std::clamp(frame, 0, cache_last_frame_for_title(*title, effectiveFrameRate()))
        : 0;
    return state_tracker_.stateForFrame(title_id, display_frame);
}

bool CacheManager::displayFrameIsStatic(const std::shared_ptr<Title> &title, int frame) const
{
    if (!title)
        return false;
    if (!titleHasTimelineChanges(*title))
        return true;

    const int last_frame = cache_last_frame_for_title(*title, effectiveFrameRate());
    const int f = std::clamp(frame, 0, last_frame);
    const QString content_hash = contentHash(*title);
    const double fps = std::max(1.0, effectiveFrameRate());
    const QString current = adaptiveVisualStateHash(*title, double(f) / fps, content_hash);
    if (f > 0 && adaptiveVisualStateHash(*title, double(f - 1) / fps, content_hash) == current)
        return true;
    if (f < last_frame && adaptiveVisualStateHash(*title, double(f + 1) / fps, content_hash) == current)
        return true;
    return false;
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

static std::pair<double, double> animated_scalar_extents(
    const AnimatedProperty &property)
{
    double minimum = property.static_value;
    double maximum = property.static_value;
    for (const Keyframe &keyframe : property.keyframes) {
        minimum = std::min(minimum, keyframe.value);
        maximum = std::max(maximum, keyframe.value);
    }
    return {minimum, maximum};
}

static double animated_scalar_max_nonnegative(
    const AnimatedProperty &property, double fallback)
{
    const auto extents = animated_scalar_extents(property);
    return std::max({0.0, fallback, extents.second});
}

struct AnimatedVec2Extents {
    double min_x = 0.0;
    double max_x = 0.0;
    double min_y = 0.0;
    double max_y = 0.0;
};

static AnimatedVec2Extents animated_vec2_extents(
    const AnimatedVec2Property &property)
{
    AnimatedVec2Extents extents {
        property.static_value.x, property.static_value.x,
        property.static_value.y, property.static_value.y};
    for (const VectorKeyframe &keyframe : property.keyframes) {
        extents.min_x = std::min(extents.min_x, keyframe.value.x);
        extents.max_x = std::max(extents.max_x, keyframe.value.x);
        extents.min_y = std::min(extents.min_y, keyframe.value.y);
        extents.max_y = std::max(extents.max_y, keyframe.value.y);
    }
    return extents;
}

QRect CacheManager::layerDirtyRect(const Title &title, const Layer &layer) const
{
    const QSize frame_size(std::max(1, title.width), std::max(1, title.height));
    const QRect frame_rect(QPoint(0, 0), frame_size);
    if (!layer.visible || layer.live_cue_hidden_if_empty)
        return QRect();
    if (layer.type == LayerType::Adjustment || layer.type == LayerType::ColorSolid)
        return frame_rect;

    /* Dirty tiles are shared by every invalidated frame in the layer range, so
     * their bounds must cover the whole animated envelope rather than only the
     * static/default values. Bezier easing is clamped between keyframe values
     * by AnimatedProperty, making keyframe extrema conservative here. */
    const AnimatedVec2Extents position = animated_vec2_extents(layer.position);
    const AnimatedVec2Extents scale = animated_vec2_extents(layer.scale);
    const AnimatedVec2Extents size = animated_vec2_extents(layer.size);
    const double sx = std::max({0.01, std::abs(scale.min_x),
                                std::abs(scale.max_x)});
    const double sy = std::max({0.01, std::abs(scale.min_y),
                                std::abs(scale.max_y)});
    double w = std::max({1.0, std::abs(size.min_x),
                         std::abs(size.max_x)}) * sx;
    double h = std::max({1.0, std::abs(size.min_y),
                         std::abs(size.max_y)}) * sy;

    if (layer.type == LayerType::Text || layer.type == LayerType::Clock ||
        layer.type == LayerType::Ticker) {
        const double font_size = animated_scalar_max_nonnegative(
            layer.font_size_prop, std::max(1, layer.font_size));
        w = std::max(w, font_size *
                         std::max<size_t>(1, layer.text_content.size()) *
                         0.75 * sx);
        h = std::max(h, font_size * 1.6 * sy);
    } else if (layer.type == LayerType::Image && w <= 1.0 && h <= 1.0) {
        w = frame_size.width();
        h = frame_size.height();
    }

    double left = 8.0 + std::max(0.0f, layer.stroke_width);
    double right = left;
    double top = left;
    double bottom = left;
    const bool background_may_be_enabled = layer.background_enabled ||
        layer.background_enabled_prop.is_animated();
    if (background_may_be_enabled) {
        const double padding_x = animated_scalar_max_nonnegative(
            layer.background_padding_x_prop, layer.background_padding_x);
        const double padding_y = animated_scalar_max_nonnegative(
            layer.background_padding_y_prop, layer.background_padding_y);
        left += std::max(padding_x, animated_scalar_max_nonnegative(
            layer.background_padding_left_prop,
            layer.background_padding_left));
        right += std::max(padding_x, animated_scalar_max_nonnegative(
            layer.background_padding_right_prop,
            layer.background_padding_right));
        top += std::max(padding_y, animated_scalar_max_nonnegative(
            layer.background_padding_top_prop,
            layer.background_padding_top));
        bottom += std::max(padding_y, animated_scalar_max_nonnegative(
            layer.background_padding_bottom_prop,
            layer.background_padding_bottom));
        const double stroke = animated_scalar_max_nonnegative(
            layer.background_stroke_width_prop,
            layer.background_stroke_width);
        left += stroke;
        right += stroke;
        top += stroke;
        bottom += stroke;
    }

    for (const auto &effect : layer.effects) {
        if (!effect.enabled && !effect.enabled_prop.is_animated())
            continue;
        const double effect_size = animated_scalar_max_nonnegative(
            effect.size_prop, effect.effect_size);
        const double spread = animated_scalar_max_nonnegative(
            effect.spread_prop, effect.effect_spread);
        const double distance = animated_scalar_max_nonnegative(
            effect.distance_prop, effect.effect_distance);
        switch (effect.type) {
        case LayerEffectType::BackgroundColor:
            left += animated_scalar_max_nonnegative(
                effect.padding_left_prop, effect.effect_padding_left);
            right += animated_scalar_max_nonnegative(
                effect.padding_right_prop, effect.effect_padding_right);
            top += animated_scalar_max_nonnegative(
                effect.padding_top_prop, effect.effect_padding_top);
            bottom += animated_scalar_max_nonnegative(
                effect.padding_bottom_prop, effect.effect_padding_bottom);
            break;
        case LayerEffectType::Outline: {
            const double outline = animated_scalar_max_nonnegative(
                effect.stroke_width_prop, effect.effect_stroke_width);
            left += outline;
            right += outline;
            top += outline;
            bottom += outline;
            break;
        }
        case LayerEffectType::DropShadow:
        case LayerEffectType::LongShadow: {
            const double halo = effect_size + spread;
            if (effect.angle_prop.is_animated()) {
                /* An animated direction can visit either side of the layer. */
                left += halo + distance;
                right += halo + distance;
                top += halo + distance;
                bottom += halo + distance;
            } else {
                const double angle = effect.angle_prop.is_animated()
                    ? effect.angle_prop.static_value
                    : effect.effect_angle;
                const double radians = angle *
                    3.14159265358979323846 / 180.0;
                const double dx = std::cos(radians) * distance;
                const double dy = std::sin(radians) * distance;
                left += halo + std::max(0.0, -dx);
                right += halo + std::max(0.0, dx);
                top += halo + std::max(0.0, -dy);
                bottom += halo + std::max(0.0, dy);
            }
            break;
        }
        case LayerEffectType::Glow:
        case LayerEffectType::Bloom:
        case LayerEffectType::Blur:
        case LayerEffectType::MotionBlur: {
            const double halo = effect_size + spread +
                (effect.type == LayerEffectType::MotionBlur ? distance : 0.0);
            left += halo;
            right += halo;
            top += halo;
            bottom += halo;
            break;
        }
        case LayerEffectType::Emboss:
            left += effect_size;
            right += effect_size;
            top += effect_size;
            bottom += effect_size;
            break;
        case LayerEffectType::LensFlare:
        case LayerEffectType::Vignette:
        case LayerEffectType::Noise:
        case LayerEffectType::RoughenEdges:
            return frame_rect;
        case LayerEffectType::InnerGlow:
        case LayerEffectType::InnerShadow:
        case LayerEffectType::BrightnessContrast:
        case LayerEffectType::Saturation:
        case LayerEffectType::ColorOverlay:
            break;
        }
    }

    const AnimatedVec2Extents origin = animated_vec2_extents(layer.origin_prop);
    const double geometry_left = std::max(std::abs(origin.min_x),
                                          std::abs(origin.max_x)) * w;
    const double geometry_right = std::max(std::abs(1.0 - origin.min_x),
                                           std::abs(1.0 - origin.max_x)) * w;
    const double geometry_top = std::max(std::abs(origin.min_y),
                                         std::abs(origin.max_y)) * h;
    const double geometry_bottom = std::max(std::abs(1.0 - origin.min_y),
                                            std::abs(1.0 - origin.max_y)) * h;
    const double local_left = geometry_left + left;
    const double local_right = geometry_right + right;
    const double local_top = geometry_top + top;
    const double local_bottom = geometry_bottom + bottom;

    if (layer.rotation.is_animated()) {
        /* Rotation about an arbitrary animated origin is cheap to invalidate as
         * a full frame and avoids underestimating a swept corner. The readback
         * policy may still choose a region for other layers/titles. */
        return frame_rect;
    }

    double rotated_min_x = -local_left;
    double rotated_max_x = local_right;
    double rotated_min_y = -local_top;
    double rotated_max_y = local_bottom;
    if (std::fmod(std::abs(layer.rotation.static_value), 360.0) > 0.001) {
        const double radians = layer.rotation.static_value *
            3.14159265358979323846 / 180.0;
        const double cosine = std::cos(radians);
        const double sine = std::sin(radians);
        rotated_min_x = std::numeric_limits<double>::max();
        rotated_max_x = std::numeric_limits<double>::lowest();
        rotated_min_y = std::numeric_limits<double>::max();
        rotated_max_y = std::numeric_limits<double>::lowest();
        for (double corner_x : {-local_left, local_right}) {
            for (double corner_y : {-local_top, local_bottom}) {
                const double transformed_x = corner_x * cosine - corner_y * sine;
                const double transformed_y = corner_x * sine + corner_y * cosine;
                rotated_min_x = std::min(rotated_min_x, transformed_x);
                rotated_max_x = std::max(rotated_max_x, transformed_x);
                rotated_min_y = std::min(rotated_min_y, transformed_y);
                rotated_max_y = std::max(rotated_max_y, transformed_y);
            }
        }
    }

    const QRectF rect(
        position.min_x + rotated_min_x,
        position.min_y + rotated_min_y,
        (position.max_x - position.min_x) +
            (rotated_max_x - rotated_min_x),
        (position.max_y - position.min_y) +
            (rotated_max_y - rotated_min_y));
    return rect.toAlignedRect().intersected(frame_rect);
}

QString CacheManager::tileStateKey(const QString &title_id, int frame) const
{
    return title_id + QLatin1Char(':') + QString::number(frame);
}

void CacheManager::markDirtyTiles(const QString &title_id, int first_frame, int last_frame, const QVector<CacheTileRegion> &tiles)
{
    if (title_id.isEmpty() || first_frame > last_frame || tiles.isEmpty())
        return;
    QMutexLocker lock(&dirty_tiles_mutex_);
    for (int frame = first_frame; frame <= last_frame; ++frame) {
        QSet<QString> &set = dirty_tiles_by_frame_[tileStateKey(title_id, frame)];
        for (const auto &tile : tiles)
            set.insert(QString::number(tile.tile_x) + QLatin1Char(',') + QString::number(tile.tile_y));
    }
}

QVector<CacheTileRegion> CacheManager::dirtyTilesForKey(const CacheFrameKey &key) const
{
    QVector<CacheTileRegion> regions;
    const QSize frame_size(std::max(1, key.width), std::max(1, key.height));
    QSet<QString> ids;
    {
        QMutexLocker lock(&dirty_tiles_mutex_);
        ids = dirty_tiles_by_frame_.value(tileStateKey(key.title_id, key.frame));
    }
    for (const QString &id : ids) {
        const QStringList parts = id.split(QLatin1Char(','));
        if (parts.size() != 2) continue;
        bool ok_x = false, ok_y = false;
        const int tx = parts[0].toInt(&ok_x);
        const int ty = parts[1].toInt(&ok_y);
        if (!ok_x || !ok_y) continue;
        const QRect tile_rect(tx * kCacheTileSize, ty * kCacheTileSize, kCacheTileSize, kCacheTileSize);
        regions.push_back({tx, ty, tile_rect.intersected(QRect(QPoint(0, 0), frame_size))});
    }
    return regions;
}

static bool effect_requires_full_gpu_cache_frame(LayerEffectType type)
{
    switch (type) {
    case LayerEffectType::DropShadow:
    case LayerEffectType::LongShadow:
    case LayerEffectType::Glow:
    case LayerEffectType::InnerGlow:
    case LayerEffectType::InnerShadow:
    case LayerEffectType::Blur:
    case LayerEffectType::MotionBlur:
    case LayerEffectType::Bloom:
    case LayerEffectType::Emboss:
    case LayerEffectType::LensFlare:
    case LayerEffectType::Vignette:
    case LayerEffectType::Noise:
    case LayerEffectType::RoughenEdges:
        return true;
    default:
        return false;
    }
}

static bool title_requires_full_gpu_cache_frame(const Title &title)
{
    for (const auto &layer : title.layers) {
        if (!layer)
            continue;
        if (layer->type == LayerType::Adjustment ||
            layer->type == LayerType::ColorSolid ||
            !layer->parent_id.empty() ||
            layer->mask_mode != MaskMode::None ||
            layer->use_as_scene_mask)
            return true;
        for (const LayerEffect &effect : layer->effects) {
            if ((effect.enabled || effect.enabled_prop.is_animated()) &&
                effect_requires_full_gpu_cache_frame(effect.type))
                return true;
        }
        for (const LayerTransition &transition : layer->transitions) {
            if (!transition.enabled || transition.kind != LayerTransitionKind::General)
                continue;
            switch (transition.type) {
            case LayerTransitionType::OpacityBlur:
            case LayerTransitionType::ZoomBlur:
            case LayerTransitionType::BlurSlide:
                if (transition.blur_amount > 0.01)
                    return true;
                break;
            case LayerTransitionType::Blocks:
            case LayerTransitionType::ImageWipe:
            case LayerTransitionType::Clock:
            case LayerTransitionType::Iris:
            case LayerTransitionType::GradientWipe:
                return true;
            default:
                break;
            }
        }
    }
    return false;
}


void CacheManager::rememberVisualHash(const Title &title, const QString &known_hash)
{
    const QString title_id = QString::fromStdString(title.id);
    const QString visual_hash = known_hash.isEmpty() ? contentHash(title) : known_hash;
    QMutexLocker lock(&visual_hash_mutex_);
    if (last_visual_hash_by_title_.value(title_id) == visual_hash &&
        last_layer_rects_by_title_.contains(title_id))
        return;

    last_visual_hash_by_title_[title_id] = visual_hash;
    QHash<QString, QRect> rects;
    for (const auto &layer : title.layers) {
        if (layer)
            rects[QString::fromStdString(layer->id)] = layerDirtyRect(title, *layer);
    }
    last_layer_rects_by_title_[title_id] = rects;
}

bool CacheManager::visualHashUnchanged(const Title &title) const
{
    QMutexLocker lock(&visual_hash_mutex_);
    const QString title_id = QString::fromStdString(title.id);
    return last_visual_hash_by_title_.contains(title_id) &&
           last_visual_hash_by_title_.value(title_id) == contentHash(title);
}

void CacheManager::resumePrerender()
{
    paused_.store(false);
    wakeWorker();
}

void CacheManager::invalidateAll(const std::shared_ptr<Title> &title)
{
    if (!title) return;
    if (titleCacheability(title) == TitleCacheability::NonCacheable) {
        removeTitleCache(QString::fromStdString(title->id), true);
        return;
    }
    if (visualHashUnchanged(*title)) {
        BGL_LOG_TRACE("Cache", QStringLiteral("Skipped frame-cache invalidation for unchanged rendered title=%1; refreshing live-cue structure selectively")
                                      .arg(QString::fromStdString(title->id)));
        /* Structural cue-list edits do not change the title's currently rendered
         * pixels. Clearing every live-cue state here defeated stable row IDs and
         * forced all rows to render again after add/remove/reorder. Let the
         * structure reconciler invalidate only added/removed/edited row states. */
        refreshLiveCueStructure(title);
        return;
    }
    const QString title_id = QString::fromStdString(title->id);
    /* invalidateAll() cancels/marks the previous generation stale. A same-frame
     * reprioritize must therefore not be suppressed by the old UI sentinel. */
    if (last_reprioritize_title_id_ == title_id) {
        last_reprioritize_title_id_.clear();
        last_reprioritize_frame_ = -1;
    }
    const QSize frame_size(std::max(1, title->width), std::max(1, title->height));
    markDirtyTiles(title_id, 0, cache_last_frame_for_title(*title, effectiveFrameRate()),
                   tilesForRect(QRect(QPoint(0, 0), frame_size), frame_size));
    invalidation_.invalidateAll(title);
    invalidateLiveCues(title);
}

void CacheManager::invalidateRange(const std::shared_ptr<Title> &title, double start, double end)
{
    if (!title) return;
    if (visualHashUnchanged(*title)) {
        BGL_LOG_TRACE("Cache", QStringLiteral("Skipped cache range invalidation for unchanged title=%1")
                                      .arg(QString::fromStdString(title->id)));
        return;
    }
    const int max_frame = cache_last_frame_for_title(*title, effectiveFrameRate());
    if (end < start) std::swap(start, end);
    const int first = std::clamp(frameForTime(std::clamp(start, 0.0, title->duration)), 0, max_frame);
    const int last = std::clamp(frameForTime(std::clamp(end, 0.0, title->duration)), 0, max_frame);
    const QSize frame_size(std::max(1, title->width), std::max(1, title->height));
    markDirtyTiles(QString::fromStdString(title->id), first, last,
                   tilesForRect(QRect(QPoint(0, 0), frame_size), frame_size));
    invalidation_.invalidateRange(title, start, end);
}

void CacheManager::invalidateLayer(const std::shared_ptr<Title> &title, const std::string &layer_id)
{
    if (!title) return;
    if (visualHashUnchanged(*title)) {
        BGL_LOG_TRACE("Cache", QStringLiteral("Skipped layer invalidation for unchanged title=%1 layer=%2")
                                      .arg(QString::fromStdString(title->id), QString::fromStdString(layer_id)));
        return;
    }
    if (auto layer = title->find_layer(layer_id)) {
        const int max_frame = cache_last_frame_for_title(*title, effectiveFrameRate());
        const int first = std::clamp(frameForTime(std::clamp(layer->in_time, 0.0, title->duration)), 0, max_frame);
        const int last = std::clamp(frameForTime(std::clamp(layer->out_time, 0.0, title->duration)), 0, max_frame);
        const QSize frame_size(std::max(1, title->width), std::max(1, title->height));
        QRect dirty_rect = layerDirtyRect(*title, *layer);
        {
            QMutexLocker lock(&visual_hash_mutex_);
            dirty_rect = dirty_rect.united(last_layer_rects_by_title_
                .value(QString::fromStdString(title->id))
                .value(QString::fromStdString(layer_id)));
        }
        markDirtyTiles(QString::fromStdString(title->id), first, last, tilesForRect(dirty_rect, frame_size));
    }
    invalidation_.invalidateLayer(title, layer_id);
}

std::shared_ptr<Title> CacheManager::titleWithCueApplied(const std::shared_ptr<Title> &title, int row) const
{
    std::lock_guard<std::recursive_mutex> live_lock(live_cue_mutex_);
    if (!title || row < 0 || row >= static_cast<int>(title->live_text_rows.size()))
        return nullptr;

    auto cue_title = clone_title_snapshot(title);
    if (!cue_title)
        return nullptr;

    std::vector<std::shared_ptr<Layer>> exposed;
    for (const auto &layer : cue_title->layers) {
        if (!layer) continue;
        if ((layer->type == LayerType::Text || layer->type == LayerType::Ticker || layer->type == LayerType::Image) &&
            layer->expose_text)
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

    for (int col = 0; col < static_cast<int>(exposed.size()) &&
         col < static_cast<int>(cue_title->live_text_rows[row].size()); ++col) {
        auto &target = exposed[col];
        if (!target)
            continue;
        const int value_row = target->exposed_single_value ? 0 : row;
        if (value_row < 0 || value_row >= static_cast<int>(cue_title->live_text_rows.size()) ||
            col >= static_cast<int>(cue_title->live_text_rows[value_row].size()))
            continue;
        const std::string cue_value = cue_title->live_text_rows[value_row][col];
        target->live_cue_hidden_if_empty = target->exposed_hide_if_empty && cue_value.empty();
        if (target->type == LayerType::Image) {
            bgs::apply_exposed_image_cue_value(*target, cue_value);
            continue;
        }
        target->text_content = cue_value;
        if (target->rich_text.empty())
            target->rich_text = rich_text_document_from_layer_defaults(*target);
        RichTextCharFormat insertion_format =
            rich_text_effective_typing_format(target->rich_text);
        rich_text_document_replace_text(target->rich_text, cue_value, insertion_format,
                                        target->rich_text.has_typing_format
                                            ? target->rich_text.typing_format_mask : 0);
    }

    /* Steady cue titles are deterministic. Runtime current/pending state must
     * never leak into their cache identity, otherwise adding/removing a row
     * changes every existing row's key. Stateful persistence transitions are
     * represented by separate from->to cache states below. */
    cue_title->current_cue_row = row;
    cue_title->pending_cue_row = -1;
    cue_title->cue_persistence_transition = false;
    cue_title->cue_persistent_text_columns.clear();
    return cue_title;
}


QVector<std::shared_ptr<Title>> CacheManager::liveCueVariants(const std::shared_ptr<Title> &title, int row) const
{
    std::lock_guard<std::recursive_mutex> live_lock(live_cue_mutex_);
    QVector<std::shared_ptr<Title>> variants;
    if (auto steady = titleWithCueApplied(title, row))
        variants.push_back(steady);
    return variants;
}

QVector<std::shared_ptr<Title>> CacheManager::liveCueTransitionVariants(
    const std::shared_ptr<Title> &title, int from_row, int to_row) const
{
    std::lock_guard<std::recursive_mutex> live_lock(live_cue_mutex_);
    QVector<std::shared_ptr<Title>> variants;
    if (!title || from_row < 0 || to_row < 0 || from_row == to_row ||
        from_row >= static_cast<int>(title->live_text_rows.size()) ||
        to_row >= static_cast<int>(title->live_text_rows.size()))
        return variants;

    if (title->playback_mode != 1 && title->playback_mode != 2 && !title->cue_background_persistence)
        return variants;

    auto persistent_columns = [&](const std::shared_ptr<Title> &candidate) {
        std::vector<bool> result;
        if (!candidate || !candidate->cue_text_persistence)
            return result;
        std::vector<std::shared_ptr<Layer>> exposed;
        for (const auto &layer : candidate->layers) {
            if (layer && (layer->type == LayerType::Text || layer->type == LayerType::Ticker || layer->type == LayerType::Image) &&
                layer->expose_text)
                exposed.push_back(layer);
        }
        if (!candidate->live_text_column_order.empty()) {
            std::vector<std::shared_ptr<Layer>> ordered;
            ordered.reserve(exposed.size());
            for (const auto &layer_id : candidate->live_text_column_order) {
                auto it = std::find_if(exposed.begin(), exposed.end(),
                                       [&](const std::shared_ptr<Layer> &layer) {
                                           return layer && layer->id == layer_id;
                                       });
                if (it != exposed.end())
                    ordered.push_back(*it);
            }
            for (const auto &layer : exposed) {
                auto it = std::find_if(ordered.begin(), ordered.end(),
                                       [&](const std::shared_ptr<Layer> &candidate_layer) {
                                           return candidate_layer && layer && candidate_layer->id == layer->id;
                                       });
                if (it == ordered.end())
                    ordered.push_back(layer);
            }
            exposed = std::move(ordered);
        }
        result.assign(exposed.size(), false);
        for (size_t col = 0; col < result.size(); ++col) {
            const auto &layer = exposed[col];
            const int from_value_row = layer && layer->exposed_single_value ? 0 : from_row;
            const int to_value_row = layer && layer->exposed_single_value ? 0 : to_row;
            if (from_value_row < 0 || to_value_row < 0 ||
                from_value_row >= static_cast<int>(title->live_text_rows.size()) ||
                to_value_row >= static_cast<int>(title->live_text_rows.size()) ||
                col >= title->live_text_rows[static_cast<size_t>(from_value_row)].size() ||
                col >= title->live_text_rows[static_cast<size_t>(to_value_row)].size())
                continue;
            result[col] = title->live_text_rows[static_cast<size_t>(from_value_row)][col] ==
                          title->live_text_rows[static_cast<size_t>(to_value_row)][col];
        }
        return result;
    };

    /* Outgoing phase: the previous cue remains applied while the title plays
     * its outro. pending_cue_row is control state only; rendered identity comes
     * from the applied previous-row pixels and persistence column mask. */
    if (auto outgoing = titleWithCueApplied(title, from_row)) {
        outgoing->current_cue_row = from_row;
        outgoing->pending_cue_row = to_row;
        outgoing->cue_persistence_transition = title->cue_background_persistence;
        outgoing->cue_persistent_text_columns = persistent_columns(outgoing);
        variants.push_back(outgoing);
    }

    /* Incoming phase: target cue text is applied, while persistent background
     * layers (and unchanged text columns) are held exactly as in live playback. */
    if (auto incoming = titleWithCueApplied(title, to_row)) {
        incoming->current_cue_row = to_row;
        incoming->pending_cue_row = -1;
        incoming->cue_persistence_transition = title->cue_background_persistence;
        incoming->cue_persistent_text_columns = persistent_columns(incoming);
        variants.push_back(incoming);
    }
    return variants;
}


int CacheManager::liveCueVariantsProgress(const QVector<std::shared_ptr<Title>> &variants) const
{
    std::lock_guard<std::recursive_mutex> live_lock(live_cue_mutex_);
    if (variants.isEmpty())
        return 0;
    int total_frames = 0;
    int cached_frames = 0;
    QSet<CacheFrameKey> seen;
    for (const auto &variant : variants) {
        if (!variant)
            continue;
        for (const CacheFrameKey &frame_key : frameKeysForTitle(*variant)) {
            if (seen.contains(frame_key))
                continue;
            seen.insert(frame_key);
            ++total_frames;
            if (cache_ram_frame_contains(ram_cache_, frame_key) ||
                disk_cache_.contains(frame_key))
                ++cached_frames;
        }
    }
    if (total_frames <= 0)
        return 0;
    return std::clamp((cached_frames * 100) / total_frames, 0, 100);
}


int CacheManager::liveCueStoredProgress(const QString &state_key) const
{
    std::lock_guard<std::recursive_mutex> live_lock(live_cue_mutex_);
    /* A manual rebuild is a new rendering generation even when its
     * content-addressed frames are still resident from the previous one.
     * Report generation completion, not old RAM/SSD residency, otherwise the
     * badge stays at 100% and oscillates between Rendering and CachedRam. */
    if (live_cue_total_by_key_.contains(state_key)) {
        const int total = std::max(1, live_cue_total_by_key_.value(state_key));
        const int done = std::clamp(live_cue_done_by_key_.value(state_key, 0), 0, total);
        return std::clamp((done * 100) / total, 0, 100);
    }
    const auto keys = live_cue_required_keys_.value(state_key);
    if (keys.isEmpty())
        return live_cue_progress_percent_.value(state_key, 0);

    int cached = 0;
    for (const CacheFrameKey &frame_key : keys) {
        if (cache_ram_frame_contains(ram_cache_, frame_key) ||
            disk_cache_.contains(frame_key))
            ++cached;
    }
    const int total = std::max(1, static_cast<int>(keys.size()));
    return std::clamp((cached * 100) / total, 0, 100);
}

FrameCacheState CacheManager::liveCueStoredState(const QString &state_key) const
{
    std::lock_guard<std::recursive_mutex> live_lock(live_cue_mutex_);
    const auto keys = live_cue_required_keys_.value(state_key);
    if (keys.isEmpty())
        return live_cue_states_.value(state_key, FrameCacheState::NotCached);

    const FrameCacheState explicit_state = live_cue_states_.value(state_key, FrameCacheState::NotCached);
    if (live_cue_total_by_key_.contains(state_key) &&
        live_cue_done_by_key_.value(state_key, 0) < live_cue_total_by_key_.value(state_key)) {
        return explicit_state == FrameCacheState::Rendering
            ? FrameCacheState::Rendering
            : FrameCacheState::Queued;
    }
    bool any_disk = false;
    bool any_rendering = explicit_state == FrameCacheState::Rendering;
    bool any_queued = explicit_state == FrameCacheState::Queued;
    bool any_stale = explicit_state == FrameCacheState::Stale;
    int cached = 0;

    for (const CacheFrameKey &frame_key : keys) {
        const FrameCacheState frame_state = state_tracker_.state(frame_key);
        if (frame_state == FrameCacheState::Rendering)
            any_rendering = true;
        if (frame_state == FrameCacheState::Queued || queue_.contains(frame_key))
            any_queued = true;
        if (frame_state == FrameCacheState::Stale)
            any_stale = true;
        if (cache_ram_frame_contains(ram_cache_, frame_key)) {
            ++cached;
        } else if (disk_cache_.contains(frame_key)) {
            ++cached;
            any_disk = true;
        }
    }

    if (any_rendering) return FrameCacheState::Rendering;
    /* Shared content-addressed frames can complete through another cue state.
     * Full residency is authoritative even if this state's advisory Queued flag
     * has not yet received its own completion callback. */
    if (cached >= static_cast<int>(keys.size())) return any_disk ? FrameCacheState::CachedDisk : FrameCacheState::CachedRam;
    if (any_queued) return FrameCacheState::Queued;
    if (cached > 0) return any_stale ? FrameCacheState::Stale : FrameCacheState::NotCached;
    if (any_stale) return FrameCacheState::Stale;
    return FrameCacheState::NotCached;
}

bool CacheManager::shouldEmitLiveCueUpdate(const QString &state_key, FrameCacheState state, int progress) const
{
    std::lock_guard<std::recursive_mutex> live_lock(live_cue_mutex_);
    const int bucket = progress >= 100 ? 100 : (progress / 25) * 25;

    /* Queue-to-worker handoff must not be treated as a visible state change.
     * Both states use the same four-stage progress presentation; emitting on
     * every Queued <-> Rendering transition caused needless icon replacement
     * and orange/yellow flicker while the percentage stayed in one bucket. */
    const FrameCacheState visual_state =
        (state == FrameCacheState::Queued || state == FrameCacheState::Rendering)
            ? FrameCacheState::Rendering
            : state;
    const bool changed = !live_cue_last_emit_states_.contains(state_key) ||
        live_cue_last_emit_states_.value(state_key) != visual_state ||
        live_cue_last_emit_buckets_.value(state_key, -1) != bucket;
    if (changed) {
        live_cue_last_emit_states_[state_key] = visual_state;
        live_cue_last_emit_buckets_[state_key] = bucket;
    }
    return changed;
}

bool CacheManager::isLiveCueKeyReferenced(const CacheFrameKey &key) const
{
    std::lock_guard<std::recursive_mutex> live_lock(live_cue_mutex_);
    return !liveCueStateReferencingKey(key).isEmpty();
}

QString CacheManager::liveCueStateReferencingKey(const CacheFrameKey &key,
                                                  const QString &preferred_state) const
{
    std::lock_guard<std::recursive_mutex> live_lock(live_cue_mutex_);
    auto contains_key = [&](const QString &state_key) {
        const QVector<CacheFrameKey> required = live_cue_required_keys_.value(state_key);
        return !required.isEmpty() &&
            std::find(required.cbegin(), required.cend(), key) != required.cend();
    };
    if (!preferred_state.isEmpty() && contains_key(preferred_state))
        return preferred_state;
    for (auto it = live_cue_required_keys_.constBegin(); it != live_cue_required_keys_.constEnd(); ++it) {
        if (live_cue_title_ids_.value(it.key()) != key.title_id)
            continue;
        if (std::find(it.value().cbegin(), it.value().cend(), key) != it.value().cend())
            return it.key();
    }
    return {};
}

void CacheManager::removeLiveCueState(const QString &state_key)
{
    std::lock_guard<std::recursive_mutex> live_lock(live_cue_mutex_);
    live_cue_states_.remove(state_key);
    live_cue_progress_percent_.remove(state_key);
    live_cue_last_emit_states_.remove(state_key);
    live_cue_last_emit_buckets_.remove(state_key);
    live_cue_required_keys_.remove(state_key);
    live_cue_required_total_.remove(state_key);
    live_cue_rendered_since_emit_.remove(state_key);
    live_cue_total_by_key_.remove(state_key);
    live_cue_done_by_key_.remove(state_key);
    live_cue_rows_.remove(state_key);
    live_cue_row_ids_.remove(state_key);
    live_cue_title_ids_.remove(state_key);
    live_cue_transition_state_keys_.remove(state_key);
}

void CacheManager::pruneUnreferencedLiveCueRam(const QString &title_id)
{
    std::lock_guard<std::recursive_mutex> live_lock(live_cue_mutex_);
    QSet<CacheFrameKey> referenced;
    for (auto it = live_cue_required_keys_.constBegin(); it != live_cue_required_keys_.constEnd(); ++it) {
        if (live_cue_title_ids_.value(it.key()) != title_id)
            continue;
        for (const CacheFrameKey &key : it.value())
            referenced.insert(key);
    }

    /* Inspect every key ever registered by the live-cue cache, not just frames
     * currently resident in RAM. Otherwise obsolete queued jobs continue to
     * render after add/remove/edit, then repopulate RAM with superseded frames. */
    QSet<CacheFrameKey> candidates;
    for (const CacheFrameKey &key : live_cue_known_keys_) {
        if (key.title_id == title_id)
            candidates.insert(key);
    }
    for (const CacheFrameKey &key : ram_cache_.keysForTitle(title_id))
        candidates.insert(key);

    int cancelled_jobs = 0;
    int removed_ram = 0;
    int removed_keys = 0;
    for (const CacheFrameKey &key : candidates) {
        if (referenced.contains(key))
            continue;
        if (queue_.cancelKey(key))
            ++cancelled_jobs;
        const bool gpu_resident = gpu_ram_frame_contains(key);
        QImage resident;
        const bool cpu_resident = ram_cache_.get(key, resident);
        if (gpu_resident)
            title_gpu_frame_cache_remove(key.toString().toStdString());
        if (cpu_resident)
            ram_cache_.remove(key);
        if (gpu_resident || cpu_resident)
            ++removed_ram;
        if (disk_cache_.contains(key))
            state_tracker_.setState(key, FrameCacheState::CachedDisk);
        else
            state_tracker_.setState(key, FrameCacheState::NotCached);
        live_cue_known_keys_.remove(key);
        ++removed_keys;
    }

    if (removed_keys > 0) {
        BGL_LOG_INFO("LiveCue", QStringLiteral("Pruned superseded live cue work title=%1 keys=%2 ramFrames=%3 queuedJobs=%4")
                                  .arg(title_id)
                                  .arg(removed_keys)
                                  .arg(removed_ram)
                                  .arg(cancelled_jobs));
        emit diagnosticsChanged();
    }
}

FrameCacheState CacheManager::liveCueVariantsState(const QVector<std::shared_ptr<Title>> &variants, const QString &state_key) const
{
    std::lock_guard<std::recursive_mutex> live_lock(live_cue_mutex_);
    if (variants.isEmpty())
        return FrameCacheState::NotCached;

    const FrameCacheState explicit_state = live_cue_states_.value(state_key, FrameCacheState::NotCached);
    bool any_disk = false;
    bool any_rendering = explicit_state == FrameCacheState::Rendering;
    bool any_queued = explicit_state == FrameCacheState::Queued;
    bool any_stale = explicit_state == FrameCacheState::Stale;
    int total = 0;
    int cached = 0;

    QSet<CacheFrameKey> seen;
    for (const auto &variant : variants) {
        if (!variant)
            continue;
        for (const CacheFrameKey &frame_key : frameKeysForTitle(*variant)) {
            if (seen.contains(frame_key))
                continue;
            seen.insert(frame_key);
            ++total;
            const FrameCacheState frame_state = state_tracker_.state(frame_key);
            if (frame_state == FrameCacheState::Rendering)
                any_rendering = true;
            if (frame_state == FrameCacheState::Queued || queue_.contains(frame_key))
                any_queued = true;
            if (frame_state == FrameCacheState::Stale)
                any_stale = true;
            if (cache_ram_frame_contains(ram_cache_, frame_key)) {
                ++cached;
            } else if (disk_cache_.contains(frame_key)) {
                ++cached;
                any_disk = true;
            }
        }
    }

    if (any_rendering)
        return FrameCacheState::Rendering;
    if (total > 0 && cached >= total)
        return any_disk ? FrameCacheState::CachedDisk : FrameCacheState::CachedRam;
    if (any_queued)
        return FrameCacheState::Queued;
    if (cached > 0)
        return any_stale ? FrameCacheState::Stale : FrameCacheState::NotCached;
    if (any_stale)
        return FrameCacheState::Stale;
    return FrameCacheState::NotCached;
}

CacheFrameKey CacheManager::liveCueKey(const std::shared_ptr<Title> &title, int row) const
{
    auto cue_title = titleWithCueApplied(title, row);
    if (!cue_title) return {};
    return keyForTime(*cue_title, std::clamp(cue_title->pause_time, 0.0, cue_title->duration));
}

QString CacheManager::liveCueRowIdentity(const std::shared_ptr<Title> &title, int row) const
{
    if (!title)
        return {};
    return QString::fromStdString(live_text_row_id(*title, row));
}

QString CacheManager::liveCueStateKey(const std::shared_ptr<Title> &title, int row) const
{
    std::lock_guard<std::recursive_mutex> live_lock(live_cue_mutex_);
    if (!title)
        return {};
    return QStringLiteral("%1:live-cue:%2")
        .arg(QString::fromStdString(title->id))
        .arg(liveCueRowIdentity(title, row));
}

QString CacheManager::liveCueTransitionStateKey(const std::shared_ptr<Title> &title,
                                                int from_row, int to_row) const
{
    if (!title)
        return {};
    return QStringLiteral("%1:live-transition:%2:%3")
        .arg(QString::fromStdString(title->id))
        .arg(liveCueRowIdentity(title, from_row))
        .arg(liveCueRowIdentity(title, to_row));
}

int CacheManager::liveCueRangeProgress(const std::shared_ptr<Title> &cue_title) const
{
    std::lock_guard<std::recursive_mutex> live_lock(live_cue_mutex_);
    if (!cue_title)
        return 0;
    const auto keys = frameKeysForTitle(*cue_title);
    const int total = std::max(1, static_cast<int>(keys.size()));
    int cached = 0;
    for (const CacheFrameKey &frame_key : keys) {
        if (cache_ram_frame_contains(ram_cache_, frame_key) ||
            disk_cache_.contains(frame_key))
            ++cached;
    }
    return std::clamp((cached * 100) / total, 0, 100);
}

FrameCacheState CacheManager::liveCueRangeState(const std::shared_ptr<Title> &cue_title, const QString &state_key) const
{
    std::lock_guard<std::recursive_mutex> live_lock(live_cue_mutex_);
    if (!cue_title)
        return FrameCacheState::NotCached;

    const FrameCacheState explicit_state = live_cue_states_.value(state_key, FrameCacheState::NotCached);
    const auto keys = frameKeysForTitle(*cue_title);
    const int total = std::max(1, static_cast<int>(keys.size()));
    int cached = 0;
    bool any_disk = false;
    bool any_rendering = false;
    bool any_queued = false;
    bool any_stale = explicit_state == FrameCacheState::Stale;

    for (const CacheFrameKey &frame_key : keys) {
        const FrameCacheState frame_state = state_tracker_.state(frame_key);
        if (frame_state == FrameCacheState::Rendering)
            any_rendering = true;
        if (frame_state == FrameCacheState::Queued || queue_.contains(frame_key))
            any_queued = true;
        if (frame_state == FrameCacheState::Stale)
            any_stale = true;

        if (cache_ram_frame_contains(ram_cache_, frame_key)) {
            ++cached;
        } else if (disk_cache_.contains(frame_key)) {
            ++cached;
            any_disk = true;
        }
    }

    if (explicit_state == FrameCacheState::Rendering || any_rendering)
        return FrameCacheState::Rendering;
    if (cached >= total)
        return any_disk ? FrameCacheState::CachedDisk : FrameCacheState::CachedRam;
    if (explicit_state == FrameCacheState::Queued || any_queued)
        return FrameCacheState::Queued;
    if (cached > 0)
        return FrameCacheState::Queued;
    if (any_stale)
        return FrameCacheState::Stale;
    return FrameCacheState::NotCached;
}

bool CacheManager::liveCueRowRenderPausedLocked(const QString &title_id, const QString &row_id) const
{
    if (title_id.isEmpty() || row_id.isEmpty())
        return false;
    return live_cue_paused_row_ids_.value(title_id).contains(row_id);
}

bool CacheManager::liveCueStateRenderPausedLocked(const QString &state_key) const
{
    if (state_key.isEmpty())
        return false;

    QString title_id = live_cue_title_ids_.value(state_key);
    if (title_id.isEmpty()) {
        const int marker = state_key.indexOf(QStringLiteral(":live-"));
        if (marker > 0)
            title_id = state_key.left(marker);
    }
    const QSet<QString> paused_rows = live_cue_paused_row_ids_.value(title_id);
    if (paused_rows.isEmpty())
        return false;

    const QString destination_id = live_cue_row_ids_.value(state_key);
    for (const QString &row_id : paused_rows) {
        if (destination_id == row_id ||
            state_key.contains(QStringLiteral(":%1:").arg(row_id)) ||
            state_key.endsWith(QStringLiteral(":%1").arg(row_id)))
            return true;
    }
    return false;
}

void CacheManager::queueLiveCueVariantSet(
    const std::shared_ptr<Title> &title, int row, const QString &state_key,
    const QVector<std::shared_ptr<Title>> &variants, bool urgent,
    bool hydrate_disk_to_ram, int manual_priority_group)
{
    std::lock_guard<std::recursive_mutex> live_lock(live_cue_mutex_);
    if (!cache_enabled_.load() || !title || state_key.isEmpty() || variants.isEmpty())
        return;

    const QString title_id = QString::fromStdString(title->id);
    const QString row_id = liveCueRowIdentity(title, row);
    if (liveCueRowRenderPausedLocked(title_id, row_id) || liveCueStateRenderPausedLocked(state_key)) {
        BGL_LOG_DEBUG("LiveCue", QStringLiteral("Skipped paused live cue state title=%1 row=%2 stateKey=%3")
                                    .arg(title_id).arg(row).arg(state_key));
        return;
    }
    live_cue_rows_[state_key] = row;
    live_cue_row_ids_[state_key] = row_id;
    live_cue_title_ids_[state_key] = title_id;
    if (state_key.contains(QStringLiteral(":live-transition:")))
        live_cue_transition_state_keys_.insert(state_key);
    else
        live_cue_transition_state_keys_.remove(state_key);

    struct RequiredFrame {
        CacheFrameKey key;
        std::shared_ptr<Title> title;
        int variant_index = 0;
    };
    QVector<RequiredFrame> required_frames;
    QVector<CacheFrameKey> required_keys;
    QSet<CacheFrameKey> required_seen;
    int variant_index = 0;
    for (const auto &variant_title : variants) {
        if (!variant_title) {
            ++variant_index;
            continue;
        }
        for (const CacheFrameKey &frame_key : frameKeysForTitle(*variant_title)) {
            if (required_seen.contains(frame_key))
                continue;
            required_seen.insert(frame_key);
            required_keys.push_back(frame_key);
            required_frames.push_back({frame_key, variant_title, variant_index});
            live_cue_known_keys_.insert(frame_key);
        }
        ++variant_index;
    }
    if (required_keys.isEmpty())
        return;

    const QVector<CacheFrameKey> old_required = live_cue_required_keys_.value(state_key);
    const bool requirements_changed = !live_cue_required_keys_.contains(state_key) || old_required != required_keys;
    live_cue_required_keys_[state_key] = required_keys;
    live_cue_required_total_[state_key] = static_cast<int>(required_keys.size());
    if (urgent) {
        /* Manual refresh owns an independent progress generation. Existing
         * resident frames may still be used by playback, but they must not
         * count as completed work for the new rebuild. */
        live_cue_total_by_key_[state_key] = static_cast<int>(required_keys.size());
        live_cue_done_by_key_[state_key] = 0;
        live_cue_last_emit_states_.remove(state_key);
        live_cue_last_emit_buckets_.remove(state_key);
    }
    if (requirements_changed) {
        live_cue_rendered_since_emit_.remove(state_key);
        live_cue_last_emit_states_.remove(state_key);
        live_cue_last_emit_buckets_.remove(state_key);
    }

    const FrameCacheState existing_state = liveCueStoredState(state_key);
    const int existing_progress = liveCueStoredProgress(state_key);
    const bool busy = existing_state == FrameCacheState::Queued || existing_state == FrameCacheState::Rendering;
    BGL_LOG_DEBUG("LiveCue", QStringLiteral("queueLiveCueState title=%1 row=%2 transition=%3 urgent=%4 state=%5 busy=%6 requiredFrames=%7 progress=%8 stateKey=%9")
                                .arg(title_id).arg(row)
                                .arg(live_cue_transition_state_keys_.contains(state_key))
                                .arg(urgent).arg((int)existing_state).arg(busy)
                                .arg(static_cast<int>(required_keys.size())).arg(existing_progress)
                                .arg(state_key));

    if (!urgent && !hydrate_disk_to_ram && (existing_state == FrameCacheState::CachedRam || existing_state == FrameCacheState::CachedDisk)) {
        live_cue_states_[state_key] = existing_state;
        live_cue_progress_percent_[state_key] = 100;
        return;
    }
    if (!requirements_changed && existing_state == FrameCacheState::Rendering)
        return;

    int queued = 0;
    bool active_or_queued = false;
    for (const RequiredFrame &required : required_frames) {
        const bool in_ram = cache_ram_frame_contains(ram_cache_, required.key);
        const bool on_disk = !in_ram && disk_cache_.contains(required.key);
        if (in_ram)
            continue;
        if (!urgent && !hydrate_disk_to_ram && on_disk)
            continue;
        if (state_tracker_.state(required.key) == FrameCacheState::Rendering ||
            queue_.contains(required.key)) {
            active_or_queued = true;
            if (!urgent && !hydrate_disk_to_ram)
                continue;
        }
        if (urgent)
            queue_.cancelKey(required.key);

        /* Playback hydration outranks all background prerender work, but it is
         * not a forced rebuild: renderJob() will load the existing sparse/LZ4
         * payload from disk and publish it into the shared RAM cache. */
        constexpr qint64 kPlaybackPriorityBase = -2000000000LL;
        constexpr qint64 kManualPriorityBase = -1500000000LL;
        constexpr qint64 kManualRowStride = 1000000LL;
        constexpr qint64 kVariantStride = 100000LL;
        const qint64 variant_priority = static_cast<qint64>(required.variant_index) * kVariantStride +
            std::max(0, required.key.frame);
        const int priority = hydrate_disk_to_ram
            ? clamp_cache_priority(kPlaybackPriorityBase + variant_priority)
            : urgent
                ? clamp_cache_priority(kManualPriorityBase +
                    static_cast<qint64>(std::max(0, manual_priority_group)) * kManualRowStride +
                    variant_priority)
                : clamp_cache_priority(-1000000LL + static_cast<qint64>(std::max(0, row)) * kVariantStride +
                    static_cast<qint64>(required.variant_index) * 1000LL + std::max(0, required.key.frame));
        RenderQueueManager::Job job;
        job.key = required.key;
        job.title = required.title;
        job.time = timeForFrame(required.key.frame);
        job.priority = priority;
        job.live_cue = true;
        job.realtime = hydrate_disk_to_ram;
        job.urgent = urgent || hydrate_disk_to_ram;
        job.force_render = !hydrate_disk_to_ram &&
            (urgent || live_cue_states_.value(state_key, FrameCacheState::NotCached) == FrameCacheState::Stale);
        job.cue_row = row;
        job.cue_state_key = state_key;
        job.cache_epoch = cache_epoch_.load();
        job.title_generation = titleGeneration(required.key.title_id);
        if (queue_.enqueue(job))
            ++queued;
    }

    if (urgent && queued == 0 && !active_or_queued) {
        /* A manual rebuild can be satisfied entirely by resident/shared frames.
         * Close that progress generation so the row resolves from real cache
         * state instead of staying Queued with no worker job to complete it. */
        live_cue_total_by_key_.remove(state_key);
        live_cue_done_by_key_.remove(state_key);
    }

    const int progress = liveCueStoredProgress(state_key);
    if (queued > 0) {
        ++live_cue_stats_.misses;
        live_cue_states_[state_key] = FrameCacheState::Queued;
        live_cue_progress_percent_[state_key] = progress;
        BGL_LOG_INFO("LiveCue", QStringLiteral("Queued live cue state title=%1 row=%2 transition=%3 queuedFrames=%4 requiredFrames=%5 progress=%6 stateKey=%7")
                                  .arg(title_id).arg(row)
                                  .arg(live_cue_transition_state_keys_.contains(state_key))
                                  .arg(queued).arg(static_cast<int>(required_keys.size()))
                                  .arg(progress).arg(state_key));
        emit liveCueStateChanged(title_id, row);
        emit diagnosticsChanged();
        wakeWorker();
    } else {
        const FrameCacheState resolved_state = liveCueStoredState(state_key);
        live_cue_states_[state_key] = resolved_state;
        live_cue_progress_percent_[state_key] = progress;
        if (shouldEmitLiveCueUpdate(state_key, resolved_state, progress))
            emit liveCueStateChanged(title_id, row);
    }

    if (requirements_changed)
        pruneUnreferencedLiveCueRam(title_id);
}

void CacheManager::queueLiveCue(const std::shared_ptr<Title> &title, int row, bool urgent)
{
    apply_persisted_live_cue_persistence(title);
    std::lock_guard<std::recursive_mutex> live_lock(live_cue_mutex_);
    if (!cache_enabled_.load() || !title ||
        titleCacheability(title) == TitleCacheability::NonCacheable) {
        BGL_LOG_DEBUG("LiveCue", QStringLiteral("Skipped queueLiveCue row=%1 because title caching is unavailable").arg(row));
        return;
    }
    const auto variants = liveCueVariants(title, row);
    if (variants.isEmpty()) {
        BGL_LOG_WARNING("LiveCue", QStringLiteral("Skipped queueLiveCue row=%1 because steady cue variant could not be created").arg(row));
        return;
    }
    queueLiveCueVariantSet(title, row, liveCueStateKey(title, row), variants, urgent);

    /* A cue request must also make its currently reachable transition ready.
     * Previously the dock could wait forever on isLiveCueReady(): it detected a
     * missing active->target transition but queueLiveCue() queued only the
     * steady target state. */
    if (title && title->current_cue_row >= 0 && title->current_cue_row != row &&
        title->current_cue_row < static_cast<int>(title->live_text_rows.size()) &&
        (title->playback_mode == 1 || title->playback_mode == 2 || title->cue_background_persistence)) {
        queueLiveCueTransition(title, title->current_cue_row, row, urgent);
    }
}

void CacheManager::queueLiveCueTransition(const std::shared_ptr<Title> &title,
                                          int from_row, int to_row, bool urgent)
{
    apply_persisted_live_cue_persistence(title);
    std::lock_guard<std::recursive_mutex> live_lock(live_cue_mutex_);
    if (!cache_enabled_.load() || !title ||
        titleCacheability(title) == TitleCacheability::NonCacheable)
        return;
    const auto variants = liveCueTransitionVariants(title, from_row, to_row);
    if (variants.isEmpty())
        return;
    queueLiveCueVariantSet(title, to_row,
                           liveCueTransitionStateKey(title, from_row, to_row),
                           variants, urgent);
}

void CacheManager::cacheLiveCueNow(const std::shared_ptr<Title> &title, int row)
{
    std::lock_guard<std::recursive_mutex> publication_lock(publication_mutex_);
    std::lock_guard<std::recursive_mutex> live_lock(live_cue_mutex_);
    if (!title || row < 0 || row >= static_cast<int>(title->live_text_rows.size()))
        return;

    const QString title_id = QString::fromStdString(title->id);
    const QString row_id = liveCueRowIdentity(title, row);
    if (liveCueRowRenderPausedLocked(title_id, row_id)) {
        /* Manual cache is an explicit user command and must win over the
         * edit-focus pause. A leaked/stale focus pause previously made the
         * cache button a permanent no-op. */
        QSet<QString> &paused_rows = live_cue_paused_row_ids_[title_id];
        paused_rows.remove(row_id);
        if (paused_rows.isEmpty())
            live_cue_paused_row_ids_.remove(title_id);
        BGL_LOG_INFO("LiveCue", QStringLiteral("Manual rebuild force-resumed paused live cue row title=%1 row=%2")
                                   .arg(title_id).arg(row));
    }
    QVector<QString> rebuild_states;
    /* A row's manual refresh must rebuild only its steady state. Transition
     * pairs belong to title-level prerendering and must not drive the status
     * badges of unrelated rows to Queued (100%). They remain valid until the
     * row content actually changes, at which point refreshLiveCueStructure()
     * invalidates only the affected transition pairs. */
    const QString steady_state_key = liveCueStateKey(title, row);
    if (live_cue_title_ids_.value(steady_state_key) == title_id ||
        live_cue_required_keys_.contains(steady_state_key))
        rebuild_states.push_back(steady_state_key);

    QSet<CacheFrameKey> rebuild_keys;
    for (const QString &state_key : rebuild_states) {
        for (const CacheFrameKey &old_key : live_cue_required_keys_.value(state_key))
            rebuild_keys.insert(old_key);
        removeLiveCueState(state_key);
    }

    /* Drop only frames that became unreferenced after removing the selected
     * row's steady/transition states. Content-addressed frames shared by an
     * unaffected state remain valid and must not be destroyed. Manual rebuild
     * still removes unshared disk entries so the selected state is regenerated. */
    int removed_frames = 0;
    for (const CacheFrameKey &frame_key : rebuild_keys) {
        if (isLiveCueKeyReferenced(frame_key))
            continue;
        queue_.cancelKey(frame_key);
        ram_cache_.remove(frame_key);
        title_gpu_frame_cache_remove(frame_key.toString().toStdString());
        disk_cache_.remove(frame_key);
        live_cue_known_keys_.remove(frame_key);
        state_tracker_.setState(frame_key, FrameCacheState::NotCached);
        ++removed_frames;
    }

    BGL_LOG_INFO("LiveCue", QStringLiteral("Manual live cue cache rebuild title=%1 row=%2 removedFrames=%3 candidateFrames=%4 removedStates=%5")
                              .arg(title_id).arg(row)
                              .arg(removed_frames)
                              .arg(static_cast<int>(rebuild_keys.size()))
                              .arg(static_cast<int>(rebuild_states.size())));

    const int manual_priority_group = next_manual_live_cue_priority_group_++;
    if (next_manual_live_cue_priority_group_ > 1000)
        next_manual_live_cue_priority_group_ = 0;
    queueLiveCueVariantSet(title, row, steady_state_key, liveCueVariants(title, row), true, false,
                           manual_priority_group);
    pruneUnreferencedLiveCueRam(title_id);
}


QImage CacheManager::requestLiveCueFrame(const std::shared_ptr<Title> &title, int row, double time, bool queue_if_missing)
{
    std::lock_guard<std::recursive_mutex> live_lock(live_cue_mutex_);
    if (!cache_enabled_.load() || !title || row < 0 || row >= static_cast<int>(title->live_text_rows.size()))
        return QImage();

    std::shared_ptr<Title> cue_title;
    const bool exact_runtime_state = title->pending_cue_row >= 0 ||
        (title->cue_persistence_transition && title->current_cue_row == row);
    if (exact_runtime_state)
        cue_title = clone_title_snapshot(title);
    else
        cue_title = titleWithCueApplied(title, row);
    if (!cue_title)
        return QImage();

    const double clamped_time = std::clamp(time, 0.0, std::max(0.0, cue_title->duration));
    const CacheFrameKey key = keyForTime(*cue_title, clamped_time);
    QString state_key = liveCueStateKey(title, row);

    /* Runtime transition snapshots are matched by their exact content-addressed
     * key. This avoids relying on mutable row indices or on a remembered
     * previous row after the incoming phase has begun. */
    for (auto it = live_cue_required_keys_.constBegin(); it != live_cue_required_keys_.constEnd(); ++it) {
        if (live_cue_title_ids_.value(it.key()) != key.title_id)
            continue;
        if (std::find(it.value().cbegin(), it.value().cend(), key) != it.value().cend()) {
            state_key = it.key();
            break;
        }
    }

    const FrameCacheState existing_state = live_cue_states_.value(state_key, FrameCacheState::NotCached);
    const bool busy = existing_state == FrameCacheState::Queued || existing_state == FrameCacheState::Rendering;
    QImage image;
    if (existing_state != FrameCacheState::Stale && ram_cache_.get(key, image)) {
        live_cue_known_keys_.insert(key);
        ++live_cue_stats_.hits;
        ++live_cue_stats_.reuses;
        emit diagnosticsChanged();
        return image;
    }
    if (existing_state != FrameCacheState::Stale && disk_cache_.get(key, image)) {
        if (!liveCueUseDiskStreaming())
            ram_cache_.put(key, image);
        live_cue_known_keys_.insert(key);
        ++live_cue_stats_.hits;
        ++live_cue_stats_.reuses;
        emit diagnosticsChanged();
        return image;
    }

    if (queue_if_missing) {
        if (title->pending_cue_row >= 0 && title->current_cue_row >= 0 &&
            title->current_cue_row != title->pending_cue_row) {
            queueLiveCueTransition(title, title->current_cue_row, title->pending_cue_row);
        } else {
            queueLiveCue(title, row);
        }
    }
    BGL_LOG_DEBUG("LiveCue", QStringLiteral("Live cue cache miss title=%1 row=%2 frame=%3 transition=%4 busy=%5 queue=%6 stateKey=%7")
                                .arg(key.title_id).arg(row).arg(key.frame)
                                .arg(live_cue_transition_state_keys_.contains(state_key))
                                .arg(busy).arg(queue_if_missing).arg(state_key));
    return QImage();
}

QString CacheManager::requestLiveCueFrameGpuToken(
    const std::shared_ptr<Title> &title, int row, double time,
    bool queue_if_missing)
{
    std::lock_guard<std::recursive_mutex> live_lock(live_cue_mutex_);
    if (!cache_enabled_.load() || !title || row < 0 ||
        row >= static_cast<int>(title->live_text_rows.size()))
        return QString();

    std::shared_ptr<Title> cue_title;
    const bool exact_runtime_state = title->pending_cue_row >= 0 ||
        (title->cue_persistence_transition && title->current_cue_row == row);
    if (exact_runtime_state)
        cue_title = clone_title_snapshot(title);
    else
        cue_title = titleWithCueApplied(title, row);
    if (!cue_title)
        return QString();

    const double clamped_time = std::clamp(
        time, 0.0, std::max(0.0, cue_title->duration));
    const CacheFrameKey key = keyForTime(*cue_title, clamped_time);
    if (title_gpu_frame_cache_contains(key.toString().toStdString())) {
        live_cue_known_keys_.insert(key);
        ++live_cue_stats_.hits;
        ++live_cue_stats_.reuses;
        emit diagnosticsChanged();
        return key.toString();
    }

    if (queue_if_missing && !disk_cache_.contains(key)) {
        if (title->pending_cue_row >= 0 && title->current_cue_row >= 0 &&
            title->current_cue_row != title->pending_cue_row) {
            queueLiveCueTransition(title, title->current_cue_row,
                                   title->pending_cue_row);
        } else {
            queueLiveCue(title, row);
        }
    }
    return QString();
}

QImage CacheManager::requestLiveCueFrameRealtime(const std::shared_ptr<Title> &title, int row, double time,
                                                     const QString &known_content_hash)
{
    if (!cache_enabled_.load() || interactive_bypass_.load() || !title || row < 0 ||
        row >= static_cast<int>(title->live_text_rows.size()))
        return QImage();

    /* Realtime playback must use the exact same runtime title snapshot as the
     * proven uncached source path. Re-applying a row here can discard an
     * in-progress persistence transition and can also make the caller-provided
     * content hash describe a different title than the rendered frame. */
    /* Lookup uses only immutable frame-key fields plus the caller's already
     * validated content hash. Do not deep-copy the complete title graph on
     * every OBS video tick; queueRealtimeJob snapshots only on a true miss. */
    const double clamped_time = std::clamp(time, 0.0, std::max(0.0, title->duration));
    const CacheFrameKey key = keyForTime(*title, clamped_time, known_content_hash);
    const FrameCacheState initial_state = state_tracker_.state(key);
    log_cache_playback(QStringLiteral("request kind=live-cue title=%1 row=%2 time=%3 frame=%4 state=%5 key=%6")
        .arg(key.title_id).arg(row).arg(clamped_time, 0, 'f', 6).arg(key.frame)
        .arg(QString::fromUtf8(cache_playback_state_name(initial_state)), key.toString()));
    QImage image;
    if (initial_state != FrameCacheState::Stale && ram_cache_.get(key, image)) {
        log_cache_playback(QStringLiteral("result=ram-hit kind=live-cue title=%1 row=%2 frame=%3 cacheKey=%4 size=%5x%6 key=%7")
            .arg(key.title_id).arg(row).arg(key.frame).arg(image.cacheKey())
            .arg(image.width()).arg(image.height()).arg(key.toString()));
        return image;
    }
    if (initial_state == FrameCacheState::CachedDisk && disk_cache_.get(key, image)) {
        ram_cache_.put(key, image);
        state_tracker_.setState(key, FrameCacheState::CachedRam);
        log_cache_playback(QStringLiteral("result=disk-hit-promoted kind=live-cue title=%1 row=%2 frame=%3 cacheKey=%4 key=%5")
            .arg(key.title_id).arg(row).arg(key.frame).arg(image.cacheKey()).arg(key.toString()));
        return image;
    }

    queueRealtimeJob(title, clamped_time, false, row, QString(), known_content_hash);
    log_cache_playback(QStringLiteral("result=miss-queued kind=live-cue title=%1 row=%2 frame=%3 initialState=%4 key=%5")
        .arg(key.title_id).arg(row).arg(key.frame)
        .arg(QString::fromUtf8(cache_playback_state_name(initial_state)), key.toString()));
    return QImage();
}

QImage CacheManager::requestLiveCueFrame(const std::shared_ptr<Title> &title, int row, bool queue_if_missing)
{
    std::lock_guard<std::recursive_mutex> live_lock(live_cue_mutex_);
    if (!title)
        return QImage();
    return requestLiveCueFrame(title, row, std::clamp(title->pause_time, 0.0, title->duration), queue_if_missing);
}

void CacheManager::preloadLiveCues(const std::shared_ptr<Title> &title, int current_row, int nearby_count)
{
    std::lock_guard<std::recursive_mutex> live_lock(live_cue_mutex_);
    if (!cache_enabled_.load() || !title || title->live_text_rows.empty() ||
        titleCacheability(title) == TitleCacheability::NonCacheable)
        return;
    const int count = static_cast<int>(title->live_text_rows.size());
    const int current = std::clamp(current_row, 0, count - 1);
    queueLiveCue(title, current);
    for (int i = 1; i <= std::max(1, nearby_count); ++i) {
        if (current - i >= 0) queueLiveCue(title, current - i);
        if (current + i < count) queueLiveCue(title, current + i);
    }
}

FrameCacheState CacheManager::liveCueState(const std::shared_ptr<Title> &title, int row) const
{
    std::lock_guard<std::recursive_mutex> live_lock(live_cue_mutex_);
    if (!cache_enabled_.load())
        return FrameCacheState::Disabled;
    if (title && titleCacheability(title) == TitleCacheability::NonCacheable)
        return FrameCacheState::Disabled;
    if (!title || row < 0 || row >= static_cast<int>(title->live_text_rows.size()))
        return FrameCacheState::NotCached;

    /* Row badges describe only the row's steady prerender state. Transition
     * variants are independent title-level work and are represented by the
     * aggregate cache status. Mixing them here caused every row to display
     * Queued (100%) whenever one row refresh created new transition work. */
    const QString steady_key = liveCueStateKey(title, row);
    return live_cue_required_keys_.contains(steady_key)
        ? liveCueStoredState(steady_key)
        : liveCueVariantsState(liveCueVariants(title, row), steady_key);
}

int CacheManager::liveCueProgressPercent(const std::shared_ptr<Title> &title, int row) const
{
    std::lock_guard<std::recursive_mutex> live_lock(live_cue_mutex_);
    if (!cache_enabled_.load() ||
        (title && titleCacheability(title) == TitleCacheability::NonCacheable))
        return 100;
    if (!title || row < 0 || row >= static_cast<int>(title->live_text_rows.size()))
        return 0;

    /* Keep per-row progress isolated from persistence transition pairs. The
     * aggregate progress API continues to include all transition frames. */
    const QString steady_key = liveCueStateKey(title, row);
    if (live_cue_total_by_key_.contains(steady_key))
        return liveCueStoredProgress(steady_key);

    const auto keys = live_cue_required_keys_.value(steady_key);
    if (keys.isEmpty())
        return liveCueVariantsProgress(liveCueVariants(title, row));

    int cached = 0;
    for (const CacheFrameKey &key : keys) {
        if (cache_ram_frame_contains(ram_cache_, key) ||
            disk_cache_.contains(key))
            ++cached;
    }
    return std::clamp((cached * 100) / static_cast<int>(keys.size()), 0, 100);
}

bool CacheManager::liveCueStateFullyResidentInRam(const QString &state_key) const
{
    const QVector<CacheFrameKey> keys = live_cue_required_keys_.value(state_key);
    if (keys.isEmpty())
        return false;
    for (const CacheFrameKey &key : keys) {
        if (!cache_ram_frame_contains(ram_cache_, key))
            return false;
    }
    return true;
}

bool CacheManager::liveCueStateStartResidentInRam(const QString &state_key) const
{
    const QVector<CacheFrameKey> keys = live_cue_required_keys_.value(state_key);
    if (keys.isEmpty())
        return false;

    QHash<QString, CacheFrameKey> first_key_by_content;
    for (const CacheFrameKey &key : keys) {
        const QString content_key = QStringLiteral("%1:%2:%3x%4")
            .arg(key.title_id, key.content_hash)
            .arg(key.width).arg(key.height);
        auto it = first_key_by_content.find(content_key);
        if (it == first_key_by_content.end() || key.frame < it.value().frame)
            first_key_by_content[content_key] = key;
    }

    for (const CacheFrameKey &key : first_key_by_content) {
        if (!cache_ram_frame_contains(ram_cache_, key))
            return false;
    }
    return true;
}

bool CacheManager::liveCueStateRangePlayable(const QString &state_key, int first_frame, int last_frame) const
{
    const QVector<CacheFrameKey> keys = live_cue_required_keys_.value(state_key);
    if (keys.isEmpty())
        return false;

    const int clamped_first = std::max(0, first_frame);
    const int clamped_last = std::max(clamped_first, last_frame);
    bool found_required_frame = false;
    for (const CacheFrameKey &key : keys) {
        if (key.frame < clamped_first || key.frame > clamped_last)
            continue;
        found_required_frame = true;
        if (cache_ram_frame_contains(ram_cache_, key) ||
            disk_cache_.contains(key))
            continue;
        return false;
    }
    return found_required_frame;
}

bool CacheManager::liveCueStateFullyPlayable(const QString &state_key) const
{
    const QVector<CacheFrameKey> keys = live_cue_required_keys_.value(state_key);
    if (keys.isEmpty())
        return false;

    for (const CacheFrameKey &key : keys) {
        if (cache_ram_frame_contains(ram_cache_, key) ||
            disk_cache_.contains(key))
            continue;
        return false;
    }
    return true;
}


bool CacheManager::liveCueUseDiskStreaming() const
{
    constexpr quint64 kMinSystemHeadroom = 512ull * 1024ull * 1024ull;
    constexpr quint64 kMinCacheHeadroom = 96ull * 1024ull * 1024ull;
    const quint64 available = available_system_memory_bytes();
    const quint64 limit = ram_cache_.maxBytes();
    const quint64 used = static_cast<quint64>(
        title_gpu_frame_cache_bytes_used());
    const quint64 cache_headroom = limit > used ? limit - used : 0;
    return available < kMinSystemHeadroom ||
           (used >= (limit * 85ull) / 100ull && cache_headroom < kMinCacheHeadroom);
}

bool CacheManager::prepareLiveCueForPlayback(const std::shared_ptr<Title> &title, int row)
{
    apply_persisted_live_cue_persistence(title);
    std::lock_guard<std::recursive_mutex> live_lock(live_cue_mutex_);
    if (!cache_enabled_.load() || !title || row < 0 ||
        row >= static_cast<int>(title->live_text_rows.size()) ||
        titleCacheability(title) == TitleCacheability::NonCacheable)
        return false;

    const QString steady_key = liveCueStateKey(title, row);
    const auto steady_variants = liveCueVariants(title, row);
    const bool disk_streaming = liveCueUseDiskStreaming();
    auto ensure_playback_state = [&](const QString &state_key,
                                     const QVector<std::shared_ptr<Title>> &variants) {
        const FrameCacheState state = live_cue_states_.value(state_key, FrameCacheState::NotCached);
        if (state == FrameCacheState::Queued || state == FrameCacheState::Rendering)
            return;
        queueLiveCueVariantSet(title, row, state_key, variants, false, !disk_streaming);
    };
    ensure_playback_state(steady_key, steady_variants);

    const double duration = std::max(0.0, title->duration);
    double steady_end_time = duration;
    if (title->playback_mode == 1) {
        const double loop_start = std::clamp(title->loop_start, 0.0, duration);
        steady_end_time = std::clamp(title->loop_end, loop_start, duration);
    } else if (title->playback_mode == 2) {
        steady_end_time = std::clamp(title->pause_time, 0.0, duration);
    }
    const int steady_last_frame = titleHasTimelineChanges(*title)
        ? std::clamp(frameForTime(steady_end_time), 0,
                     cache_last_frame_for_title(*title, effectiveFrameRate()))
        : 0;

    bool ready = liveCueStateRangePlayable(steady_key, 0, steady_last_frame);
    int required_state_count = 1;
    const int active_row = title->current_cue_row;
    if (active_row >= 0 && active_row != row &&
        active_row < static_cast<int>(title->live_text_rows.size()) &&
        (title->playback_mode == 1 || title->playback_mode == 2 || title->cue_background_persistence)) {
        const QString transition_key = liveCueTransitionStateKey(title, active_row, row);
        const auto transition_variants = liveCueTransitionVariants(title, active_row, row);
        ensure_playback_state(transition_key, transition_variants);
        /* A row-to-row cue starts on the outgoing suffix and then enters the
         * incoming prefix. Require the complete transition state so playback
         * can never fall through to uncached rendering between those phases. */
        ready = ready && liveCueStateFullyPlayable(transition_key);
        ++required_state_count;
    }

    BGL_LOG_INFO("LiveCue", QStringLiteral("Playback cache gate title=%1 row=%2 states=%3 ready=%4 requiredEndFrame=%5 storage=ram-or-ssd hydrate=%6")
                              .arg(QString::fromStdString(title->id)).arg(row)
                              .arg(required_state_count).arg(ready)
                              .arg(steady_last_frame)
                              .arg(disk_streaming ? QStringLiteral("ssd-stream") : QStringLiteral("ram-prefetch")));
    return ready;
}


bool CacheManager::isLiveCueReady(const std::shared_ptr<Title> &title, int row)
{
    std::lock_guard<std::recursive_mutex> live_lock(live_cue_mutex_);
    if (!title || row < 0 || row >= static_cast<int>(title->live_text_rows.size()))
        return false;

    const QString steady_key = liveCueStateKey(title, row);
    const FrameCacheState steady = live_cue_required_keys_.contains(steady_key)
        ? liveCueStoredState(steady_key) : FrameCacheState::NotCached;
    bool ready = steady == FrameCacheState::CachedRam || steady == FrameCacheState::CachedDisk;

    const int active_row = title->current_cue_row;
    if (ready && active_row >= 0 && active_row != row &&
        active_row < static_cast<int>(title->live_text_rows.size()) &&
        (title->playback_mode == 1 || title->playback_mode == 2 || title->cue_background_persistence)) {
        const QString transition_key = liveCueTransitionStateKey(title, active_row, row);
        const FrameCacheState transition = live_cue_required_keys_.contains(transition_key)
            ? liveCueStoredState(transition_key) : FrameCacheState::NotCached;
        ready = transition == FrameCacheState::CachedRam || transition == FrameCacheState::CachedDisk;
    }

    if (ready) {
        ++live_cue_stats_.hits;
        ++live_cue_stats_.reuses;
        emit diagnosticsChanged();
    }
    return ready;
}

FrameCacheState CacheManager::liveCueAggregateState(const std::shared_ptr<Title> &title) const
{
    std::lock_guard<std::recursive_mutex> live_lock(live_cue_mutex_);
    if (!cache_enabled_.load())
        return FrameCacheState::Disabled;
    if (!title)
        return FrameCacheState::NotCached;
    const QString title_id = QString::fromStdString(title->id);
    bool any = false, queued = false, rendering = false, stale = false, disk = false;
    bool missing_rows = false;
    for (auto it = live_cue_title_ids_.constBegin(); it != live_cue_title_ids_.constEnd(); ++it) {
        if (it.value() != title_id)
            continue;
        any = true;
        const FrameCacheState state = liveCueStoredState(it.key());
        stale |= state == FrameCacheState::Stale;
        rendering |= state == FrameCacheState::Rendering;
        queued |= state == FrameCacheState::Queued || state == FrameCacheState::NotCached;
        disk |= state == FrameCacheState::CachedDisk;
    }
    for (int row = 0; row < static_cast<int>(title->live_text_rows.size()); ++row) {
        const QString state_key = liveCueStateKey(title, row);
        if (live_cue_required_keys_.contains(state_key))
            continue;
        const FrameCacheState state = liveCueVariantsState(liveCueVariants(title, row), state_key);
        if (state == FrameCacheState::NotCached) {
            missing_rows = true;
            continue;
        }
        any = true;
        stale |= state == FrameCacheState::Stale;
        rendering |= state == FrameCacheState::Rendering;
        queued |= state == FrameCacheState::Queued;
        disk |= state == FrameCacheState::CachedDisk;
    }
    if (!any) return FrameCacheState::NotCached;
    queued |= missing_rows;
    if (stale) return FrameCacheState::Stale;
    if (rendering) return FrameCacheState::Rendering;
    if (queued) return FrameCacheState::Queued;
    return disk ? FrameCacheState::CachedDisk : FrameCacheState::CachedRam;
}

int CacheManager::liveCueAggregateProgressPercent(const std::shared_ptr<Title> &title) const
{
    std::lock_guard<std::recursive_mutex> live_lock(live_cue_mutex_);
    if (!cache_enabled_.load())
        return 100;
    if (!title)
        return 0;
    const QString title_id = QString::fromStdString(title->id);
    QSet<CacheFrameKey> keys;
    for (auto it = live_cue_required_keys_.constBegin(); it != live_cue_required_keys_.constEnd(); ++it) {
        if (live_cue_title_ids_.value(it.key()) != title_id)
            continue;
        for (const CacheFrameKey &key : it.value())
            keys.insert(key);
    }
    for (int row = 0; row < static_cast<int>(title->live_text_rows.size()); ++row) {
        const QString state_key = liveCueStateKey(title, row);
        if (live_cue_required_keys_.contains(state_key))
            continue;
        for (const auto &variant : liveCueVariants(title, row)) {
            if (!variant)
                continue;
            for (const CacheFrameKey &key : frameKeysForTitle(*variant))
                keys.insert(key);
        }
    }
    if (keys.isEmpty())
        return 0;
    int cached = 0;
    for (const CacheFrameKey &key : keys) {
        if (cache_ram_frame_contains(ram_cache_, key) ||
            disk_cache_.contains(key))
            ++cached;
    }
    return std::clamp((cached * 100) / static_cast<int>(keys.size()), 0, 100);
}


void CacheManager::invalidateLiveCue(const std::shared_ptr<Title> &title, int row)
{
    std::lock_guard<std::recursive_mutex> publication_lock(publication_mutex_);
    std::lock_guard<std::recursive_mutex> live_lock(live_cue_mutex_);
    if (!title || row < 0 || row >= static_cast<int>(title->live_text_rows.size()))
        return;

    ensure_live_text_row_ids(*title);
    const QString title_id = QString::fromStdString(title->id);
    const QString row_id = liveCueRowIdentity(title, row);
    QVector<QString> affected_states;
    for (auto it = live_cue_title_ids_.constBegin(); it != live_cue_title_ids_.constEnd(); ++it) {
        if (it.value() != title_id)
            continue;
        const QString state_key = it.key();
        if (live_cue_row_ids_.value(state_key) == row_id ||
            state_key.contains(QStringLiteral(":%1:").arg(row_id)) ||
            state_key.endsWith(QStringLiteral(":%1").arg(row_id)))
            affected_states.push_back(state_key);
    }
    for (const QString &state_key : affected_states)
        removeLiveCueState(state_key);

    ++live_cue_stats_.invalidations;
    pruneUnreferencedLiveCueRam(title_id);
    queueLiveCue(title, row);
    for (int other = 0; other < static_cast<int>(title->live_text_rows.size()); ++other) {
        if (other == row)
            continue;
        queueLiveCueTransition(title, row, other);
        queueLiveCueTransition(title, other, row);
    }
    BGL_LOG_INFO("LiveCue", QStringLiteral("Invalidated live cue identity title=%1 row=%2 rowId=%3 removedStates=%4")
                              .arg(title_id).arg(row).arg(row_id)
                              .arg(static_cast<int>(affected_states.size())));
    emit diagnosticsChanged();
    emit liveCueStateChanged(title_id, row);
}

void CacheManager::invalidateLiveCues(const std::shared_ptr<Title> &title)
{
    std::lock_guard<std::recursive_mutex> publication_lock(publication_mutex_);
    std::lock_guard<std::recursive_mutex> live_lock(live_cue_mutex_);
    if (!title)
        return;
    const QString title_id = QString::fromStdString(title->id);
    QVector<QString> state_keys;
    for (auto it = live_cue_title_ids_.constBegin(); it != live_cue_title_ids_.constEnd(); ++it) {
        if (it.value() == title_id)
            state_keys.push_back(it.key());
    }
    for (const QString &state_key : state_keys) {
        const int row = live_cue_rows_.value(state_key, -1);
        removeLiveCueState(state_key);
        if (row >= 0)
            emit liveCueStateChanged(title_id, row);
    }
    live_cue_structure_row_ids_.remove(title_id);
    live_cue_row_fingerprints_.remove(title_id);
    live_cue_transition_signatures_.remove(title_id);
    live_cue_paused_row_ids_.remove(title_id);
    ++live_cue_stats_.invalidations;
    pruneUnreferencedLiveCueRam(title_id);
    emit diagnosticsChanged();
}

void CacheManager::refreshLiveCueStructure(const std::shared_ptr<Title> &title)
{
    apply_persisted_live_cue_persistence(title);
    if (!title)
        return;
    if (titleCacheability(title) == TitleCacheability::NonCacheable) {
        removeTitleCache(QString::fromStdString(title->id), true);
        emit liveCueStateChanged(QString::fromStdString(title->id), -1);
        return;
    }
    std::lock_guard<std::recursive_mutex> publication_lock(publication_mutex_);
    std::lock_guard<std::recursive_mutex> live_lock(live_cue_mutex_);
    if (!cache_enabled_.load() || !title)
        return;

    const QString title_id = QString::fromStdString(title->id);
    if (live_cue_structure_refresh_in_progress_.contains(title_id)) {
        BGL_LOG_TRACE("LiveCue", QStringLiteral("Skipped re-entrant live cue structure refresh title=%1")
                                      .arg(title_id));
        return;
    }

    struct RefreshGuard {
        QSet<QString> &set;
        QString id;
        RefreshGuard(QSet<QString> &target, const QString &title_id) : set(target), id(title_id)
        {
            set.insert(id);
        }
        ~RefreshGuard()
        {
            set.remove(id);
        }
    } refresh_guard(live_cue_structure_refresh_in_progress_, title_id);

    ensure_live_text_row_ids(*title);
    const int row_count = static_cast<int>(title->live_text_rows.size());
    QSet<QString> current_ids;
    QHash<QString, int> index_by_id;
    QHash<QString, QString> current_fingerprints;
    for (int row = 0; row < row_count; ++row) {
        const QString row_id = liveCueRowIdentity(title, row);
        current_ids.insert(row_id);
        index_by_id.insert(row_id, row);
        QCryptographicHash hash(QCryptographicHash::Sha1);
        for (const std::string &cell : title->live_text_rows[static_cast<size_t>(row)]) {
            const QByteArray bytes = QByteArray::fromStdString(cell);
            hash.addData(bytes);
            hash.addData("\0", 1);
        }
        current_fingerprints.insert(row_id, QString::fromLatin1(hash.result().toHex()));
    }

    QCryptographicHash signature_hash(QCryptographicHash::Sha1);
    auto add_sig = [&](const QByteArray &value) { signature_hash.addData(value); signature_hash.addData("\0", 1); };
    add_sig(QByteArray::number(title->playback_mode));
    add_sig(QByteArray::number(title->cue_background_persistence));
    add_sig(QByteArray::number(title->cue_text_persistence));
    add_sig(QByteArray::number(title->pause_time, 'g', 17));
    add_sig(QByteArray::number(title->loop_start, 'g', 17));
    add_sig(QByteArray::number(title->loop_end, 'g', 17));
    add_sig(QByteArray::number(title->duration, 'g', 17));
    const QString transition_signature = QString::fromLatin1(signature_hash.result().toHex());

    const QSet<QString> previous_ids = live_cue_structure_row_ids_.value(title_id);
    const QHash<QString, QString> previous_fingerprints = live_cue_row_fingerprints_.value(title_id);
    const bool settings_changed = live_cue_transition_signatures_.value(title_id) != transition_signature;
    const bool first_structure_refresh = previous_ids.isEmpty();

    QSet<QString> added_ids = current_ids;
    for (const QString &id : previous_ids)
        added_ids.remove(id);
    QSet<QString> removed_ids = previous_ids;
    for (const QString &id : current_ids)
        removed_ids.remove(id);
    QSet<QString> changed_ids;
    for (const QString &id : current_ids) {
        if (previous_fingerprints.contains(id) && previous_fingerprints.value(id) != current_fingerprints.value(id))
            changed_ids.insert(id);
    }
    if (settings_changed && !previous_ids.isEmpty())
        changed_ids.unite(current_ids);

    QVector<QString> obsolete_states;
    auto state_involves_any = [&](const QString &state_key, const QSet<QString> &ids) {
        const QString destination_id = live_cue_row_ids_.value(state_key);
        for (const QString &id : ids) {
            if (destination_id == id ||
                state_key.contains(QStringLiteral(":%1:").arg(id)) ||
                state_key.endsWith(QStringLiteral(":%1").arg(id)))
                return true;
        }
        return false;
    };

    for (auto it = live_cue_title_ids_.constBegin(); it != live_cue_title_ids_.constEnd(); ++it) {
        if (it.value() != title_id)
            continue;
        const QString state_key = it.key();
        if (state_involves_any(state_key, removed_ids) ||
            state_involves_any(state_key, changed_ids)) {
            obsolete_states.push_back(state_key);
            continue;
        }
        const QString destination_id = live_cue_row_ids_.value(state_key);
        if (index_by_id.contains(destination_id))
            live_cue_rows_[state_key] = index_by_id.value(destination_id);
    }
    for (const QString &state_key : obsolete_states)
        removeLiveCueState(state_key);

    /* Cancel obsolete queued work and evict orphan RAM before registering new
     * states. Doing this afterwards allowed an old job with the same content
     * key to block the replacement job, then finish against a deleted row. */
    pruneUnreferencedLiveCueRam(title_id);

    QSet<QString> affected_ids = added_ids;
    affected_ids.unite(changed_ids);
    if (first_structure_refresh)
        affected_ids = current_ids;

    if (!first_structure_refresh) {
        for (const QString &row_id : affected_ids) {
            if (!index_by_id.contains(row_id))
                continue;
            queueLiveCue(title, index_by_id.value(row_id));
        }

        /* Transition states are independent from row-ready states. Adding a row
         * creates only the new from/to pairs involving that row; existing rows stay
         * ready and keep their cache icons. Editing a row rebuilds only pairs that
         * actually include that stable row identity. */
        for (const QString &affected_id : affected_ids) {
            if (!index_by_id.contains(affected_id))
                continue;
            const int affected_row = index_by_id.value(affected_id);
            for (int other = 0; other < row_count; ++other) {
                if (other == affected_row)
                    continue;
                queueLiveCueTransition(title, affected_row, other);
                queueLiveCueTransition(title, other, affected_row);
            }
        }
    }

    live_cue_structure_row_ids_[title_id] = current_ids;
    live_cue_row_fingerprints_[title_id] = current_fingerprints;
    live_cue_transition_signatures_[title_id] = transition_signature;
    pruneUnreferencedLiveCueRam(title_id);
    BGL_LOG_INFO("LiveCue", QStringLiteral("Refreshed live cue structure title=%1 rows=%2 added=%3 changed=%4 removed=%5 removedStates=%6")
                              .arg(title_id).arg(row_count)
                              .arg(static_cast<int>(added_ids.size()))
                              .arg(static_cast<int>(changed_ids.size()))
                              .arg(static_cast<int>(removed_ids.size()))
                              .arg(static_cast<int>(obsolete_states.size())));
    emit diagnosticsChanged();
}

void CacheManager::refreshLiveCueStructureAsync(const std::shared_ptr<Title> &title)
{
    if (!title)
        return;
    auto snapshot = clone_title_snapshot(title);
    if (!snapshot)
        return;
    const QString title_id = QString::fromStdString(snapshot->id);
    {
        std::lock_guard<std::mutex> lock(live_cue_refresh_mutex_);
        pending_live_cue_structure_refreshes_[title_id] = std::move(snapshot);
    }
    wakeWorker();
}

bool CacheManager::takePendingLiveCueStructureRefresh(std::shared_ptr<Title> &title)
{
    std::lock_guard<std::mutex> lock(live_cue_refresh_mutex_);
    if (pending_live_cue_structure_refreshes_.isEmpty())
        return false;
    auto it = pending_live_cue_structure_refreshes_.begin();
    title = it.value();
    pending_live_cue_structure_refreshes_.erase(it);
    return title != nullptr;
}

void CacheManager::setLiveCueRowRenderPaused(const std::shared_ptr<Title> &title, int row, bool paused)
{
    if (!title || row < 0 || row >= static_cast<int>(title->live_text_rows.size()))
        return;

    const QString title_id = QString::fromStdString(title->id);
    const QString row_id = liveCueRowIdentity(title, row);
    if (row_id.isEmpty())
        return;

    QVector<QString> affected_states;
    {
        std::lock_guard<std::recursive_mutex> publication_lock(publication_mutex_);
        std::lock_guard<std::recursive_mutex> live_lock(live_cue_mutex_);
        QSet<QString> &paused_rows = live_cue_paused_row_ids_[title_id];
        const bool changed = paused ? !paused_rows.contains(row_id)
                                    : paused_rows.contains(row_id);
        if (!changed)
            return;

        if (paused)
            paused_rows.insert(row_id);
        else {
            paused_rows.remove(row_id);
            if (paused_rows.isEmpty())
                live_cue_paused_row_ids_.remove(title_id);
        }

        for (auto it = live_cue_title_ids_.constBegin(); it != live_cue_title_ids_.constEnd(); ++it) {
            if (it.value() != title_id)
                continue;
            if (liveCueStateRenderPausedLocked(it.key()))
                affected_states.push_back(it.key());
        }

        if (paused) {
            for (const QString &state_key : affected_states) {
                for (const CacheFrameKey &key : live_cue_required_keys_.value(state_key))
                    queue_.cancelKey(key);
                const FrameCacheState state = liveCueStoredState(state_key);
                live_cue_states_[state_key] = state;
                live_cue_progress_percent_[state_key] = liveCueStoredProgress(state_key);
                live_cue_last_emit_states_.remove(state_key);
                live_cue_last_emit_buckets_.remove(state_key);
            }
        }
    }

    BGL_LOG_DEBUG("LiveCue", QStringLiteral("%1 live cue row render title=%2 row=%3 rowId=%4 states=%5")
                                .arg(paused ? QStringLiteral("Paused") : QStringLiteral("Resumed"))
                                .arg(title_id).arg(row).arg(row_id)
                                .arg(static_cast<int>(affected_states.size())));
    emit liveCueStateChanged(title_id, row);
    emit diagnosticsChanged();

    if (!paused) {
        refreshLiveCueStructure(title);
        /* refreshLiveCueStructure() may already have accepted the edited row's
         * new fingerprint while the row was paused. In that case it sees no new
         * change here and would not requeue the jobs that focus-in cancelled.
         * Explicitly restore the steady row and every transition involving it. */
        queueLiveCue(title, row);
        for (int other = 0; other < static_cast<int>(title->live_text_rows.size()); ++other) {
            if (other == row)
                continue;
            queueLiveCueTransition(title, row, other);
            queueLiveCueTransition(title, other, row);
        }
        wakeWorker();
    }
}

void CacheManager::cancelTitleWork(const QString &title_id)
{
    if (title_id.isEmpty())
        return;
    {
        std::lock_guard<std::mutex> refresh_lock(live_cue_refresh_mutex_);
        pending_live_cue_structure_refreshes_.remove(title_id);
    }
    {
        std::lock_guard<std::recursive_mutex> publication_lock(publication_mutex_);
        bumpTitleGeneration(title_id);
        queue_.cancelTitle(title_id);
    }
    resetCancelledWorkState(title_id);
    wakeWorker();
    BGL_LOG_DEBUG("Cache", QStringLiteral("Cancelled pending/in-flight title cache work title=%1").arg(title_id));
}

void CacheManager::removeTitleCache(const QString &title_id, bool remove_disk)
{
    std::lock_guard<std::recursive_mutex> publication_lock(publication_mutex_);
    std::lock_guard<std::recursive_mutex> live_lock(live_cue_mutex_);
    if (title_id.isEmpty())
        return;
    {
        std::lock_guard<std::mutex> refresh_lock(live_cue_refresh_mutex_);
        pending_live_cue_structure_refreshes_.remove(title_id);
    }
    bumpTitleGeneration(title_id);
    queue_.cancelTitle(title_id);

    QVector<QString> state_keys;
    for (auto it = live_cue_title_ids_.constBegin(); it != live_cue_title_ids_.constEnd(); ++it) {
        if (it.value() == title_id)
            state_keys.push_back(it.key());
    }
    for (const QString &state_key : state_keys)
        removeLiveCueState(state_key);

    title_gpu_frame_cache_remove_title(title_id.toStdString());
    const auto ram_keys = ram_cache_.keysForTitle(title_id);
    for (const CacheFrameKey &key : ram_keys) {
        ram_cache_.remove(key);
        title_gpu_frame_cache_remove(key.toString().toStdString());
    }
    if (remove_disk) {
        const auto disk_keys = disk_cache_.keysForTitle(title_id);
        for (const CacheFrameKey &key : disk_keys)
            disk_cache_.remove(key);
    }
    for (auto it = live_cue_known_keys_.begin(); it != live_cue_known_keys_.end();) {
        if ((*it).title_id == title_id)
            it = live_cue_known_keys_.erase(it);
        else
            ++it;
    }
    state_tracker_.clearTitle(title_id);
    live_cue_structure_row_ids_.remove(title_id);
    live_cue_row_fingerprints_.remove(title_id);
    live_cue_transition_signatures_.remove(title_id);
    live_cue_paused_row_ids_.remove(title_id);
    BGL_LOG_INFO("Cache", QStringLiteral("Removed title cache title=%1 ramFrames=%2 states=%3 disk=%4")
                            .arg(title_id).arg(static_cast<int>(ram_keys.size()))
                            .arg(static_cast<int>(state_keys.size())).arg(remove_disk));
    emit diagnosticsChanged();
}


CachePlaybackSettings CacheManager::playbackSettings() const
{
    QMutexLocker lock(&playback_settings_mutex_);
    return playback_settings_;
}

LiveCueCacheStats CacheManager::liveCueStats() const
{
    std::lock_guard<std::recursive_mutex> live_lock(live_cue_mutex_);
    return live_cue_stats_;
}

void CacheManager::setPlaybackSettings(const CachePlaybackSettings &settings)
{
    QMutexLocker lock(&playback_settings_mutex_);
    playback_settings_ = settings;
}

void CacheManager::abandonJobState(const RenderQueueManager::Job &job,
                                         const QString &live_state_key)
{
    if (job.live_cue) {
        std::lock_guard<std::recursive_mutex> live_lock(live_cue_mutex_);
        const QString state_key = liveCueStateReferencingKey(job.key, live_state_key);
        if (state_key.isEmpty() ||
            live_cue_states_.value(state_key, FrameCacheState::NotCached) != FrameCacheState::Rendering)
            return;

        bool replacement_queued = false;
        for (const CacheFrameKey &required : live_cue_required_keys_.value(state_key)) {
            if (queue_.contains(required)) {
                replacement_queued = true;
                break;
            }
        }
        const FrameCacheState next_state = replacement_queued
            ? FrameCacheState::Queued
            : FrameCacheState::NotCached;
        const int progress = liveCueStoredProgress(state_key);
        const int signal_row = live_cue_rows_.value(state_key, job.cue_row);
        live_cue_states_[state_key] = next_state;
        live_cue_progress_percent_[state_key] = progress;
        live_cue_last_emit_states_.remove(state_key);
        live_cue_last_emit_buckets_.remove(state_key);
        if (signal_row >= 0)
            emit liveCueStateChanged(job.key.title_id, signal_row);
        return;
    }

    if (state_tracker_.state(job.key) != FrameCacheState::Rendering)
        return;
    state_tracker_.setState(job.key, queue_.contains(job.key)
        ? FrameCacheState::Queued
        : FrameCacheState::NotCached);
}

bool CacheManager::hasPendingGpuReadbacks() const
{
    return worker_pipeline_ && !worker_pipeline_->pending_readbacks.empty();
}

void CacheManager::workerLoop()
{
    enter_cache_worker_background_mode();

    while (!worker_stop_.load()) {
        {
            std::unique_lock<std::mutex> lock(worker_wait_mutex_);
            worker_cv_.wait(lock, [this]() {
                if (worker_stop_.load())
                    return true;
                if (hasPendingGpuReadbacks())
                    return true;
                if (!cache_enabled_.load())
                    return false;
                {
                    std::lock_guard<std::mutex> refresh_lock(live_cue_refresh_mutex_);
                    if (!pending_live_cue_structure_refreshes_.isEmpty())
                        return true;
                }
                const bool live_only = paused_.load() || interactive_bypass_.load();
                if (live_only)
                    return queue_.hasAvailableJob(true);
                bool editor_focus_active = false;
                QString editor_title_id;
                {
                    QMutexLocker focus_lock(&editor_focus_mutex_);
                    editor_focus_active = editor_focus_active_;
                    editor_title_id = editor_focus_title_id_;
                }
                if (editor_focus_active)
                    return queue_.hasAvailableJob(true) ||
                           queue_.hasAvailableJobForTitle(editor_title_id);
                return queue_.hasAvailableJob(false);
            });
        }
        if (worker_stop_.load())
            break;
        if (!cache_enabled_.load()) {
            if (hasPendingGpuReadbacks())
                resolveOldestGpuReadback(true);
            continue;
        }

        std::shared_ptr<Title> refresh_title;
        if (takePendingLiveCueStructureRefresh(refresh_title)) {
            refreshLiveCueStructure(refresh_title);
            continue;
        }

        RenderQueueManager::Job job;
        const bool urgent_only = paused_.load() || interactive_bypass_.load();
        bool editor_focus_active = false;
        QString editor_title_id;
        {
            QMutexLocker lock(&editor_focus_mutex_);
            editor_focus_active = editor_focus_active_;
            editor_title_id = editor_focus_title_id_;
        }

        bool have_job = false;
        if (urgent_only) {
            have_job = queue_.takeNextUrgent(job);
        } else if (editor_focus_active) {
            have_job = queue_.takeNextUrgent(job);
            if (!have_job)
                have_job = queue_.takeNextForTitle(editor_title_id, job);
        } else {
            have_job = queue_.takeNext(job);
        }

        if (!have_job) {
            /* No more GPU work is available. Resolve the oldest submitted
             * staging surface rather than sleeping with an incomplete frame. */
            if (hasPendingGpuReadbacks())
                resolveOldestGpuReadback(true);
            continue;
        }

        if (job.live_cue && !job.urgent && !job.realtime)
            std::this_thread::sleep_for(std::chrono::milliseconds(8));

        const bool readback_submitted = renderJob(job);
        if (!readback_submitted)
            queue_.complete(job);

        /* Keep up to three GPU copies in flight. Mapping starts only when the
         * ring is full, allowing frame N readback to overlap rendering N+1/N+2. */
        if (worker_pipeline_->pending_readbacks.size() >= 3)
            resolveOldestGpuReadback(true);

        const int cooldown_ms = (job.live_cue && !job.urgent && !job.realtime)
            ? 12 : (job.urgent || (job.live_cue && job.realtime)) ? 0 : 1;
        if (cooldown_ms > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(cooldown_ms));
    }

    while (hasPendingGpuReadbacks())
        resolveOldestGpuReadback(true);
    disk_cache_.flushWrites();
}

bool CacheManager::retryFailedJob(RenderQueueManager::Job job,
                                  const QString &live_state_key,
                                  const char *stage)
{
    constexpr int kMaximumGpuReadbackRetries = 4;
    if (worker_stop_.load() || !cache_enabled_.load() ||
        !jobIsCurrent(job) || job.retry_count >= kMaximumGpuReadbackRetries)
        return false;

    /* The queue suppresses duplicate keys while a job is active. Release the
     * active token before re-enqueuing the exact same content-addressed frame. */
    queue_.complete(job);
    ++job.retry_count;

    if (job.live_cue) {
        std::lock_guard<std::recursive_mutex> live_lock(live_cue_mutex_);
        const QString state_key = liveCueStateReferencingKey(job.key, live_state_key);
        if (!state_key.isEmpty()) {
            job.cue_state_key = state_key;
            job.cue_row = live_cue_rows_.value(state_key, job.cue_row);
            live_cue_states_[state_key] = FrameCacheState::Queued;
            live_cue_progress_percent_[state_key] = liveCueStoredProgress(state_key);
            live_cue_last_emit_states_.remove(state_key);
            live_cue_last_emit_buckets_.remove(state_key);
            if (job.cue_row >= 0)
                emit liveCueStateChanged(job.key.title_id, job.cue_row);
        }
    } else {
        state_tracker_.setState(job.key, FrameCacheState::Queued);
    }

    const bool queued = queue_.enqueue(job);
    if (!queued) {
        /* Another producer can fill the small gap between complete() and
         * enqueue(). In that case the retry has already been coalesced into a
         * replacement job and must stay Queued instead of being mislabeled as
         * Stale. If the queue is shutting down, consume the released active
         * token here and publish a terminal state so the caller does not
         * complete the same token twice. */
        const bool replacement_queued = queue_.contains(job.key);
        if (!replacement_queued) {
            if (job.live_cue) {
                std::lock_guard<std::recursive_mutex> live_lock(live_cue_mutex_);
                const QString state_key = liveCueStateReferencingKey(job.key, live_state_key);
                if (!state_key.isEmpty()) {
                    const int signal_row = live_cue_rows_.value(state_key, job.cue_row);
                    live_cue_states_[state_key] = FrameCacheState::NotCached;
                    live_cue_progress_percent_[state_key] = liveCueStoredProgress(state_key);
                    live_cue_last_emit_states_.remove(state_key);
                    live_cue_last_emit_buckets_.remove(state_key);
                    if (signal_row >= 0)
                        emit liveCueStateChanged(job.key.title_id, signal_row);
                }
            }
            state_tracker_.setState(job.key, FrameCacheState::Stale);
            BGL_LOG_ERROR("CacheQueue", QStringLiteral(
                "stage=%1 action=retry-enqueue-failed title=%2 frame=%3 attempt=%4 key=%5")
                .arg(QString::fromUtf8(stage ? stage : "gpu-readback"))
                .arg(job.key.title_id)
                .arg(job.key.frame)
                .arg(job.retry_count)
                .arg(job.key.toString()));
        } else {
            BGL_LOG_DEBUG("CacheQueue", QStringLiteral(
                "stage=%1 action=retry-coalesced title=%2 frame=%3 attempt=%4 key=%5")
                .arg(QString::fromUtf8(stage ? stage : "gpu-readback"))
                .arg(job.key.title_id)
                .arg(job.key.frame)
                .arg(job.retry_count)
                .arg(job.key.toString()));
            wakeWorker();
        }
        return true;
    }

    BGL_LOG_WARNING("CacheQueue", QStringLiteral(
        "stage=%1 action=retry title=%2 frame=%3 attempt=%4/%5 key=%6")
        .arg(QString::fromUtf8(stage ? stage : "gpu-readback"))
        .arg(job.key.title_id)
        .arg(job.key.frame)
        .arg(job.retry_count)
        .arg(kMaximumGpuReadbackRetries)
        .arg(job.key.toString()));
    wakeWorker();
    return true;
}

bool CacheManager::renderJob(RenderQueueManager::Job job)
{
    if (!job.title || !jobIsCurrent(job))
        return false;

    QString live_state_key = job.cue_state_key;
    bool was_stale = false;
    if (job.live_cue) {
        std::lock_guard<std::recursive_mutex> lock(live_cue_mutex_);
        const QString rebound_state = liveCueStateReferencingKey(job.key, live_state_key);
        if (rebound_state.isEmpty())
            return false;
        if (liveCueStateRenderPausedLocked(rebound_state))
            return false;
        live_state_key = rebound_state;
        job.cue_state_key = rebound_state;
        job.cue_row = live_cue_rows_.value(rebound_state, job.cue_row);
        was_stale = job.force_render ||
            live_cue_states_.value(rebound_state, FrameCacheState::NotCached) ==
                FrameCacheState::Stale;
        if (live_cue_states_.value(rebound_state, FrameCacheState::NotCached) !=
            FrameCacheState::Rendering) {
            live_cue_states_[rebound_state] = FrameCacheState::Rendering;
            live_cue_progress_percent_[rebound_state] = liveCueStoredProgress(rebound_state);
            if (shouldEmitLiveCueUpdate(
                    rebound_state, FrameCacheState::Rendering,
                    live_cue_progress_percent_.value(rebound_state)))
                emit liveCueStateChanged(job.key.title_id, job.cue_row);
        }
    } else {
        was_stale = job.force_render ||
            state_tracker_.state(job.key) == FrameCacheState::Stale;
        state_tracker_.setState(job.key, FrameCacheState::Rendering);
    }

    /* A CPU/disk-resident frame does not need to be rerendered merely to become
     * a GPU token. Hydrate its sparse tiled payload on the cache worker and
     * publish it into the shared GPU tile cache. This also prevents each
     * live/editor session from accumulating its own 2D fallback textures after
     * an application restart. */
    if (!was_stale) {
        QImage resident_image;
        const bool found_resident = ram_cache_.get(job.key, resident_image) ||
            disk_cache_.get(job.key, resident_image);
        if (found_resident &&
            title_gpu_frame_cache_store_image(
                job.key.toString().toStdString(), resident_image,
                static_cast<uint32_t>(std::max(1, job.key.width)),
                static_cast<uint32_t>(std::max(1, job.key.height)))) {
            std::unique_lock<std::recursive_mutex> publication_lock(
                publication_mutex_);
            if (!jobIsCurrent(job)) {
                publication_lock.unlock();
                title_gpu_frame_cache_remove(
                    job.key.toString().toStdString());
                abandonJobState(job, live_state_key);
                return false;
            }

            state_tracker_.setState(job.key, FrameCacheState::CachedRam);
            if (job.live_cue) {
                std::lock_guard<std::recursive_mutex> live_lock(
                    live_cue_mutex_);
                live_cue_known_keys_.insert(job.key);
                live_cue_states_[live_state_key] =
                    FrameCacheState::NotCached;
                const FrameCacheState next_state =
                    liveCueStoredState(live_state_key);
                const int progress = liveCueStoredProgress(live_state_key);
                live_cue_states_[live_state_key] = next_state;
                live_cue_progress_percent_[live_state_key] = progress;
                if (job.cue_row >= 0 && shouldEmitLiveCueUpdate(
                        live_state_key, next_state, progress))
                    emit liveCueStateChanged(job.key.title_id,
                                             job.cue_row);
            } else {
                emit frameReady(job.key.title_id, job.key.frame);
            }
            publication_lock.unlock();
            emit diagnosticsChanged();
            return false;
        }
    }

    QImage previous;
    bool had_previous = ram_cache_.get(job.key, previous);
    if (!had_previous)
        had_previous = disk_cache_.get(job.key, previous);

    /* A visual edit produces a new content-addressed key. Looking up only the
     * new key made dirty-region regeneration degenerate into a full readback,
     * because the previous pixels still live under the last published visual
     * hash. Resolve that exact predecessor before deciding whether a GPU patch
     * is worthwhile. Live-cue variants keep their independent state keys and
     * conservatively use the full-frame path. */
    if (!had_previous && was_stale && !job.live_cue) {
        QString previous_hash;
        {
            QMutexLocker lock(&visual_hash_mutex_);
            previous_hash = last_visual_hash_by_title_.value(job.key.title_id);
        }
        if (!previous_hash.isEmpty() &&
            previous_hash != job.key.content_hash) {
            CacheFrameKey previous_key = job.key;
            previous_key.content_hash = previous_hash;
            had_previous = ram_cache_.get(previous_key, previous);
            if (!had_previous)
                had_previous = disk_cache_.get(previous_key, previous);
        }
    }

    QRect readback_region(0, 0, std::max(1, job.key.width),
                          std::max(1, job.key.height));
    if (was_stale && had_previous &&
        !title_requires_full_gpu_cache_frame(*job.title)) {
        const QVector<CacheTileRegion> dirty_tiles = dirtyTilesForKey(job.key);
        QRect dirty_union;
        for (const auto &tile : dirty_tiles)
            dirty_union = dirty_union.united(tile.rect);
        const qint64 full_pixels = qint64(readback_region.width()) *
                                   qint64(readback_region.height());
        const qint64 dirty_pixels = qint64(dirty_union.width()) *
                                    qint64(dirty_union.height());
        /* A bounded dirty union is staged as a GPU crop. Large or fragmented
         * updates are cheaper and safer as one full-frame staging copy. */
        if (!dirty_union.isEmpty() && full_pixels > 0 &&
            dirty_pixels * 100 < full_pixels * 60) {
            readback_region = dirty_union.intersected(readback_region);
        }
    }

    while (worker_pipeline_->pending_readbacks.size() >= 3)
        resolveOldestGpuReadback(true);

    TitleGpuReadbackTicket ticket;
    bool submitted = render_title_gpu_cache_submit_readback(
        *job.title, job.time, gpu_cache_model_revision(job.key),
        job.key.toString().toStdString(), readback_region, ticket);
    if (!submitted && hasPendingGpuReadbacks()) {
        resolveOldestGpuReadback(true);
        submitted = render_title_gpu_cache_submit_readback(
            *job.title, job.time, gpu_cache_model_revision(job.key),
            job.key.toString().toStdString(), readback_region, ticket);
    }
    if (!submitted) {
        if (retryFailedJob(job, live_state_key, "submit"))
            return true; // active token was completed and the job was requeued
        abandonJobState(job, live_state_key);
        state_tracker_.setState(job.key, FrameCacheState::Stale);
        BGL_LOG_ERROR("Prerender", QStringLiteral(
            "stage=submit action=failed-final title=%1 frame=%2 retries=%3")
            .arg(job.key.title_id).arg(job.key.frame).arg(job.retry_count));
        return false;
    }

    WorkerPipeline::PendingReadback pending;
    pending.job = std::move(job);
    pending.ticket = ticket;
    pending.previous = std::move(previous);
    pending.was_stale = was_stale;
    pending.had_previous = had_previous;
    pending.live_state_key = live_state_key;
    worker_pipeline_->pending_readbacks.push_back(std::move(pending));
    return true;
}

bool CacheManager::resolveOldestGpuReadback(bool force)
{
    Q_UNUSED(force);
    if (!hasPendingGpuReadbacks())
        return false;

    WorkerPipeline::PendingReadback pending =
        std::move(worker_pipeline_->pending_readbacks.front());
    worker_pipeline_->pending_readbacks.pop_front();

    QImage staged;
    const bool resolved = title_gpu_render_session_resolve_readback(
        pending.ticket, staged);
    RenderQueueManager::Job job = std::move(pending.job);
    struct QueueCompletionGuard {
        RenderQueueManager &queue;
        RenderQueueManager::Job &job;
        bool armed = true;
        ~QueueCompletionGuard() { if (armed) queue.complete(job); }
        void disarm() { armed = false; }
    } completion_guard {queue_, job};
    QString live_state_key = pending.live_state_key;

    if (!resolved || staged.isNull()) {
        title_gpu_frame_cache_remove(job.key.toString().toStdString());
        if (retryFailedJob(job, live_state_key, "resolve")) {
            completion_guard.disarm();
            emit diagnosticsChanged();
            return false;
        }
        abandonJobState(job, live_state_key);
        state_tracker_.setState(job.key, FrameCacheState::Stale);
        BGL_LOG_ERROR("Prerender", QStringLiteral(
            "stage=resolve action=failed-final title=%1 frame=%2 retries=%3")
            .arg(job.key.title_id).arg(job.key.frame).arg(job.retry_count));
        emit diagnosticsChanged();
        return false;
    }

    QImage image;
    const QRect full_rect(0, 0, std::max(1, job.key.width),
                          std::max(1, job.key.height));
    if (pending.was_stale && pending.had_previous &&
        pending.ticket.region != full_rect) {
        QImage merged = expand_sparse_frame(
            pending.previous, job.key.width, job.key.height);
        bgs::cache_frame_payload::Placement placement;
        const QImage patch = staged.format() == kDiskFrameFormat
            ? staged : staged.convertToFormat(kDiskFrameFormat);
        if (!merged.isNull() && !patch.isNull() &&
            bgs::cache_frame_payload::read_placement(patch, placement) &&
            placement.canvas_width == job.key.width &&
            placement.canvas_height == job.key.height) {
            const qsizetype row_bytes = qsizetype(patch.width()) * 4;
            for (int y = 0; y < patch.height(); ++y) {
                std::memcpy(
                    merged.scanLine(placement.y + y) +
                        qsizetype(placement.x) * 4,
                    patch.constScanLine(y), row_bytes);
            }
            image = make_sparse_frame(merged, job.key.width, job.key.height);
        }
    } else {
        image = make_sparse_frame(staged, job.key.width, job.key.height);
    }

    if (image.isNull()) {
        title_gpu_frame_cache_remove(job.key.toString().toStdString());
        if (retryFailedJob(job, live_state_key, "payload")) {
            completion_guard.disarm();
            emit diagnosticsChanged();
            return false;
        }
        abandonJobState(job, live_state_key);
        state_tracker_.setState(job.key, FrameCacheState::Stale);
        BGL_LOG_ERROR("Prerender", QStringLiteral(
            "stage=payload action=failed-final title=%1 frame=%2 retries=%3")
            .arg(job.key.title_id).arg(job.key.frame).arg(job.retry_count));
        emit diagnosticsChanged();
        return false;
    }

    /* Publish the final sparse payload into the shared GPU RAM cache only
     * after readback resolution/dirty-patch merge. The cache decomposes it into
     * aligned content-addressed tiles, skips transparent tiles and reuses
     * unchanged tile textures across frames. */
    const bool gpu_ram_stored = title_gpu_frame_cache_store_image(
        job.key.toString().toStdString(), image,
        static_cast<uint32_t>(std::max(1, job.key.width)),
        static_cast<uint32_t>(std::max(1, job.key.height)));

    {
        QMutexLocker lock(&dirty_tiles_mutex_);
        dirty_tiles_by_frame_.remove(tileStateKey(job.key.title_id, job.key.frame));
    }

    std::unique_lock<std::recursive_mutex> publication_lock(publication_mutex_);
    if (!jobIsCurrent(job)) {
        publication_lock.unlock();
        title_gpu_frame_cache_remove(job.key.toString().toStdString());
        abandonJobState(job, live_state_key);
        return false;
    }

    std::unique_lock<std::recursive_mutex> live_publish_lock;
    QVector<QString> publish_live_states;
    if (job.live_cue) {
        live_publish_lock = std::unique_lock<std::recursive_mutex>(live_cue_mutex_);
        const QString rebound_state = liveCueStateReferencingKey(job.key, live_state_key);
        if (rebound_state.isEmpty()) {
            publication_lock.unlock();
            title_gpu_frame_cache_remove(job.key.toString().toStdString());
            return false;
        }
        live_state_key = rebound_state;
        job.cue_state_key = rebound_state;
        job.cue_row = live_cue_rows_.value(rebound_state, job.cue_row);
        publish_live_states.push_back(rebound_state);
        for (auto it = live_cue_required_keys_.cbegin();
             it != live_cue_required_keys_.cend(); ++it) {
            if (it.key() == rebound_state ||
                live_cue_title_ids_.value(it.key()) != job.key.title_id)
                continue;
            if (std::find(it.value().cbegin(), it.value().cend(), job.key) !=
                it.value().cend())
                publish_live_states.push_back(it.key());
        }
    }

    /* The readback image is not retained by RamFrameCache. Its pixels have
     * already been offered to the sparse GPU tile cache; the same payload now
     * continues asynchronously to the bounded SSD compression writer. */
    disk_cache_.enqueuePut(job.key, image);

    if (!job.live_cue && !job.realtime)
        rememberVisualHash(*job.title, job.key.content_hash);

    if (job.live_cue) {
        live_cue_known_keys_.insert(job.key);
        for (const QString &state_key : publish_live_states) {
            const int signal_row = live_cue_rows_.value(state_key, job.cue_row);
            if (job.force_render && live_cue_total_by_key_.contains(state_key)) {
                const int total = std::max(1, live_cue_total_by_key_.value(state_key));
                live_cue_done_by_key_[state_key] = std::min(
                    total, live_cue_done_by_key_.value(state_key, 0) + 1);
            }
            const int progress = liveCueStoredProgress(state_key);
            live_cue_states_[state_key] = FrameCacheState::NotCached;
            FrameCacheState next_state = liveCueStoredState(state_key);
            const bool manual_rebuild_active =
                live_cue_total_by_key_.contains(state_key) &&
                live_cue_done_by_key_.value(state_key, 0) <
                    live_cue_total_by_key_.value(state_key);
            if (manual_rebuild_active) {
                next_state = FrameCacheState::Rendering;
            } else if (live_cue_total_by_key_.contains(state_key)) {
                live_cue_total_by_key_.remove(state_key);
                live_cue_done_by_key_.remove(state_key);
                next_state = liveCueStoredState(state_key);
            }
            live_cue_states_[state_key] = next_state;
            live_cue_progress_percent_[state_key] = progress;
            if (signal_row >= 0 &&
                shouldEmitLiveCueUpdate(state_key, next_state, progress))
                emit liveCueStateChanged(job.key.title_id, signal_row);
        }
    } else {
        state_tracker_.setState(
            job.key, gpu_ram_stored
                ? FrameCacheState::CachedRam
                : FrameCacheState::CachedDisk);
        emit frameReady(job.key.title_id, job.key.frame);
    }
    emit diagnosticsChanged();
    return true;
}
