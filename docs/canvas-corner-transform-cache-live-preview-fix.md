# Canvas corner transform cache live-preview fix

- Corner-radius handle drags now bypass the frame cache while the pointer is moving, matching the immediate editor feedback of move, resize, rotate, and gradient transforms.
- The normal cached playback path resumes as soon as the corner drag ends.
- Shape geometry and corner-style fields are now included in the cache content hash so completed corner edits cannot reuse stale RAM or disk frames.
