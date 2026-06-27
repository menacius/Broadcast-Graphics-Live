# Development Version 012

## Adjustment blur editor correction

The editor transparency checkerboard is presentation chrome and is no longer uploaded as the destination texture used by the title compositor. The GPU compositor renders the title against transparency, allowing Adjustment Layers to sample the complete accumulated artwork beneath them without sampling the checkerboard.

After the transparent result is read back, the checkerboard is composited underneath with SourceOver. This preserves a sharp checkerboard while retaining blur tails, shadows, glows and other effect-generated pixels outside the original artwork bounds.

The OBS Preview/Program compositor is unchanged by this editor-only presentation correction.
