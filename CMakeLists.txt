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
  src/util/cmdline.c
  src/util/console.c
  src/util/wavio.c)
target_include_directories(vfcore PUBLIC src)
target_link_libraries(vfcore PUBLIC rnnoise miniaudio)
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
foreach(t voicefilter-file vf-eval vf-latency)
  if(NOT MSVC)
    target_compile_options(${t} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endforeach()
