cmake_minimum_required(VERSION 3.16)
project(broadcast-graphics-live VERSION 0.8.4)

# Public development version shown in the About dialog and diagnostic logs.
# Keep PROJECT_VERSION numeric for CMake/package compatibility and append the
# prerelease channel separately.
set(OBS_BGS_PRERELEASE "alpha")
set(OBS_BGS_VERSION "${PROJECT_VERSION}-${OBS_BGS_PRERELEASE}")

# ── C++ standard ────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── OBS plugin packaging layout ─────────────────────────────────────
# OBS' modern manual-install layout is:
#   <plugins-root>/<plugin-name>/bin/<arch>/<plugin binary>
#   <plugins-root>/<plugin-name>/data/locale/*.ini
# Keeping build artifacts in this shape makes the build tree directly copyable
# into the OBS plugins directory and matches the Windows ProgramData layout.
set(OBS_TITLES_PLUGIN_NAME "broadcast-graphics-live")
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(OBS_TITLES_ARCH_DIR "64bit")
  set(OBS_TITLES_VCPKG_DEFAULT_TRIPLET "x64-windows")
else()
  set(OBS_TITLES_ARCH_DIR "32bit")
  set(OBS_TITLES_VCPKG_DEFAULT_TRIPLET "x86-windows")
endif()
set(OBS_TITLES_PACKAGE_DIR
    "${CMAKE_BINARY_DIR}/${OBS_TITLES_PLUGIN_NAME}"
    CACHE PATH "Staging directory for the OBS user-plugin folder layout")
set(OBS_TITLES_PACKAGE_BIN_DIR
    "${OBS_TITLES_PACKAGE_DIR}/bin/${OBS_TITLES_ARCH_DIR}")

# ── Qt auto-tools ───────────────────────────────────────────────────
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# ── Find OBS ────────────────────────────────────────────────────────
# Try to find standard CMake config package first (e.g. from CMAKE_PREFIX_PATH)
find_package(libobs QUIET)
find_package(obs-frontend-api QUIET)

if(NOT libobs_FOUND OR NOT obs-frontend-api_FOUND)
  # Allow callers to point at an OBS SDK/install tree without baking in
  # machine-specific paths. CMake cache values take precedence over
  # environment variables so CI and developers can override discovery.
  set(OBS_SEARCH_ROOTS)
  if(OBS_SDK_DIR)
    list(APPEND OBS_SEARCH_ROOTS "${OBS_SDK_DIR}")
  endif()
  if(DEFINED ENV{OBS_SDK_DIR})
    list(APPEND OBS_SEARCH_ROOTS "$ENV{OBS_SDK_DIR}")
  endif()
  if(DEFINED ENV{OBS_STUDIO_DIR})
    list(APPEND OBS_SEARCH_ROOTS "$ENV{OBS_STUDIO_DIR}")
  endif()

  if(WIN32)
    foreach(base_dir
        "$ENV{USERPROFILE}/Desktop/obs-build-dependencies"
        "$ENV{USERPROFILE}/Downloads/obs-build-dependencies"
        "${CMAKE_SOURCE_DIR}/obs-build-dependencies")
      if(IS_DIRECTORY "${base_dir}")
        file(GLOB plugin_deps "${base_dir}/plugin-deps-*")
        list(SORT plugin_deps ORDER DESCENDING)
        list(APPEND OBS_SEARCH_ROOTS ${plugin_deps})
      endif()
    endforeach()

    if(DEFINED ENV{ProgramFiles})
      list(APPEND OBS_SEARCH_ROOTS "$ENV{ProgramFiles}/obs-studio")
    endif()
    if(DEFINED ENV{ProgramW6432})
      list(APPEND OBS_SEARCH_ROOTS "$ENV{ProgramW6432}/obs-studio")
    endif()
  endif()

  list(REMOVE_DUPLICATES OBS_SEARCH_ROOTS)

  foreach(candidate IN LISTS OBS_SEARCH_ROOTS)
    if(NOT IS_DIRECTORY "${candidate}")
      continue()
    endif()

    unset(OBS_LIB CACHE)
    unset(OBS_LIB)
    unset(OBS_FRONTEND_API_LIB CACHE)
    unset(OBS_FRONTEND_API_LIB)
    unset(OBS_INCLUDE_DIR CACHE)
    unset(OBS_INCLUDE_DIR)
    unset(OBS_DLL CACHE)
    unset(OBS_DLL)
    unset(OBS_FRONTEND_API_DLL CACHE)
    unset(OBS_FRONTEND_API_DLL)

    find_library(OBS_LIB NAMES obs
      HINTS "${candidate}/lib" "${candidate}/lib/obs"
      NO_DEFAULT_PATH)
    find_library(OBS_FRONTEND_API_LIB NAMES obs-frontend-api
      HINTS "${candidate}/lib" "${candidate}/lib/obs"
      NO_DEFAULT_PATH)
    find_path(OBS_INCLUDE_DIR NAMES obs.h
      HINTS "${candidate}/include" "${candidate}/include/obs"
      NO_DEFAULT_PATH)

    if(OBS_LIB AND OBS_FRONTEND_API_LIB AND OBS_INCLUDE_DIR)
      set(OBS_SDK_DIR "${candidate}" CACHE PATH "Path to OBS Studio SDK" FORCE)
      message(STATUS "Found OBS SDK at: ${OBS_SDK_DIR}")
      list(APPEND CMAKE_PREFIX_PATH "${OBS_SDK_DIR}")

      # Find DLLs to specify IMPORTED_LOCATION and prevent CMP0111 warnings.
      find_file(OBS_DLL NAMES obs.dll
        HINTS "${OBS_SDK_DIR}/bin/64bit" "${OBS_SDK_DIR}/bin"
        NO_DEFAULT_PATH)
      find_file(OBS_FRONTEND_API_DLL NAMES obs-frontend-api.dll
        HINTS "${OBS_SDK_DIR}/bin/64bit" "${OBS_SDK_DIR}/bin"
        NO_DEFAULT_PATH)

      if(NOT TARGET OBS::libobs)
        add_library(OBS::libobs SHARED IMPORTED GLOBAL)
        set_target_properties(OBS::libobs PROPERTIES
          INTERFACE_INCLUDE_DIRECTORIES "${OBS_INCLUDE_DIR}"
        )
        if(WIN32)
          set_target_properties(OBS::libobs PROPERTIES IMPORTED_IMPLIB "${OBS_LIB}")
          if(OBS_DLL)
            set_target_properties(OBS::libobs PROPERTIES IMPORTED_LOCATION "${OBS_DLL}")
          endif()
        else()
          set_target_properties(OBS::libobs PROPERTIES IMPORTED_LOCATION "${OBS_LIB}")
        endif()
      endif()
      if(NOT TARGET OBS::obs-frontend-api)
        add_library(OBS::obs-frontend-api SHARED IMPORTED GLOBAL)
        set_target_properties(OBS::obs-frontend-api PROPERTIES
          INTERFACE_INCLUDE_DIRECTORIES "${OBS_INCLUDE_DIR}"
        )
        if(WIN32)
          set_target_properties(OBS::obs-frontend-api PROPERTIES IMPORTED_IMPLIB "${OBS_FRONTEND_API_LIB}")
          if(OBS_FRONTEND_API_DLL)
            set_target_properties(OBS::obs-frontend-api PROPERTIES IMPORTED_LOCATION "${OBS_FRONTEND_API_DLL}")
          endif()
        else()
          set_target_properties(OBS::obs-frontend-api PROPERTIES IMPORTED_LOCATION "${OBS_FRONTEND_API_LIB}")
        endif()
      endif()
      if(EXISTS "${OBS_SDK_DIR}/cmake/Qt6")
        set(Qt6_DIR "${OBS_SDK_DIR}/cmake/Qt6" CACHE PATH "Qt6 cmake dir from OBS" FORCE)
      endif()
      set(libobs_FOUND TRUE)
      set(obs-frontend-api_FOUND TRUE)
      break()
    endif()

    unset(OBS_LIB CACHE)
    unset(OBS_LIB)
    unset(OBS_FRONTEND_API_LIB CACHE)
    unset(OBS_FRONTEND_API_LIB)
    unset(OBS_INCLUDE_DIR CACHE)
    unset(OBS_INCLUDE_DIR)
  endforeach()
endif()

if(NOT libobs_FOUND OR NOT obs-frontend-api_FOUND)
  # Fallback to finding them normally if we didn't resolve them manually
  find_package(libobs REQUIRED)
  find_package(obs-frontend-api REQUIRED)
endif()

# ── Find Qt ─────────────────────────────────────────────────────────
find_package(Qt6 COMPONENTS Core Widgets Svg QUIET)
if(NOT Qt6_FOUND)
  find_package(Qt5 5.15 COMPONENTS Core Widgets Svg REQUIRED)
  set(QT_VERSION_MAJOR 5)
else()
  set(QT_VERSION_MAJOR 6)
endif()

set(QT_LIBS Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::Svg)

# ── Find Cairo + Pango ──────────────────────────────────────────────
find_package(PkgConfig QUIET)
set(OBS_BGS_TEXT_RENDER_TARGETS "")
if(PKG_CONFIG_FOUND)
  # IMPORTED_TARGET is important on Windows/MSVC: it preserves the vcpkg
  # library directories and transitive link dependencies. Linking the raw
  # *_LIBRARIES values can otherwise emit only `cairo.lib`, with no /LIBPATH.
  pkg_check_modules(CAIRO QUIET IMPORTED_TARGET cairo)
  pkg_check_modules(PANGO QUIET IMPORTED_TARGET pango)
  pkg_check_modules(PANGOCAIRO QUIET IMPORTED_TARGET pangocairo)
  if(TARGET PkgConfig::CAIRO AND TARGET PkgConfig::PANGO AND TARGET PkgConfig::PANGOCAIRO)
    list(APPEND OBS_BGS_TEXT_RENDER_TARGETS
      PkgConfig::CAIRO
      PkgConfig::PANGO
      PkgConfig::PANGOCAIRO)
  endif()
endif()

if(NOT CAIRO_FOUND OR NOT PANGO_FOUND OR NOT PANGOCAIRO_FOUND)
  message(STATUS "PkgConfig not found or missing components. Falling back to native search...")

  find_path(CAIRO_INCLUDE_DIR NAMES cairo.h PATH_SUFFIXES cairo)
  find_library(CAIRO_LIBRARY NAMES cairo)

  find_path(PANGO_INCLUDE_DIR NAMES pango/pango.h PATH_SUFFIXES pango-1.0)
  find_library(PANGO_LIBRARY NAMES pango-1.0)

  find_path(PANGOCAIRO_INCLUDE_DIR NAMES pango/pangocairo.h PATH_SUFFIXES pango-1.0)
  find_library(PANGOCAIRO_LIBRARY NAMES pangocairo-1.0)

  find_path(GLIB_INCLUDE_DIR NAMES glib.h PATH_SUFFIXES glib-2.0)
  if(GLIB_INCLUDE_DIR)
    get_filename_component(GLIB_PARENT_DIR "${GLIB_INCLUDE_DIR}" DIRECTORY)
    get_filename_component(GLIB_GRANDPARENT_DIR "${GLIB_PARENT_DIR}" DIRECTORY)
    find_path(GLIB_CONFIG_INCLUDE_DIR NAMES glibconfig.h
      HINTS "${GLIB_PARENT_DIR}/lib" "${GLIB_GRANDPARENT_DIR}/lib"
      PATH_SUFFIXES glib-2.0/include
    )
  else()
    find_path(GLIB_CONFIG_INCLUDE_DIR NAMES glibconfig.h PATH_SUFFIXES glib-2.0/include)
  endif()
  find_library(GLIB_LIBRARY NAMES glib-2.0)
  find_library(GOBJECT_LIBRARY NAMES gobject-2.0)
  find_path(HARFBUZZ_INCLUDE_DIR NAMES hb.h PATH_SUFFIXES harfbuzz)
  find_library(HARFBUZZ_LIBRARY NAMES harfbuzz)

  if(CAIRO_INCLUDE_DIR AND CAIRO_LIBRARY AND PANGO_INCLUDE_DIR AND PANGO_LIBRARY AND PANGOCAIRO_LIBRARY)
    set(CAIRO_FOUND TRUE)
    get_filename_component(CAIRO_PARENT_DIR "${CAIRO_INCLUDE_DIR}" DIRECTORY)
    set(CAIRO_INCLUDE_DIRS ${CAIRO_INCLUDE_DIR} ${CAIRO_PARENT_DIR})
    set(CAIRO_LIBRARIES ${CAIRO_LIBRARY})

    set(PANGO_FOUND TRUE)
    get_filename_component(PANGO_PARENT_DIR "${PANGO_INCLUDE_DIR}" DIRECTORY)
    set(PANGO_INCLUDE_DIRS ${PANGO_INCLUDE_DIR} ${PANGO_PARENT_DIR} ${GLIB_INCLUDE_DIR} ${GLIB_CONFIG_INCLUDE_DIR})
    if(HARFBUZZ_INCLUDE_DIR)
      list(APPEND PANGO_INCLUDE_DIRS ${HARFBUZZ_INCLUDE_DIR})
    endif()
    set(PANGO_LIBRARIES ${PANGO_LIBRARY} ${GLIB_LIBRARY} ${GOBJECT_LIBRARY})
    if(HARFBUZZ_LIBRARY)
      list(APPEND PANGO_LIBRARIES ${HARFBUZZ_LIBRARY})
    endif()

    set(PANGOCAIRO_FOUND TRUE)
    get_filename_component(PANGOCAIRO_PARENT_DIR "${PANGOCAIRO_INCLUDE_DIR}" DIRECTORY)
    set(PANGOCAIRO_INCLUDE_DIRS ${PANGOCAIRO_INCLUDE_DIR} ${PANGOCAIRO_PARENT_DIR})
    set(PANGOCAIRO_LIBRARIES ${PANGOCAIRO_LIBRARY})

    add_library(OBS_BGS_CAIRO UNKNOWN IMPORTED)
    set_target_properties(OBS_BGS_CAIRO PROPERTIES
      IMPORTED_LOCATION "${CAIRO_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${CAIRO_INCLUDE_DIRS}")
    add_library(OBS_BGS_PANGO UNKNOWN IMPORTED)
    set_target_properties(OBS_BGS_PANGO PROPERTIES
      IMPORTED_LOCATION "${PANGO_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${PANGO_INCLUDE_DIRS}"
      INTERFACE_LINK_LIBRARIES "${GLIB_LIBRARY};${GOBJECT_LIBRARY};${HARFBUZZ_LIBRARY}")
    add_library(OBS_BGS_PANGOCAIRO UNKNOWN IMPORTED)
    set_target_properties(OBS_BGS_PANGOCAIRO PROPERTIES
      IMPORTED_LOCATION "${PANGOCAIRO_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${PANGOCAIRO_INCLUDE_DIRS}"
      INTERFACE_LINK_LIBRARIES "OBS_BGS_CAIRO;OBS_BGS_PANGO")
    list(APPEND OBS_BGS_TEXT_RENDER_TARGETS
      OBS_BGS_CAIRO OBS_BGS_PANGO OBS_BGS_PANGOCAIRO)

    message(STATUS "Native Cairo found: ${CAIRO_LIBRARY}")
    message(STATUS "Native Pango found: ${PANGO_LIBRARY}")
    message(STATUS "Native PangoCairo found: ${PANGOCAIRO_LIBRARY}")
  else()
    message(FATAL_ERROR "Could not find Cairo, Pango, or PangoCairo. Please install dependencies.")
  endif()
endif()

# ── nlohmann/json ────────────────────────────────────────────────────
# Either install via package manager:  apt install nlohmann-json3-dev
# or rely on FetchContent:
include(FetchContent)
FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
)
FetchContent_MakeAvailable(nlohmann_json)

# QtColorWidgets provides the OBS-themed color wheel/dialog used by all editor
# color pickers. Keep the tag pinned so clean builds stay reproducible.
FetchContent_Declare(
  QtColorWidgets
  GIT_REPOSITORY https://gitlab.com/mattbas/Qt-Color-Widgets.git
  GIT_TAG 8491078434b24cba295b5e41cc0d2a94c7049a5b
  SOURCE_SUBDIR cmake-do-not-add
  GIT_PROGRESS FALSE
)
FetchContent_MakeAvailable(QtColorWidgets)

set(OBS_BGS_QTCOLORWIDGETS_ROOT "${qtcolorwidgets_SOURCE_DIR}")
set(OBS_BGS_QTCOLORWIDGETS_SOURCES
    "${OBS_BGS_QTCOLORWIDGETS_ROOT}/src/QtColorWidgets/abstract_widget_list.cpp"
    "${OBS_BGS_QTCOLORWIDGETS_ROOT}/src/QtColorWidgets/bound_color_selector.cpp"
    "${OBS_BGS_QTCOLORWIDGETS_ROOT}/src/QtColorWidgets/color_2d_slider.cpp"
    "${OBS_BGS_QTCOLORWIDGETS_ROOT}/src/QtColorWidgets/color_delegate.cpp"
    "${OBS_BGS_QTCOLORWIDGETS_ROOT}/src/QtColorWidgets/color_dialog.cpp"
    "${OBS_BGS_QTCOLORWIDGETS_ROOT}/src/QtColorWidgets/color_dialog.ui"
    "${OBS_BGS_QTCOLORWIDGETS_ROOT}/src/QtColorWidgets/color_line_edit.cpp"
    "${OBS_BGS_QTCOLORWIDGETS_ROOT}/src/QtColorWidgets/color_list_widget.cpp"
    "${OBS_BGS_QTCOLORWIDGETS_ROOT}/src/QtColorWidgets/color_names.cpp"
    "${OBS_BGS_QTCOLORWIDGETS_ROOT}/src/QtColorWidgets/color_palette.cpp"
    "${OBS_BGS_QTCOLORWIDGETS_ROOT}/src/QtColorWidgets/color_palette_model.cpp"
    "${OBS_BGS_QTCOLORWIDGETS_ROOT}/src/QtColorWidgets/color_palette_widget.cpp"
    "${OBS_BGS_QTCOLORWIDGETS_ROOT}/src/QtColorWidgets/color_palette_widget.ui"
    "${OBS_BGS_QTCOLORWIDGETS_ROOT}/src/QtColorWidgets/color_preview.cpp"
    "${OBS_BGS_QTCOLORWIDGETS_ROOT}/src/QtColorWidgets/color_selector.cpp"
    "${OBS_BGS_QTCOLORWIDGETS_ROOT}/src/QtColorWidgets/color_utils.cpp"
    "${OBS_BGS_QTCOLORWIDGETS_ROOT}/src/QtColorWidgets/color_wheel.cpp"
    "${OBS_BGS_QTCOLORWIDGETS_ROOT}/src/QtColorWidgets/gradient_editor.cpp"
    "${OBS_BGS_QTCOLORWIDGETS_ROOT}/src/QtColorWidgets/gradient_list_model.cpp"
    "${OBS_BGS_QTCOLORWIDGETS_ROOT}/src/QtColorWidgets/gradient_slider.cpp"
    "${OBS_BGS_QTCOLORWIDGETS_ROOT}/src/QtColorWidgets/harmony_color_wheel.cpp"
    "${OBS_BGS_QTCOLORWIDGETS_ROOT}/src/QtColorWidgets/hue_slider.cpp"
    "${OBS_BGS_QTCOLORWIDGETS_ROOT}/src/QtColorWidgets/swatch.cpp"
    "${OBS_BGS_QTCOLORWIDGETS_ROOT}/resources/QtColorWidgets/color_widgets.qrc"
)
file(GLOB OBS_BGS_QTCOLORWIDGETS_HEADERS CONFIGURE_DEPENDS
     "${OBS_BGS_QTCOLORWIDGETS_ROOT}/include/QtColorWidgets/*.hpp")

add_library(QtColorWidgets STATIC
    ${OBS_BGS_QTCOLORWIDGETS_SOURCES}
    ${OBS_BGS_QTCOLORWIDGETS_HEADERS}
)
target_compile_definitions(QtColorWidgets PUBLIC QTCOLORWIDGETS_STATICALLY_LINKED)
target_include_directories(QtColorWidgets PUBLIC
    "${OBS_BGS_QTCOLORWIDGETS_ROOT}/include")
target_link_libraries(QtColorWidgets PRIVATE Qt${QT_VERSION_MAJOR}::Widgets)
set_target_properties(QtColorWidgets PROPERTIES
    CXX_STANDARD 11
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON)

# ── Sources ─────────────────────────────────────────────────────────
set(OBS_BGS_CORE_SOURCES
    src/core/title-data.cpp
    src/core/title-logger.cpp
    src/core/title-preferences.cpp
    src/core/system-memory.cpp
    src/core/ticker-runtime.cpp
)
set(OBS_BGS_CORE_HEADERS
    src/core/live-text-cue-utils.h
    src/core/title-data.h
    src/core/title-snapshot.h
    src/core/title-localization.h
    src/core/title-logger.h
    src/core/title-preferences.h
    src/core/system-memory.h
    src/core/ticker-runtime.h
)

set(OBS_BGS_LAYER_HEADERS
    src/layers/layer-model.h
    src/layers/image-layer-utils.h
    src/layers/path-geometry.h
    src/layers/layer-stack-widget.h
)

set(OBS_BGS_LAYER_SOURCES
    src/layers/image-layer-utils.cpp
    src/layers/path-geometry.cpp
    src/layers/layer-stack-widget.cpp
)

set(OBS_BGS_EFFECT_HEADERS
    src/effects/layer-effects.h
    src/effects/effect-preset-catalog.h
    src/effects/effects-panel.h
    src/effects/effects-presets-panel.h
)

set(OBS_BGS_EFFECT_SOURCES
    src/effects/effect-preset-catalog.cpp
    src/effects/effects-panel.cpp
    src/effects/effects-presets-panel.cpp
)

set(OBS_BGS_TIMELINE_SOURCES
    src/timeline/animation.cpp
    src/timeline/timeline-widget.cpp
)
set(OBS_BGS_TIMELINE_HEADERS
    src/timeline/animation.h
    src/timeline/timeline-widget.h
)

set(OBS_BGS_TRANSITION_SOURCES
    src/transitions/transition-preset-catalog.cpp
    src/transitions/transition-editor-dialog.cpp
)
set(OBS_BGS_TRANSITION_HEADERS
    src/transitions/layer-transition.h
    src/transitions/transition-preset-catalog.h
    src/transitions/transition-editor-dialog.h
)

set(OBS_BGS_TEXT_SOURCES
    src/text/title-rich-text.cpp
    src/text/title-text-layout.cpp
    src/text/title-text-layout-qt.cpp
)
set(OBS_BGS_TEXT_HEADERS
    src/text/title-rich-text.h
    src/text/title-text-layout.h
)

set(OBS_BGS_RENDERING_SOURCES
    src/rendering/title-effect-registry.cpp
    src/rendering/title-gpu-text-renderer.cpp
    src/rendering/title-gpu-text-sdf.cpp
)
set(OBS_BGS_RENDERING_HEADERS
    src/rendering/title-effect-registry.h
    src/rendering/title-gpu-text-renderer.h
    src/rendering/title-gpu-text-sdf.h
)

set(OBS_BGS_CACHE_SOURCES
    src/cache/cache-frame-key.cpp
    src/cache/cache-state-tracker.cpp
    src/cache/cache-manager.cpp
    src/cache/prerender-dock.cpp
    src/cache/ram-frame-cache.cpp
    src/cache/render-queue-manager.cpp
)
set(OBS_BGS_CACHE_HEADERS
    src/cache/cache-frame-payload.h
    src/cache/cache-tile-payload.h
    src/cache/cache-manager.h
    src/cache/cache-time.h
    src/cache/title-cache-policy.h
    src/cache/prerender-dock.h
)

set(OBS_BGS_CANVAS_SOURCES
    src/canvas/canvas-preview-inline-text.cpp
    src/canvas/canvas-preview.cpp
)
set(OBS_BGS_CANVAS_HEADERS
    src/canvas/canvas-preview.h
)

set(OBS_BGS_OBS_SOURCES
    src/obs/plugin-main.cpp
    src/obs/title-source.cpp
)
set(OBS_BGS_OBS_HEADERS
    src/obs/plugin-main.h
    src/obs/title-source.h
)

set(OBS_BGS_EDITOR_SOURCES
    src/editor/title-hotkeys.cpp
    src/editor/timecode-spinbox.cpp
    src/editor/title-dock.cpp
    src/editor/tools-sidebar.cpp
    src/editor/title-properties-panel.cpp
    src/editor/properties-panel.cpp
    src/editor/style-presets.cpp
    src/editor/title-editor.cpp
)
set(OBS_BGS_EDITOR_HEADERS
    src/editor/title-hotkeys.h
    src/editor/timecode-spinbox.h
    src/editor/title-dock.h
    src/editor/tools-sidebar.h
    src/editor/title-properties-panel.h
    src/editor/properties-panel.h
    src/editor/style-presets.h
    src/editor/title-editor-internal.h
    src/editor/title-editor.h
    src/editor/title-assets.h
)

set(PLUGIN_SOURCES
    ${OBS_BGS_CORE_SOURCES}
    ${OBS_BGS_LAYER_SOURCES}
    ${OBS_BGS_EFFECT_SOURCES}
    ${OBS_BGS_TIMELINE_SOURCES}
    ${OBS_BGS_TRANSITION_SOURCES}
    ${OBS_BGS_TEXT_SOURCES}
    ${OBS_BGS_RENDERING_SOURCES}
    ${OBS_BGS_CACHE_SOURCES}
    ${OBS_BGS_CANVAS_SOURCES}
    ${OBS_BGS_OBS_SOURCES}
    ${OBS_BGS_EDITOR_SOURCES}
)

set(PLUGIN_HEADERS
    ${OBS_BGS_CORE_HEADERS}
    ${OBS_BGS_LAYER_HEADERS}
    ${OBS_BGS_EFFECT_HEADERS}
    ${OBS_BGS_TIMELINE_HEADERS}
    ${OBS_BGS_TRANSITION_HEADERS}
    ${OBS_BGS_TEXT_HEADERS}
    ${OBS_BGS_RENDERING_HEADERS}
    ${OBS_BGS_CACHE_HEADERS}
    ${OBS_BGS_CANVAS_HEADERS}
    ${OBS_BGS_OBS_HEADERS}
    ${OBS_BGS_EDITOR_HEADERS}
)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" PREFIX "Source Files" FILES ${PLUGIN_SOURCES})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" PREFIX "Header Files" FILES ${PLUGIN_HEADERS})

# ── LZ4 frame compression ───────────────────────────────────────────
# Prefer a package supplied by OBS/vcpkg/the host. When none is installed,
# build the pinned official LZ4 1.10.0 source directly so compressed disk
# caching does not silently degrade to raw BGRA on a clean build machine.
find_package(lz4 CONFIG QUIET)
set(OBS_BGS_LZ4_TARGET "")
if(TARGET lz4::lz4)
  set(OBS_BGS_LZ4_TARGET lz4::lz4)
elseif(TARGET LZ4::lz4)
  set(OBS_BGS_LZ4_TARGET LZ4::lz4)
else()
  find_path(OBS_BGS_LZ4_INCLUDE_DIR NAMES lz4.h)
  find_library(OBS_BGS_LZ4_LIBRARY NAMES lz4 liblz4)
  if(OBS_BGS_LZ4_INCLUDE_DIR AND OBS_BGS_LZ4_LIBRARY)
    add_library(OBS_BGS_LZ4 UNKNOWN IMPORTED)
    set_target_properties(OBS_BGS_LZ4 PROPERTIES
      IMPORTED_LOCATION "${OBS_BGS_LZ4_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${OBS_BGS_LZ4_INCLUDE_DIR}")
    set(OBS_BGS_LZ4_TARGET OBS_BGS_LZ4)
  else()
    FetchContent_Declare(
      obs_bgs_lz4_source
      GIT_REPOSITORY https://github.com/lz4/lz4.git
      GIT_TAG ebb370ca83af193212df4dcbadcc5d87bc0de2f0
      GIT_PROGRESS FALSE
    )
    FetchContent_GetProperties(obs_bgs_lz4_source)
    if(NOT obs_bgs_lz4_source_POPULATED)
      FetchContent_Populate(obs_bgs_lz4_source)
    endif()

    add_library(obs_bgs_lz4 STATIC
      "${obs_bgs_lz4_source_SOURCE_DIR}/lib/lz4.c")
    target_include_directories(obs_bgs_lz4 PUBLIC
      "${obs_bgs_lz4_source_SOURCE_DIR}/lib")
    set_target_properties(obs_bgs_lz4 PROPERTIES
      POSITION_INDEPENDENT_CODE ON)
    set(OBS_BGS_LZ4_TARGET obs_bgs_lz4)
  endif()
endif()

# ── Target ──────────────────────────────────────────────────────────
add_library(broadcast-graphics-live MODULE
    ${PLUGIN_SOURCES}
    ${PLUGIN_HEADERS}
)

target_include_directories(broadcast-graphics-live PRIVATE
    src
    src/core
    src/layers
    src/effects
    src/timeline
    src/transitions
    src/text
    src/rendering
    src/cache
    src/canvas
    src/obs
    src/editor
    ${CAIRO_INCLUDE_DIRS}
    ${PANGO_INCLUDE_DIRS}
    ${PANGOCAIRO_INCLUDE_DIRS}
)

target_link_libraries(broadcast-graphics-live PRIVATE
    OBS::libobs
    OBS::obs-frontend-api
    ${QT_LIBS}
    QtColorWidgets
    ${OBS_BGS_TEXT_RENDER_TARGETS}
    nlohmann_json::nlohmann_json
    ${OBS_BGS_LZ4_TARGET}
)

if(CAIRO_CFLAGS_OTHER)
  target_compile_options(broadcast-graphics-live PRIVATE ${CAIRO_CFLAGS_OTHER})
endif()
if(PANGO_CFLAGS_OTHER)
  target_compile_options(broadcast-graphics-live PRIVATE ${PANGO_CFLAGS_OTHER})
endif()
if(PANGOCAIRO_CFLAGS_OTHER)
  target_compile_options(broadcast-graphics-live PRIVATE ${PANGOCAIRO_CFLAGS_OTHER})
endif()

# ── Locale / data files ──────────────────────────────────────────────
target_compile_definitions(broadcast-graphics-live PRIVATE
    PLUGIN_NAME="${OBS_TITLES_PLUGIN_NAME}"
    PLUGIN_VERSION="${OBS_BGS_VERSION}"
)
if(OBS_BGS_LZ4_TARGET)
    target_compile_definitions(broadcast-graphics-live PRIVATE OBS_BGS_HAVE_LZ4=1)
endif()

# ── OBS module helper (sets correct output name & install path) ──────
# This macro is provided by the OBS cmake helpers:
#   set_target_obs_bundle(broadcast-graphics-live)
# If building standalone, stage artifacts in OBS' user-plugin folder layout.
set_target_properties(broadcast-graphics-live PROPERTIES
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${OBS_TITLES_PACKAGE_BIN_DIR}"
    RUNTIME_OUTPUT_DIRECTORY "${OBS_TITLES_PACKAGE_BIN_DIR}"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)
foreach(config IN LISTS CMAKE_CONFIGURATION_TYPES)
  string(TOUPPER "${config}" config_upper)
  set_target_properties(broadcast-graphics-live PROPERTIES
      LIBRARY_OUTPUT_DIRECTORY_${config_upper} "${OBS_TITLES_PACKAGE_BIN_DIR}"
      RUNTIME_OUTPUT_DIRECTORY_${config_upper} "${OBS_TITLES_PACKAGE_BIN_DIR}"
      ARCHIVE_OUTPUT_DIRECTORY_${config_upper} "${CMAKE_BINARY_DIR}/lib/${config}")
endforeach()

add_custom_command(TARGET broadcast-graphics-live POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${OBS_TITLES_PACKAGE_DIR}/data"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_SOURCE_DIR}/data/locale"
            "${OBS_TITLES_PACKAGE_DIR}/data/locale"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_SOURCE_DIR}/data/icons"
            "${OBS_TITLES_PACKAGE_DIR}/data/icons"
    COMMAND "${CMAKE_COMMAND}" -E rm -rf
            "${OBS_TITLES_PACKAGE_DIR}/data/effects"
            "${OBS_TITLES_PACKAGE_DIR}/data/effects-and-presets"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_SOURCE_DIR}/data/effect-transitions"
            "${OBS_TITLES_PACKAGE_DIR}/data/effect-transitions"
    COMMENT "Staging OBS plugin data files")

if(WIN32)
  # OBS loads third-party DLL dependencies from the plugin binary directory.
  # Copy vcpkg-provided runtime DLLs into the staged folder so the plugin does
  # not compile successfully but fail LoadLibrary at OBS startup.
  set(OBS_TITLES_RUNTIME_DLL_DIRS)
  if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    list(APPEND OBS_TITLES_RUNTIME_DLL_DIRS
         "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
  endif()
  if(DEFINED _VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    list(APPEND OBS_TITLES_RUNTIME_DLL_DIRS
         "${_VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
  endif()
  if(DEFINED ENV{VCPKG_ROOT})
    if(DEFINED VCPKG_TARGET_TRIPLET)
      list(APPEND OBS_TITLES_RUNTIME_DLL_DIRS
           "$ENV{VCPKG_ROOT}/installed/${VCPKG_TARGET_TRIPLET}/bin")
    endif()
    list(APPEND OBS_TITLES_RUNTIME_DLL_DIRS
         "$ENV{VCPKG_ROOT}/installed/${OBS_TITLES_VCPKG_DEFAULT_TRIPLET}/bin")
  endif()

  set(OBS_TITLES_RUNTIME_DLLS)
  list(REMOVE_DUPLICATES OBS_TITLES_RUNTIME_DLL_DIRS)
  foreach(runtime_dir IN LISTS OBS_TITLES_RUNTIME_DLL_DIRS)
    if(IS_DIRECTORY "${runtime_dir}")
      file(GLOB runtime_dlls CONFIGURE_DEPENDS "${runtime_dir}/*.dll")
      list(APPEND OBS_TITLES_RUNTIME_DLLS ${runtime_dlls})
    endif()
  endforeach()

  if(OBS_TITLES_RUNTIME_DLLS)
    list(REMOVE_DUPLICATES OBS_TITLES_RUNTIME_DLLS)
    add_custom_command(TARGET broadcast-graphics-live POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                ${OBS_TITLES_RUNTIME_DLLS}
                "$<TARGET_FILE_DIR:broadcast-graphics-live>"
        COMMAND_EXPAND_LISTS
        COMMENT "Copying Windows runtime DLL dependencies")
    install(FILES ${OBS_TITLES_RUNTIME_DLLS}
            DESTINATION ${OBS_TITLES_PLUGIN_NAME}/bin/${OBS_TITLES_ARCH_DIR})
  else()
    message(WARNING
      "No vcpkg runtime DLLs were found to bundle with broadcast-graphics-live. "
      "If OBS reports that broadcast-graphics-live failed to load, ensure Qt/Cairo/Pango "
      "runtime DLLs are next to the plugin binary or use build-windows.ps1.")
  endif()
endif()

# ── Install ──────────────────────────────────────────────────────────
# Install into the same user-plugin layout expected below the chosen prefix.
# Example Windows prefix: C:/ProgramData/obs-studio/plugins
# Example Linux prefix:   ~/.config/obs-studio/plugins
install(TARGETS broadcast-graphics-live
        RUNTIME DESTINATION ${OBS_TITLES_PLUGIN_NAME}/bin/${OBS_TITLES_ARCH_DIR}
        LIBRARY DESTINATION ${OBS_TITLES_PLUGIN_NAME}/bin/${OBS_TITLES_ARCH_DIR})

# ── Data files ──────────────────────────────────────────────────────
install(DIRECTORY data/locale
        DESTINATION ${OBS_TITLES_PLUGIN_NAME}/data)
install(DIRECTORY data/icons
        DESTINATION ${OBS_TITLES_PLUGIN_NAME}/data)
install(DIRECTORY data/effect-transitions
        DESTINATION ${OBS_TITLES_PLUGIN_NAME}/data)

# ── Print summary ────────────────────────────────────────────────────
message(STATUS "Broadcast Graphics Live: Qt${QT_VERSION_MAJOR}, Cairo ${CAIRO_VERSION}")

option(OBS_BGS_BUILD_TESTS "Build lightweight Broadcast Graphics Live validation tests" OFF)
if(OBS_BGS_BUILD_TESTS)
  enable_testing()
  add_executable(rich_text_model_test
      tests/rich_text_model_test.cpp
      src/text/title-rich-text.cpp)
  target_include_directories(rich_text_model_test PRIVATE src src/core src/layers src/effects src/timeline src/text)
  target_compile_definitions(rich_text_model_test PRIVATE OBS_BGS_RICH_TEXT_STANDALONE_TEST)
  add_test(NAME rich_text_model_test COMMAND rich_text_model_test)

  add_executable(text_layout_contract_test
      tests/text_layout_contract_test.cpp
      src/text/title-rich-text.cpp
      src/text/title-text-layout.cpp)
  target_include_directories(text_layout_contract_test PRIVATE src src/core src/layers src/effects src/timeline src/text)
  target_compile_definitions(text_layout_contract_test PRIVATE OBS_BGS_RICH_TEXT_STANDALONE_TEST)
  add_test(NAME text_layout_contract_test COMMAND text_layout_contract_test)

  add_executable(animation_model_test
      tests/animation_model_test.cpp
      src/timeline/animation.cpp)
  target_include_directories(animation_model_test PRIVATE src/timeline)
  add_test(NAME animation_model_test COMMAND animation_model_test)

  add_executable(transition_model_test
      tests/transition_model_test.cpp)
  target_include_directories(transition_model_test PRIVATE src/transitions src/timeline)
  add_test(NAME transition_model_test COMMAND transition_model_test)

  add_executable(cache_time_contract_test
      tests/cache_time_contract_test.cpp)
  target_include_directories(cache_time_contract_test PRIVATE src/cache)
  add_test(NAME cache_time_contract_test COMMAND cache_time_contract_test)

  add_executable(ticker_runtime_contract_test
      tests/ticker_runtime_contract_test.cpp
      src/core/ticker-runtime.cpp)
  target_include_directories(ticker_runtime_contract_test PRIVATE
      src src/core src/layers src/effects src/timeline src/text src/transitions)
  add_test(NAME ticker_runtime_contract_test COMMAND ticker_runtime_contract_test)

  add_executable(ticker_controls_contract_test
      tests/ticker_controls_contract_test.cpp)
  add_test(NAME ticker_controls_contract_test
      COMMAND ticker_controls_contract_test
      ${CMAKE_CURRENT_SOURCE_DIR}/src/layers/layer-model.h
      ${CMAKE_CURRENT_SOURCE_DIR}/src/core/title-data.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/src/editor/properties-panel.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/src/editor/properties-panel.h
      ${CMAKE_CURRENT_SOURCE_DIR}/src/editor/title-hotkeys.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/src/canvas/canvas-preview.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/src/obs/title-source.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/src/editor/title-editor.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt
      ${CMAKE_CURRENT_SOURCE_DIR}/data/locale/en-US.ini)

  add_executable(adjustment_effects_transitions_contract_test
      tests/adjustment_effects_transitions_contract_test.cpp)
  add_test(NAME adjustment_effects_transitions_contract_test
      COMMAND adjustment_effects_transitions_contract_test
      ${CMAKE_CURRENT_SOURCE_DIR}/src/layers/layer-model.h
      ${CMAKE_CURRENT_SOURCE_DIR}/src/effects/layer-effects.h
      ${CMAKE_CURRENT_SOURCE_DIR}/src/transitions/layer-transition.h
      ${CMAKE_CURRENT_SOURCE_DIR}/src/core/title-data.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/src/obs/title-source.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/src/cache/cache-manager.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/src/effects/effects-panel.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/src/transitions/transition-editor-dialog.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/src/effects/effects-presets-panel.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/src/rendering/title-effect-registry.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/src/editor/properties-panel.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/src/canvas/canvas-preview.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/src/layers/layer-stack-widget.cpp)

  add_executable(effect_preview_shortcuts_contract_test
      tests/effect_preview_shortcuts_contract_test.cpp)
  add_test(NAME effect_preview_shortcuts_contract_test
      COMMAND effect_preview_shortcuts_contract_test
      ${CMAKE_CURRENT_SOURCE_DIR}/src/obs/title-source.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/src/transitions/transition-editor-dialog.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/src/editor/title-editor.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/data/effect-transitions/shaders/lens-flare/lens-flare.effect
      ${CMAKE_CURRENT_SOURCE_DIR}/data/effect-transitions/shaders/vignette/vignette.effect
      ${CMAKE_CURRENT_SOURCE_DIR}/data/effect-transitions/shaders/noise/noise.effect
      ${CMAKE_CURRENT_SOURCE_DIR}/data/effect-transitions/shaders/roughen-edges/roughen-edges.effect
      ${CMAKE_CURRENT_SOURCE_DIR}/src/cache/cache-manager.cpp)

  add_executable(procedural_effect_runtime_contract_test
      tests/procedural_effect_runtime_contract_test.cpp)
  add_test(NAME procedural_effect_runtime_contract_test
      COMMAND procedural_effect_runtime_contract_test
      ${CMAKE_CURRENT_SOURCE_DIR}/src/rendering/title-effect-registry.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/src/obs/title-source.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/src/cache/cache-manager.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/data/effect-transitions/shaders/lens-flare/lens-flare.effect
      ${CMAKE_CURRENT_SOURCE_DIR}/data/effect-transitions/shaders/vignette/vignette.effect
      ${CMAKE_CURRENT_SOURCE_DIR}/data/effect-transitions/shaders/noise/noise.effect
      ${CMAKE_CURRENT_SOURCE_DIR}/data/effect-transitions/shaders/roughen-edges/roughen-edges.effect)

  add_executable(lens_flare_runtime_contract_test
      tests/lens_flare_runtime_contract_test.cpp)
  add_test(NAME lens_flare_runtime_contract_test
      COMMAND lens_flare_runtime_contract_test
      ${CMAKE_CURRENT_SOURCE_DIR}/src/rendering/title-effect-registry.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/src/obs/title-source.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/src/cache/cache-manager.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/data/effect-transitions/shaders/lens-flare/lens-flare.effect)

  add_executable(system_memory_contract_test
      tests/system_memory_contract_test.cpp
      src/core/system-memory.cpp)
  target_include_directories(system_memory_contract_test PRIVATE src/core)
  add_test(NAME system_memory_contract_test COMMAND system_memory_contract_test)

  add_executable(cache_frame_payload_test
      tests/cache_frame_payload_test.cpp)
  target_include_directories(cache_frame_payload_test PRIVATE src/cache)
  target_link_libraries(cache_frame_payload_test PRIVATE
      Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Widgets)
  add_test(NAME cache_frame_payload_test COMMAND cache_frame_payload_test)

  add_executable(cache_tile_payload_test
      tests/cache_tile_payload_test.cpp)
  target_include_directories(cache_tile_payload_test PRIVATE src/cache)
  target_link_libraries(cache_tile_payload_test PRIVATE
      Qt${QT_VERSION_MAJOR}::Core Qt${QT_VERSION_MAJOR}::Widgets)
  add_test(NAME cache_tile_payload_test COMMAND cache_tile_payload_test)

  add_executable(disk_cache_tiling_contract_test
      tests/disk_cache_tiling_contract_test.cpp)
  add_test(NAME disk_cache_tiling_contract_test
      COMMAND disk_cache_tiling_contract_test
      ${CMAKE_CURRENT_SOURCE_DIR}/src/cache/cache-manager.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/src/cache/cache-manager.h
      ${CMAKE_CURRENT_SOURCE_DIR}/src/cache/cache-tile-payload.h)

  add_executable(live_text_cue_utils_test
      tests/live_text_cue_utils_test.cpp)
  target_include_directories(live_text_cue_utils_test PRIVATE
      src/core src/layers src/effects src/timeline src/text src/transitions)
  add_test(NAME live_text_cue_utils_test COMMAND live_text_cue_utils_test)

  add_executable(title_snapshot_test
      tests/title_snapshot_test.cpp)
  target_include_directories(title_snapshot_test PRIVATE
      src/core src/layers src/effects src/timeline src/text src/transitions)
  add_test(NAME title_snapshot_test COMMAND title_snapshot_test)

  add_executable(gpu_rounded_image_clip_test
      tests/gpu_rounded_image_clip_test.cpp)
  add_test(NAME gpu_rounded_image_clip_test COMMAND gpu_rounded_image_clip_test
      ${CMAKE_CURRENT_SOURCE_DIR}/src/obs/title-source.cpp)

  add_executable(gpu_mask_contract_test
      tests/gpu_mask_contract_test.cpp)
  add_test(NAME gpu_mask_contract_test COMMAND gpu_mask_contract_test
      ${CMAKE_CURRENT_SOURCE_DIR}/src/obs/title-source.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/src/canvas/canvas-preview.cpp)

  add_executable(gpu_ram_cache_tiling_contract_test
      tests/gpu_ram_cache_tiling_contract_test.cpp)
  add_test(NAME gpu_ram_cache_tiling_contract_test
      COMMAND gpu_ram_cache_tiling_contract_test
      ${CMAKE_CURRENT_SOURCE_DIR}/src/obs/title-source.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/src/obs/title-source.h
      ${CMAKE_CURRENT_SOURCE_DIR}/src/cache/cache-manager.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/src/cache/cache-manager.h)

  add_executable(phase15_visibility_recovery_contract_test
      tests/phase15_visibility_recovery_contract_test.cpp)
  add_test(NAME phase15_visibility_recovery_contract_test
      COMMAND phase15_visibility_recovery_contract_test
      ${CMAKE_CURRENT_SOURCE_DIR}/src/obs/title-source.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/src/rendering/title-gpu-text-renderer.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/src/cache/cache-manager.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/src/core/title-preferences.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt)

  add_executable(gpu_prerender_phase14_contract_test
      tests/gpu_prerender_phase14_contract_test.cpp)
  add_test(NAME gpu_prerender_phase14_contract_test COMMAND gpu_prerender_phase14_contract_test
      ${CMAKE_CURRENT_SOURCE_DIR}/src/obs/title-source.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/src/obs/title-source.h
      ${CMAKE_CURRENT_SOURCE_DIR}/src/cache/cache-manager.cpp
      ${CMAKE_CURRENT_SOURCE_DIR}/src/cache/cache-manager.h
      ${CMAKE_CURRENT_SOURCE_DIR}/src/canvas/canvas-preview.cpp)
endif()
