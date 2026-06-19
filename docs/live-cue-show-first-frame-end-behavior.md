# Live cue: Show first frame end behavior

The title Properties panel now exposes a third **When cue ends** mode: **Show first frame**.

The outro and delayed-uncue lifecycle are unchanged. After the outro reaches its final frame, the live cue row is cleared and the OBS source remains visible at timeline frame zero. The same final state is used when a play-once live cue reaches the end naturally. The value is serialized as `cue_end_behavior = 2` and is included in the cache content identity.
