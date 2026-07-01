#pragma once

#include <string>

struct Layer;
struct Title;

namespace bgs::asset_runtime {

/* Detects timeline-driven animation that can meaningfully be synchronized to
 * a parent title. Clock/ticker layers keep their own runtime clocks and do not
 * make an otherwise static asset expose synchronized/independent controls. */
bool layer_has_timeline_animation(const Layer &layer);
bool title_has_timeline_animation(const Title &title);
bool asset_layer_has_timeline_animation(const Title &title,
                                         const Layer &asset_layer);

/* Deterministic mapping used by the runtime and unit tests. elapsed_seconds is
 * the independent monotonic clock before applying the instance time offset. */
double map_elapsed_to_local_time(const Layer &asset_layer,
                                 double elapsed_seconds);

/* Returns the Asset Layer's local timeline time. Synchronized instances use
 * the supplied parent time; independent instances use a per-instance
 * monotonic clock that is completely detached from the parent playhead. */
double resolve_local_time(const std::string &title_id, const Layer &asset_layer,
                          double synchronized_time);

void reset_layer(const std::string &title_id, const std::string &layer_id);
void clear_title(const std::string &title_id);
void clear_all();

} // namespace bgs::asset_runtime
