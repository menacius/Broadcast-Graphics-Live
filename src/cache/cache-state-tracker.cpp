#include "cache-manager.h"

#include <QMutexLocker>

#include <algorithm>
#include <limits>

CacheStateTracker::CacheStateTracker(QObject *parent) : QObject(parent) {}

FrameCacheState CacheStateTracker::state(const CacheFrameKey &key) const
{
    QMutexLocker lock(&mutex_);
    return states_.value(key, FrameCacheState::NotCached);
}

FrameCacheState CacheStateTracker::stateForFrame(const QString &title_id, int frame) const
{
    auto rank = [](FrameCacheState state) {
        switch (state) {
        case FrameCacheState::Rendering: return 6;
        case FrameCacheState::Queued: return 5;
        case FrameCacheState::CachedRam: return 4;
        case FrameCacheState::CachedDisk: return 3;
        case FrameCacheState::Stale: return 2;
        case FrameCacheState::Disabled: return 1;
        case FrameCacheState::NotCached: return 0;
        }
        return 0;
    };

    QMutexLocker lock(&mutex_);
    FrameCacheState best = FrameCacheState::NotCached;
    int best_rank = 0;
    for (auto it = states_.cbegin(); it != states_.cend(); ++it) {
        if (it.key().title_id != title_id || it.key().frame != frame)
            continue;
        const int candidate_rank = rank(it.value());
        if (candidate_rank > best_rank) {
            best = it.value();
            best_rank = candidate_rank;
            if (best == FrameCacheState::Rendering)
                break;
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

void CacheStateTracker::resetTransient(const QString &title_id)
{
    QHash<QString, QPair<int, int>> changed_ranges;
    {
        QMutexLocker lock(&mutex_);
        for (auto it = states_.begin(); it != states_.end(); ++it) {
            if (!title_id.isEmpty() && it.key().title_id != title_id)
                continue;
            if (it.value() != FrameCacheState::Queued && it.value() != FrameCacheState::Rendering)
                continue;

            const QString changed_title = it.key().title_id;
            auto range_it = changed_ranges.find(changed_title);
            if (range_it == changed_ranges.end()) {
                changed_ranges.insert(changed_title, qMakePair(it.key().frame, it.key().frame));
            } else {
                range_it.value().first = std::min(range_it.value().first, it.key().frame);
                range_it.value().second = std::max(range_it.value().second, it.key().frame);
            }
            it.value() = FrameCacheState::NotCached;
        }
    }
    for (auto it = changed_ranges.cbegin(); it != changed_ranges.cend(); ++it)
        emit stateChanged(it.key(), it.value().first, it.value().second);
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
