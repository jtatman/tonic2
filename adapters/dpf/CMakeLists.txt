# DPF plugin adapter (VST3, CLAP, LV2). Enabled with -DTONIC2_BUILD_DPF=ON; needs the DPF submodule:
#   git submodule update --init third_party/DPF
add_subdirectory(${PROJECT_SOURCE_DIR}/third_party/DPF ${CMAKE_BINARY_DIR}/dpf)

dpf_add_plugin(tonic2_poly
  TARGETS vst3 clap lv2
  FILES_DSP poly/PolyPlugin.cpp)
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
