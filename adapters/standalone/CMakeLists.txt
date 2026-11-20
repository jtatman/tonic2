# tonic2_play: standalone player (miniaudio audio output + RtMidi input). Enabled with -DTONIC2_BUILD_STANDALONE=ON.
# Needs the miniaudio submodule and RtMidi from the DPF submodule:
#   git submodule update --init third_party/miniaudio third_party/DPF
set(RTMIDI_DIR ${PROJECT_SOURCE_DIR}/third_party/DPF/distrho/src/jackbridge/rtmidi)
add_executable(tonic2_play play.cpp ${RTMIDI_DIR}/RtMidi.cpp)
target_include_directories(tonic2_play PRIVATE ${PROJECT_SOURCE_DIR}/third_party/miniaudio ${RTMIDI_DIR}
                           ${PROJECT_SOURCE_DIR}/examples)
target_link_libraries(tonic2_play PRIVATE tonic2)
find_package(Threads REQUIRED)
target_link_libraries(tonic2_play PRIVATE Threads::Threads)
if(WIN32)
  target_compile_definitions(tonic2_play PRIVATE __WINDOWS_MM__)
  target_link_libraries(tonic2_play PRIVATE winmm ole32)
elseif(APPLE)
  target_compile_definitions(tonic2_play PRIVATE __MACOSX_CORE__)
  target_link_libraries(tonic2_play PRIVATE "-framework CoreMIDI" "-framework CoreAudio" "-framework CoreFoundation"
                        "-framework AudioToolbox" "-framework AudioUnit")
else()
  find_package(PkgConfig QUIET)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(ALSA alsa)
  endif()
  if(ALSA_FOUND)
    target_compile_definitions(tonic2_play PRIVATE __LINUX_ALSA__)
    target_include_directories(tonic2_play PRIVATE ${ALSA_INCLUDE_DIRS})
    target_link_libraries(tonic2_play PRIVATE ${ALSA_LIBRARIES})
  else()
    target_compile_definitions(tonic2_play PRIVATE __RTMIDI_DUMMY__)  # no MIDI input without ALSA headers
  endif()
  target_link_libraries(tonic2_play PRIVATE ${CMAKE_DL_LIBS} m)
endif()

enable_testing()
add_test(NAME standalone_selftest COMMAND tonic2_play --null --notes --seconds 1.5)
