# Unified Blur Effects Fix

Drop Shadow, Glow, Inner Glow, Inner Shadow, Bloom, the Blur effect, and general transition blur now share the same fast separable Gaussian blur backend. The blur shader owns both the common Gaussian generation pass and the effect-specific compositing techniques, removing the broken dependency on separate shadow/glow/bloom shaders receiving an externally generated blurred texture. GPU effect cache keys were versioned to invalidate stale pre-fix results.
