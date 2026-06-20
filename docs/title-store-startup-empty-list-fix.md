# Title Store Startup Empty List Fix

## Problem

On some OBS startups, the title/graphics dock appeared empty even though the scene collection had saved titles. The startup sequence loaded the titles successfully, then OBS emitted a scene-collection cleanup event before frontend initialization completed. That event triggered an unnecessary save. The previous Windows replacement path deleted the valid destination before renaming the temporary file, so a transient lock could leave no titles file. The subsequent reload then treated the missing file as an intentionally empty collection.

## Changes

- Replaced the manual temporary-file/`std::rename` sequence with `QSaveFile` atomic commits.
- Disabled direct-write fallback so a failed commit never truncates or removes the last valid titles file.
- Skipped scene-collection cleanup saves during initial frontend startup while preserving saves for real collection switches.
- Preserved already loaded in-memory titles when reloading the same scene collection encounters a transient read or parse failure.
- Kept normal empty-store behavior when switching to a genuinely different collection with no saved titles file.
