# libframebus: 같은 보드의 워커 사이 영상 프레임 전달 (design.md 6.7). 워크스페이스에 의존하지 않는다.
# 쓰는 워커마다 자기 폴더 lib/libframebus/에 사본을 둔다(원본 틀: workers/_template/lib/libframebus). 워커 CMakeLists에서:
#     add_subdirectory(lib/libframebus)
#     target_link_libraries(<워커> PRIVATE framebus)
cmake_minimum_required(VERSION 3.16)
project(libframebus VERSION 0.1.0 LANGUAGES CXX)

if(NOT TARGET framebus)
    find_package(Threads REQUIRED)
    add_library(framebus STATIC src/framebus.cpp)
    target_compile_features(framebus PUBLIC cxx_std_17)
    target_include_directories(framebus PUBLIC include)
    target_compile_options(framebus PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(framebus PUBLIC Threads::Threads)
endif()

# 이 폴더에서 직접 빌드할 때만 단위 시험(jetson-build --test)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    enable_testing()
    add_executable(test_framebus test/test_framebus.cpp)
    target_link_libraries(test_framebus PRIVATE framebus)
    target_compile_options(test_framebus PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME framebus COMMAND test_framebus)
    # 동시성 스트레스(생산자 하나 + 소비자 넷, 내용 검사·끊김 회수). ThreadSanitizer 빌드는 dev/check.sh가 한다
    add_executable(test_framebus_stress test/test_framebus_stress.cpp)
    target_link_libraries(test_framebus_stress PRIVATE framebus)
    target_compile_options(test_framebus_stress PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME framebus_stress COMMAND test_framebus_stress 2)
endif()
