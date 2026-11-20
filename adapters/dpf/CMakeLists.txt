# DPF plugin adapter (VST3, CLAP, LV2). Enabled with -DTONIC2_BUILD_DPF=ON; needs the DPF submodule:
#   git submodule update --init third_party/DPF
# DPF downloads the Khronos GL headers at configure time; seed them from the copy in this repo instead.
foreach(h GL/glext.h KHR/khrplatform.h)
  if(NOT EXISTS "${PROJECT_SOURCE_DIR}/third_party/DPF/khronos/${h}")
    get_filename_component(dir "${PROJECT_SOURCE_DIR}/third_party/DPF/khronos/${h}" DIRECTORY)
    file(COPY "${PROJECT_SOURCE_DIR}/third_party/khronos/${h}" DESTINATION "${dir}")
  endif()
endforeach()
# DPF relies on GNU extensions (e.g. typeof in its Linux web-view code); tonic2 itself builds without them.
set(CMAKE_CXX_EXTENSIONS ON)
add_subdirectory(${PROJECT_SOURCE_DIR}/third_party/DPF ${CMAKE_BINARY_DIR}/dpf)

option(TONIC2_DPF_UI "Build the plugin UI (needs OpenGL; on Linux also X11 dev headers)" ON)
if(TONIC2_DPF_UI)
  # jack = standalone app: JACK when a server runs, otherwise native audio + MIDI (WASAPI/WinMM,
  # CoreAudio/CoreMIDI, ALSA/Pulse) through DPF's bundled RtAudio/RtMidi.
  set(TONIC2_POLY_TARGETS vst3 clap lv2)
  if(NOT MSVC)  # DPF's native-audio fallback is MinGW-only on Windows; there use tonic2_play instead
    list(APPEND TONIC2_POLY_TARGETS jack)
  endif()
  if(WIN32)
    list(APPEND TONIC2_POLY_TARGETS vst2)  # testing only: tools/uishot screenshots the VST2 editor
  endif()
  dpf_add_plugin(tonic2_poly
    TARGETS ${TONIC2_POLY_TARGETS}
    UI_TYPE opengl
    FILES_DSP poly/PolyPlugin.cpp
    FILES_UI poly/PolyUI.cpp)
  target_compile_definitions(tonic2_poly PUBLIC TONIC2_HAS_UI=1)
else()
  dpf_add_plugin(tonic2_poly
    TARGETS vst3 clap lv2
    FILES_DSP poly/PolyPlugin.cpp)
  target_compile_definitions(tonic2_poly PUBLIC TONIC2_HAS_UI=0)
endif()
target_include_directories(tonic2_poly PUBLIC poly ${PROJECT_SOURCE_DIR}/examples)
target_link_libraries(tonic2_poly PUBLIC tonic2)
target_compile_features(tonic2_poly PUBLIC cxx_std_20)

# Test plugin: output equals its "level" parameter (used to check automation and CC timing).
dpf_add_plugin(tonic2_probe
  TARGETS clap vst3
  FILES_DSP probe/ProbePlugin.cpp)
target_include_directories(tonic2_probe PUBLIC probe)
target_link_libraries(tonic2_probe PUBLIC tonic2)
target_compile_features(tonic2_probe PUBLIC cxx_std_20)

# A minimal CLAP host that loads the built plugin and renders a chord (Linux/macOS/Windows).
add_executable(tonic2_clap_host_test clap_host_test.cpp)
target_include_directories(tonic2_clap_host_test PRIVATE ${PROJECT_SOURCE_DIR}/third_party/DPF/distrho/src/clap)
target_link_libraries(tonic2_clap_host_test PRIVATE tonic2 ${CMAKE_DL_LIBS})
add_dependencies(tonic2_clap_host_test tonic2_poly-clap tonic2_probe-clap)
foreach(name poly probe)
  if(APPLE)
    set(bin ${CMAKE_BINARY_DIR}/bin/tonic2_${name}.clap/Contents/MacOS/tonic2_${name})
  else()
    set(bin ${CMAKE_BINARY_DIR}/bin/tonic2_${name}.clap)
  endif()
  add_test(NAME clap_host_${name} COMMAND tonic2_clap_host_test ${bin})
endforeach()

# Windows: screenshot a plugin editor without a GUI host (opens the VST2 editor in a window).
if(WIN32 AND TONIC2_DPF_UI)
  add_executable(tonic2_uishot ${PROJECT_SOURCE_DIR}/tools/uishot.cpp)
  target_link_libraries(tonic2_uishot PRIVATE user32 gdi32)
  target_compile_definitions(tonic2_uishot PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
