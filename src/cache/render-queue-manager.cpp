#include "cache-manager.h"

#include <QMutexLocker>

#include <algorithm>
#include <cmath>

RenderQueueManager::RenderQueueManager(QObject *parent) : QObject(parent) {}

bool RenderQueueManager::enqueue(const Job &job)
{
    if (!job.title) return false;
    const QString key = job.key.toString();
    bool changed = false;
    {
        QMutexLocker lock(&mutex_);
        if (!accepting_jobs_)
            return false;
        const QString token = QStringLiteral("%1:%2")
            .arg(job.cache_epoch)
            .arg(job.title_generation);
        if (active_tokens_.value(key) == token)
            return false;
        for (Job &queued : jobs_) {
            if (queued.key.toString() != key)
                continue;
            /* A realtime/live-cue request must be allowed to promote an existing
             * background prerender job for the same content-addressed frame. */
            if (job.live_cue && !queued.live_cue) {
                queued.live_cue = true;
                queued.force_render = queued.force_render || job.force_render;
                queued.cue_row = job.cue_row;
                queued.cue_state_key = job.cue_state_key;
                queued.title = job.title;
                queued.time = job.time;
                queued.cache_epoch = job.cache_epoch;
                queued.title_generation = job.title_generation;
                changed = true;
            }
            if (job.realtime && !queued.realtime) {
                queued.realtime = true;
                queued.title = job.title;
                queued.time = job.time;
                queued.cache_epoch = job.cache_epoch;
                queued.title_generation = job.title_generation;
                changed = true;
            }
            if (job.urgent && !queued.urgent) {
                queued.urgent = true;
                changed = true;
            }
            if (job.force_render && !queued.force_render) {
                queued.force_render = true;
                changed = true;
            }
            if (job.priority < queued.priority) {
                queued.priority = job.priority;
                changed = true;
            }
            if (changed)
                std::sort(jobs_.begin(), jobs_.end(), [](const Job &a, const Job &b) {
                    if (a.priority != b.priority) return a.priority < b.priority;
                    return a.key.frame < b.key.frame;
                });
            break;
        }
        if (!queued_keys_.contains(key)) {
            jobs_.push_back(job);
            queued_keys_.insert(key);
            std::sort(jobs_.begin(), jobs_.end(), [](const Job &a, const Job &b) {
                if (a.priority != b.priority) return a.priority < b.priority;
                return a.key.frame < b.key.frame;
            });
            changed = true;
        }
    }
    if (changed)
        emit queueChanged();
    return changed;
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
        const QString key = job.key.toString();
        queued_keys_.remove(key);
        active_tokens_[key] = QStringLiteral("%1:%2")
            .arg(job.cache_epoch)
            .arg(job.title_generation);
    }
    emit queueChanged();
    return true;
}

bool RenderQueueManager::takeNextUrgent(Job &job)
{
    bool found = false;
    {
        QMutexLocker lock(&mutex_);
        for (auto it = jobs_.begin(); it != jobs_.end(); ++it) {
            if (!it->urgent)
                continue;
            job = *it;
            const QString key = it->key.toString();
            queued_keys_.remove(key);
            active_tokens_[key] = QStringLiteral("%1:%2")
                .arg(job.cache_epoch)
                .arg(job.title_generation);
            jobs_.erase(it);
            found = true;
            break;
        }
    }
    if (found)
        emit queueChanged();
    return found;
}


bool RenderQueueManager::takeNextForTitle(const QString &title_id, Job &job)
{
    if (title_id.isEmpty())
        return false;
    bool found = false;
    {
        QMutexLocker lock(&mutex_);
        for (auto it = jobs_.begin(); it != jobs_.end(); ++it) {
            if (it->key.title_id != title_id || it->live_cue)
                continue;
            job = *it;
            const QString key = it->key.toString();
            queued_keys_.remove(key);
            active_tokens_[key] = QStringLiteral("%1:%2")
                .arg(job.cache_epoch)
                .arg(job.title_generation);
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

bool RenderQueueManager::hasAvailableJob(bool live_cue_only) const
{
    QMutexLocker lock(&mutex_);
    if (!live_cue_only)
        return !jobs_.isEmpty();
    return std::any_of(jobs_.cbegin(), jobs_.cend(), [](const Job &job) {
        return job.live_cue || job.realtime;
    });
}


bool RenderQueueManager::hasAvailableJobForTitle(const QString &title_id) const
{
    if (title_id.isEmpty())
        return false;
    QMutexLocker lock(&mutex_);
    return std::any_of(jobs_.cbegin(), jobs_.cend(), [&title_id](const Job &job) {
        return job.key.title_id == title_id && !job.live_cue;
    });
}

void RenderQueueManager::complete(const Job &job)
{
    {
        QMutexLocker lock(&mutex_);
        const QString key = job.key.toString();
        const QString token = QStringLiteral("%1:%2")
            .arg(job.cache_epoch)
            .arg(job.title_generation);
        if (active_tokens_.value(key) == token)
            active_tokens_.remove(key);
    }
    emit queueChanged();
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
