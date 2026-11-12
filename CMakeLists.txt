cmake_minimum_required(VERSION 3.20)
project(VoiceFilter C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)   # miniaudio uses a few GNU extensions on Linux
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------- RNNoise
# Upstream has no CMake support; build the portable (non-RTCD) C path.
# Weights are compiled in (src/rnnoise_data.c), so rnnoise_create(NULL) works.
set(RNN ${CMAKE_SOURCE_DIR}/third_party/rnnoise)
add_library(rnnoise STATIC
  ${RNN}/src/denoise.c
  ${RNN}/src/rnn.c
  ${RNN}/src/pitch.c
  ${RNN}/src/kiss_fft.c
  ${RNN}/src/celt_lpc.c
  ${RNN}/src/nnet.c
  ${RNN}/src/nnet_default.c
  ${RNN}/src/parse_lpcnet_weights.c
  ${RNN}/src/rnnoise_data.c
  ${RNN}/src/rnnoise_tables.c)
target_include_directories(rnnoise PUBLIC ${RNN}/include PRIVATE ${RNN}/src)
target_compile_definitions(rnnoise PRIVATE RNNOISE_BUILD)
target_compile_options(rnnoise PRIVATE -O3 -w)
if(UNIX)
  target_link_libraries(rnnoise PUBLIC m)
endif()

# ---------------------------------------------------------------- miniaudio
add_library(miniaudio STATIC src/audio/miniaudio_impl.c)
target_include_directories(miniaudio PUBLIC ${CMAKE_SOURCE_DIR}/third_party/miniaudio)
target_compile_options(miniaudio PRIVATE -w)
if(UNIX)
  target_link_libraries(miniaudio PUBLIC pthread m ${CMAKE_DL_LIBS})
endif()

# ---------------------------------------------------------------- core lib
add_library(vfcore STATIC
  src/dsp/biquad.c
  src/dsp/fx_highpass.c
  src/dsp/fx_rnnoise.c
  src/dsp/fx_gate.c
  src/dsp/fx_compressor.c
  src/dsp/fx_eq3.c
  src/dsp/fx_limiter.c
  src/dsp/chain.c
  src/dsp/pipeline.c
  src/audio/engine.c
  src/audio/devices.c
  src/audio/sink.c
  src/audio/monitor.c
  src/util/cmdline.c
  src/util/console.c
  src/util/ringbuf.c
  src/util/wavio.c
  src/util/ini.c
  src/util/platform.c
  src/app/session.c
  src/app/settings.c)
target_include_directories(vfcore PUBLIC src)
target_link_libraries(vfcore PUBLIC rnnoise miniaudio)

# Virtual microphone: PipeWire on Linux, stub elsewhere (Windows uses VB-Cable).
option(VF_PIPEWIRE "Build the PipeWire virtual microphone (Linux)" ${UNIX})
if(VF_PIPEWIRE AND UNIX AND NOT APPLE)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
  target_sources(vfcore PRIVATE src/audio/vmic_pipewire.c)
  target_link_libraries(vfcore PUBLIC PkgConfig::PIPEWIRE)
  # SPA headers use GNU extensions that -Wpedantic flags.
  set_source_files_properties(src/audio/vmic_pipewire.c PROPERTIES COMPILE_OPTIONS "-Wno-pedantic")
  message(STATUS "VoiceFilter: PipeWire virtual microphone enabled")
else()
  target_sources(vfcore PRIVATE src/audio/vmic_stub.c)
endif()
if(MSVC)
  target_compile_options(vfcore PRIVATE /W4)
else()
  target_compile_options(vfcore PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------- executables
add_executable(voicefilter-cli src/main_cli.c)
target_link_libraries(voicefilter-cli PRIVATE vfcore)
if(NOT MSVC)
  target_compile_options(voicefilter-cli PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------- GUI (raylib + raygui)
option(VF_GUI "Build the raylib GUI (voicefilter)" ON)
if(VF_GUI)
  # raylib without its audio module: raudio embeds its own miniaudio, which would
  # clash with ours, and VoiceFilter does all audio itself.
  set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
  set(CUSTOMIZE_BUILD ON CACHE BOOL "" FORCE)
  set(SUPPORT_MODULE_RAUDIO OFF CACHE BOOL "" FORCE)
  set(USE_AUDIO OFF CACHE BOOL "" FORCE)
  add_subdirectory(third_party/raylib EXCLUDE_FROM_ALL)
  if(TARGET raylib)
    target_compile_options(raylib PRIVATE -w)
  endif()
  add_executable(voicefilter src/main_gui.c src/app/tray.c)
  target_include_directories(voicefilter PRIVATE third_party/raygui)
  target_link_libraries(voicefilter PRIVATE vfcore raylib)
  if(WIN32)
    target_link_libraries(voicefilter PRIVATE shell32 user32 gdi32)
  endif()
  if(WIN32)
    set_target_properties(voicefilter PROPERTIES WIN32_EXECUTABLE ON)   # no console window
  endif()
  if(NOT MSVC)
    # raygui itself triggers a few warnings; keep our own code strict elsewhere.
    target_compile_options(voicefilter PRIVATE -Wall -Wno-unused-function -Wno-unused-parameter
                                               -Wno-unused-but-set-variable)
  endif()
endif()

# ---------------------------------------------------------------- tests
enable_testing()
add_executable(test_dsp tests/test_dsp.c)
target_link_libraries(test_dsp PRIVATE vfcore)
if(NOT MSVC)
  target_compile_options(test_dsp PRIVATE -Wall -Wextra -Wpedantic)
endif()
add_test(NAME dsp COMMAND test_dsp)

# ---------------------------------------------------------------- offline tools
add_executable(voicefilter-file tools/vf_file.c)
target_link_libraries(voicefilter-file PRIVATE vfcore)
add_executable(vf-eval tools/vf_eval.c)
target_link_libraries(vf-eval PRIVATE vfcore)
add_executable(vf-latency tools/vf_latency.c)
target_link_libraries(vf-latency PRIVATE vfcore)
add_executable(vf-record tools/vf_record.c)
target_link_libraries(vf-record PRIVATE vfcore)
foreach(t voicefilter-file vf-eval vf-latency vf-record)
  if(NOT MSVC)
    target_compile_options(${t} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endforeach()
