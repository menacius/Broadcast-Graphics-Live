#pragma once

#ifndef PLUGIN_VERSION
#define PLUGIN_VERSION "0.8.8-alpha"
#endif

// Manually assigned delivery identifier. Increment this once per delivered
// package, never per local compile.
#ifndef BGL_DEVELOPMENT_VERSION
#define BGL_DEVELOPMENT_VERSION "144"
#endif
#define BGL_DEVELOPMENT_DISPLAY "Development Version " BGL_DEVELOPMENT_VERSION
#define BGL_BUILD_DISPLAY "v" PLUGIN_VERSION " · " BGL_DEVELOPMENT_DISPLAY
#define BGL_FULL_VERSION_DISPLAY PLUGIN_VERSION " · " BGL_DEVELOPMENT_DISPLAY
