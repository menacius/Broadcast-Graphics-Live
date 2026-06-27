# Development Version 009 — Adjustment mask compile fix

MSVC reported `C3861: apply_gpu_mask: identifier not found` because the adjustment coverage function called `apply_gpu_mask()` before its definition appeared in `title-source.cpp`.

A matching static forward declaration is now placed beside the other GPU mask graph declarations, preserving internal linkage and the existing implementation.
