cmake_minimum_required(VERSION 3.20)
project(tonic2 VERSION 0.1.0 LANGUAGES CXX)

option(TONIC2_BUILD_TESTS "Build tests and the golden-file tool" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Static MSVC runtime: plugins and tools load without the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Header-only for now (M1 = DSP kernels).
add_library(tonic2 INTERFACE)
add_library(tonic2::tonic2 ALIAS tonic2)
target_include_directories(tonic2 INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(tonic2 INTERFACE cxx_std_20)
if(MSVC)
  target_compile_definitions(tonic2 INTERFACE _USE_MATH_DEFINES NOMINMAX _CRT_SECURE_NO_WARNINGS)
  target_compile_options(tonic2 INTERFACE /wd4324)  # padding from alignas(64) in SpscQueue is intended
endif()

if(TONIC2_BUILD_TESTS)
  enable_testing()
  set(TONIC2_WARN $<IF:$<CXX_COMPILER_ID:MSVC>,/W4,-Wall -Wextra -Wpedantic>)

  add_executable(tonic2_dsp_tests tests/dsp_tests.cpp)
  target_link_libraries(tonic2_dsp_tests PRIVATE tonic2)
  target_compile_options(tonic2_dsp_tests PRIVATE ${TONIC2_WARN})
  target_compile_definitions(tonic2_dsp_tests PRIVATE TONIC2_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/golden")
  add_test(NAME dsp_tests COMMAND tonic2_dsp_tests)

  find_package(Threads REQUIRED)
  add_executable(tonic2_graph_tests tests/graph_tests.cpp)
  target_link_libraries(tonic2_graph_tests PRIVATE tonic2 Threads::Threads)
  target_compile_options(tonic2_graph_tests PRIVATE ${TONIC2_WARN})
  add_test(NAME graph_tests COMMAND tonic2_graph_tests)

  add_executable(tonic2_engine_tests tests/engine_tests.cpp)
  target_link_libraries(tonic2_engine_tests PRIVATE tonic2)
  target_compile_options(tonic2_engine_tests PRIVATE ${TONIC2_WARN})
  add_test(NAME engine_tests COMMAND tonic2_engine_tests)

  add_executable(tonic2_dsl_tests tests/dsl_tests.cpp)
  target_link_libraries(tonic2_dsl_tests PRIVATE tonic2)
  target_include_directories(tonic2_dsl_tests PRIVATE examples)
  target_compile_options(tonic2_dsl_tests PRIVATE ${TONIC2_WARN})
  add_test(NAME dsl_tests COMMAND tonic2_dsl_tests)

  add_executable(tonic2_voice_tests tests/voice_tests.cpp)
  target_link_libraries(tonic2_voice_tests PRIVATE tonic2)
  target_compile_options(tonic2_voice_tests PRIVATE ${TONIC2_WARN})
  add_test(NAME voice_tests COMMAND tonic2_voice_tests)

  add_executable(tonic2_mixer_tests tests/mixer_tests.cpp)
  target_link_libraries(tonic2_mixer_tests PRIVATE tonic2 Threads::Threads)
  target_compile_options(tonic2_mixer_tests PRIVATE ${TONIC2_WARN})
  add_test(NAME mixer_tests COMMAND tonic2_mixer_tests)

  add_executable(tonic2_ui_tests tests/ui_tests.cpp)
  target_link_libraries(tonic2_ui_tests PRIVATE tonic2)
  target_include_directories(tonic2_ui_tests PRIVATE examples)
  target_compile_options(tonic2_ui_tests PRIVATE ${TONIC2_WARN})
  add_test(NAME ui_tests COMMAND tonic2_ui_tests)

  foreach(t units fx)
    add_executable(tonic2_${t}_tests tests/${t}_tests.cpp)
    target_link_libraries(tonic2_${t}_tests PRIVATE tonic2)
    target_compile_options(tonic2_${t}_tests PRIVATE ${TONIC2_WARN})
    add_test(NAME ${t}_tests COMMAND tonic2_${t}_tests)
  endforeach()

  add_executable(tonic2_match_tests tests/match_tests.cpp)
  target_link_libraries(tonic2_match_tests PRIVATE tonic2 Threads::Threads)
  target_include_directories(tonic2_match_tests PRIVATE examples)
  target_compile_options(tonic2_match_tests PRIVATE ${TONIC2_WARN})
  add_test(NAME match_tests COMMAND tonic2_match_tests)

  add_executable(tonic2_render tools/render.cpp)
  target_link_libraries(tonic2_render PRIVATE tonic2)
  target_include_directories(tonic2_render PRIVATE examples)
  target_compile_options(tonic2_render PRIVATE ${TONIC2_WARN})

  add_executable(tonic2_match tools/match.cpp)
  target_link_libraries(tonic2_match PRIVATE tonic2 Threads::Threads)
  target_include_directories(tonic2_match PRIVATE examples)
  target_compile_options(tonic2_match PRIVATE ${TONIC2_WARN})
  add_test(NAME match_cli COMMAND tonic2_match --demo mono --truth cutoff=3000,resonance=0.5
           --params cutoff,resonance --evals 60 --seconds 0.5 --threads 2)

  add_executable(tonic2_golden tools/golden.cpp)
  target_link_libraries(tonic2_golden PRIVATE tonic2)
endif()

option(TONIC2_BUILD_DPF "Build the DPF plugin adapter (VST3/CLAP/LV2); needs third_party/DPF" OFF)
if(TONIC2_BUILD_DPF)
  enable_language(C)
  enable_testing()
  add_subdirectory(adapters/dpf)
endif()

option(TONIC2_BUILD_STANDALONE "Build tonic2_play (audio device + MIDI input); needs third_party/miniaudio and third_party/DPF" OFF)
if(TONIC2_BUILD_STANDALONE)
  enable_testing()
  add_subdirectory(adapters/standalone)
endif()
