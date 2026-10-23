cmake_minimum_required(VERSION 3.24)
if(POLICY CMP0184)
  cmake_policy(SET CMP0184 NEW) # MSVC_RUNTIME_CHECKS property
endif()
project(black_loader VERSION 1.4.2 LANGUAGES C CXX) # the release number (About box, first log line)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "The loader must be 32-bit: configure with -A Win32 (or use the 'win32' preset).")
endif()
if(NOT MSVC)
  message(FATAL_ERROR "MSVC (cl or clang-cl) is required: the loader uses MSVC x86 inline asm and linker options.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# blackrt.dll: XBE loader, HLE kernel, hooks. Everything that is real code lives here.
add_library(blackrt SHARED
  res/black.rc
  src/main.cpp
  src/log.cpp
  src/xbe.cpp
  src/image.cpp
  src/xthread.cpp
  src/hooks.cpp
  src/timing.cpp
  src/detour.cpp
  src/framerate.cpp
  src/autotune.cpp
  src/fov.cpp
  src/shake.cpp
  src/effects.cpp
  src/drawdist.cpp
  src/cpu_guard.cpp
  src/cheats.cpp
  src/saveimport.cpp
  src/reticle.cpp
  src/audio_defaults.cpp
  src/mods.cpp
  src/lang.cpp
  src/d3d/subtitles.cpp
  src/d3d/occlusion.cpp
  src/d3d/fps.cpp
  src/d3d/cursor.cpp
  src/fe/restart.cpp
  src/fe/calls.cpp
  src/fe/vars.cpp
  src/fe/pc_settings.cpp
  src/fe/apt.cpp
  src/fe/pages.cpp
  src/fe/bind_list.cpp
  src/fe/pause_ui.cpp
  src/fe/prompts.cpp
  src/input/mouselook.cpp
  src/input/autoaim.cpp
  src/input/bindings.cpp
  src/input/movies.cpp
  src/audio/reverb.cpp
  src/settings.cpp
  src/perf.cpp
  src/sha1.cpp
  src/kernel/ktable.cpp
  src/kernel/k_memory.cpp
  src/kernel/k_thread.cpp
  src/kernel/k_sync.cpp
  src/kernel/k_dpc.cpp
  src/kernel/k_file.cpp
  src/kernel/k_rtl.cpp
  src/kernel/k_misc.cpp
  src/d3d/dispatch.cpp
  src/d3d/device.cpp
  src/d3d/host.cpp
  src/d3d/pushbuf.cpp
  src/d3d/draw.cpp
  src/d3d/formats.cpp
  src/d3d/resources.cpp
  src/d3d/texture.cpp
  src/d3d/texpack.cpp
  src/d3d/vshader.cpp
  src/d3d/nv2a_vsh.cpp
  src/d3d/nv2a_psh.cpp
  src/d3d/pshader.cpp
  src/audio/dsound.cpp
  src/audio/output.cpp
  src/input/xinput.cpp
)
target_include_directories(blackrt PRIVATE src)
target_compile_definitions(blackrt PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE
  BLACK_VERSION="${PROJECT_VERSION}")
target_compile_options(blackrt PRIVATE /W3 /utf-8 /EHsc)
target_link_libraries(blackrt PRIVATE ntdll bcrypt d3d9 d3dcompiler windowscodecs ole32 user32 gdi32 winmm dbghelp advapi32 shell32)
# Xbox SEH handlers run through the same fs:[0] chain; no SafeSEH tables anywhere in the process.
target_link_options(blackrt PRIVATE /SAFESEH:NO /MAP) # blackrt.map: port addresses in crash logs -> functions

# black.exe: tiny stub with no CRT. Linked at 0x10000 with a large BSS so its own image owns the
# XBE's address range (0x10000-0x587a40) before Windows can place heaps or stacks there.
# blackrt.dll then maps the XBE over this image (headers included) and never returns.
# A Windows-subsystem program: no console window; started from a command prompt it attaches to that one (main.cpp).
add_executable(black stub/stub.c res/black.rc)
target_link_libraries(black PRIVATE blackrt kernel32)
set_property(TARGET black PROPERTY MSVC_RUNTIME_CHECKS "")
target_compile_options(black PRIVATE /GS- /Gs999999)
target_link_options(black PRIVATE
  /BASE:0x10000 /FIXED /DYNAMICBASE:NO /NXCOMPAT:NO /SAFESEH:NO /LARGEADDRESSAWARE
  /NODEFAULTLIB /ENTRY:StubEntry /SUBSYSTEM:WINDOWS /MANIFEST:NO /INCREMENTAL:NO)
