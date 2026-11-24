# status-manager: 시스템 상태 매니저(기본 탑재, managers/status/design.md). 이 폴더만으로 빌드한다.
#
#   cmake -S managers/status -B ~/status-build && cmake --build ~/status-build -j && ctest --test-dir ~/status-build --output-on-failure
#   확정(Jetson aarch64): dev/release-worker.sh status   → managers/status/bin/status-manager + BUILD
#
# 구성: lib/libworker(사본, 원본 workers/_template/lib/libworker) · lib/status(① core/ 건강 판정 ② 모듈) · src/main.cpp
#       · test/test_worker.cpp(실행 파일 통합 시험: 가짜 process-manager 소켓)
cmake_minimum_required(VERSION 3.16)
project(status_manager LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(BUILD_TESTING "Build tests" ON)
enable_testing()

add_subdirectory(lib/libworker)
add_subdirectory(lib/status)

add_executable(status-manager src/main.cpp)
target_link_libraries(status-manager PRIVATE status_module)
target_compile_options(status-manager PRIVATE -Wall -Wextra -Wpedantic)
install(TARGETS status-manager RUNTIME DESTINATION bin)

if(BUILD_TESTING)
    # 통합 시험: 가짜 process-manager 소켓에 붙어 주제 system(건강 바뀜)·get_system·끊김을 확인
    add_executable(test_worker test/test_worker.cpp)
    target_link_libraries(test_worker PRIVATE worker)
    add_test(NAME worker COMMAND test_worker ${CMAKE_CROSSCOMPILING_EMULATOR} $<TARGET_FILE:status-manager>)
endif()
