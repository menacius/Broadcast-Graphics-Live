# Install script for directory: C:/Users/menac/Desktop/obs-titles-plugin/OBS-Graphics-Studio-Pro

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "C:/Program Files/obs-graphics-studio-pro")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "Release")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/Users/menac/Desktop/obs-titles-plugin/OBS-Graphics-Studio-Pro/build/_deps/nlohmann_json-build/cmake_install.cmake")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/obs-graphics-studio-pro/bin/64bit" TYPE FILE FILES
    "C:/vcpkg/installed/x64-windows/bin/brotlicommon.dll"
    "C:/vcpkg/installed/x64-windows/bin/brotlidec.dll"
    "C:/vcpkg/installed/x64-windows/bin/brotlienc.dll"
    "C:/vcpkg/installed/x64-windows/bin/bz2.dll"
    "C:/vcpkg/installed/x64-windows/bin/cairo-2.dll"
    "C:/vcpkg/installed/x64-windows/bin/cairo-gobject-2.dll"
    "C:/vcpkg/installed/x64-windows/bin/cairo-script-interpreter-2.dll"
    "C:/vcpkg/installed/x64-windows/bin/charset-1.dll"
    "C:/vcpkg/installed/x64-windows/bin/ffi-8.dll"
    "C:/vcpkg/installed/x64-windows/bin/fontconfig-1.dll"
    "C:/vcpkg/installed/x64-windows/bin/freetype.dll"
    "C:/vcpkg/installed/x64-windows/bin/fribidi-0.dll"
    "C:/vcpkg/installed/x64-windows/bin/gio-2.0-0.dll"
    "C:/vcpkg/installed/x64-windows/bin/girepository-2.0-0.dll"
    "C:/vcpkg/installed/x64-windows/bin/glib-2.0-0.dll"
    "C:/vcpkg/installed/x64-windows/bin/gmodule-2.0-0.dll"
    "C:/vcpkg/installed/x64-windows/bin/gobject-2.0-0.dll"
    "C:/vcpkg/installed/x64-windows/bin/gthread-2.0-0.dll"
    "C:/vcpkg/installed/x64-windows/bin/harfbuzz-gpu.dll"
    "C:/vcpkg/installed/x64-windows/bin/harfbuzz-raster.dll"
    "C:/vcpkg/installed/x64-windows/bin/harfbuzz-subset.dll"
    "C:/vcpkg/installed/x64-windows/bin/harfbuzz-vector.dll"
    "C:/vcpkg/installed/x64-windows/bin/harfbuzz.dll"
    "C:/vcpkg/installed/x64-windows/bin/iconv-2.dll"
    "C:/vcpkg/installed/x64-windows/bin/intl-8.dll"
    "C:/vcpkg/installed/x64-windows/bin/libexpat.dll"
    "C:/vcpkg/installed/x64-windows/bin/libpng16.dll"
    "C:/vcpkg/installed/x64-windows/bin/pango-1.0-0.dll"
    "C:/vcpkg/installed/x64-windows/bin/pangocairo-1.0-0.dll"
    "C:/vcpkg/installed/x64-windows/bin/pangoft2-1.0-0.dll"
    "C:/vcpkg/installed/x64-windows/bin/pangowin32-1.0-0.dll"
    "C:/vcpkg/installed/x64-windows/bin/pcre2-16.dll"
    "C:/vcpkg/installed/x64-windows/bin/pcre2-32.dll"
    "C:/vcpkg/installed/x64-windows/bin/pcre2-8.dll"
    "C:/vcpkg/installed/x64-windows/bin/pcre2-posix.dll"
    "C:/vcpkg/installed/x64-windows/bin/pixman-1-0.dll"
    "C:/vcpkg/installed/x64-windows/bin/pthreadVC3.dll"
    "C:/vcpkg/installed/x64-windows/bin/pthreadVCE3.dll"
    "C:/vcpkg/installed/x64-windows/bin/pthreadVSE3.dll"
    "C:/vcpkg/installed/x64-windows/bin/z.dll"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Dd][Ee][Bb][Uu][Gg])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/obs-graphics-studio-pro/bin/64bit" TYPE MODULE FILES "C:/Users/menac/Desktop/obs-titles-plugin/OBS-Graphics-Studio-Pro/build/obs-graphics-studio-pro/bin/64bit/obs-graphics-studio-pro.dll")
  elseif(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/obs-graphics-studio-pro/bin/64bit" TYPE MODULE FILES "C:/Users/menac/Desktop/obs-titles-plugin/OBS-Graphics-Studio-Pro/build/obs-graphics-studio-pro/bin/64bit/obs-graphics-studio-pro.dll")
  elseif(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Mm][Ii][Nn][Ss][Ii][Zz][Ee][Rr][Ee][Ll])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/obs-graphics-studio-pro/bin/64bit" TYPE MODULE FILES "C:/Users/menac/Desktop/obs-titles-plugin/OBS-Graphics-Studio-Pro/build/obs-graphics-studio-pro/bin/64bit/obs-graphics-studio-pro.dll")
  elseif(CMAKE_INSTALL_CONFIG_NAME MATCHES "^([Rr][Ee][Ll][Ww][Ii][Tt][Hh][Dd][Ee][Bb][Ii][Nn][Ff][Oo])$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/obs-graphics-studio-pro/bin/64bit" TYPE MODULE FILES "C:/Users/menac/Desktop/obs-titles-plugin/OBS-Graphics-Studio-Pro/build/obs-graphics-studio-pro/bin/64bit/obs-graphics-studio-pro.dll")
  endif()
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/obs-graphics-studio-pro/data" TYPE DIRECTORY FILES "C:/Users/menac/Desktop/obs-titles-plugin/OBS-Graphics-Studio-Pro/data/locale")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/obs-graphics-studio-pro/data" TYPE DIRECTORY FILES "C:/Users/menac/Desktop/obs-titles-plugin/OBS-Graphics-Studio-Pro/data/icons")
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "C:/Users/menac/Desktop/obs-titles-plugin/OBS-Graphics-Studio-Pro/build/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
if(CMAKE_INSTALL_COMPONENT)
  if(CMAKE_INSTALL_COMPONENT MATCHES "^[a-zA-Z0-9_.+-]+$")
    set(CMAKE_INSTALL_MANIFEST "install_manifest_${CMAKE_INSTALL_COMPONENT}.txt")
  else()
    string(MD5 CMAKE_INST_COMP_HASH "${CMAKE_INSTALL_COMPONENT}")
    set(CMAKE_INSTALL_MANIFEST "install_manifest_${CMAKE_INST_COMP_HASH}.txt")
    unset(CMAKE_INST_COMP_HASH)
  endif()
else()
  set(CMAKE_INSTALL_MANIFEST "install_manifest.txt")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "C:/Users/menac/Desktop/obs-titles-plugin/OBS-Graphics-Studio-Pro/build/${CMAKE_INSTALL_MANIFEST}"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
