# Development Version 013

## Adjustment blur
The adjustment effect stack is evaluated from the complete transparent artwork composite below the adjustment layer. The coverage texture is applied only when mixing the effected frame back into the original frame. The mix shader now uses the effected frame alpha inside coverage, preventing blur kernels from being clipped to the original pixel alpha. The editor checkerboard remains presentation chrome and is composited after GPU artwork rendering.

## Shutdown hardening
The frontend exit event now marks title sources as shutting down before nested OBS scenes are dismantled. Runtime scene-mask lifecycle references remain balanced. During final process teardown, source destruction releases its strong scene references without invoking active/showing transitions that may synchronously enter partially destroyed scene children.
