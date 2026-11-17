# libvideo: 장치 워커 안의 영상 송출(자식 프로세스: gst-launch, ffmpeg). 워크스페이스에 의존하지 않는다.
# 쓰는 워커마다 자기 폴더 lib/libvideo/에 사본을 둔다(원본 틀: workers/_template/lib/libvideo).
# libworker(worker)와 libframebus(framebus)가 먼저 add_subdirectory되어 있어야 한다. 워커 CMakeLists에서:
#     add_subdirectory(lib/libworker)
#     add_subdirectory(lib/libframebus)
#     add_subdirectory(lib/libvideo)
#     target_link_libraries(<워커> PRIVATE video)
cmake_minimum_required(VERSION 3.16)
project(libvideo VERSION 0.2.0 LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    # 이 폴더에서 직접 빌드(단위 시험): 형제 사본을 쓴다(원본 틀이면 _template/lib/*)
    add_subdirectory(../libworker ${CMAKE_BINARY_DIR}/libworker)
    add_subdirectory(../libframebus ${CMAKE_BINARY_DIR}/libframebus)
endif()

if(NOT TARGET video)
    add_library(video STATIC src/stream.cpp src/streams.cpp src/pipeline.cpp src/feeder.cpp src/module.cpp)
    target_compile_features(video PUBLIC cxx_std_17)
    target_include_directories(video PUBLIC include PRIVATE src)
    target_compile_options(video PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(video PUBLIC worker framebus)
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    enable_testing()
    add_executable(test_video test/test_video.cpp)
    target_link_libraries(test_video PRIVATE video)
    target_compile_options(test_video PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME video COMMAND test_video)
endif()
