# record-manager: 기록 매니저(기본 탑재, managers/record/design.md). 이 폴더만으로 빌드한다.
#
#   cmake -S managers/record -B ~/rec-build && cmake --build ~/rec-build -j && ctest --test-dir ~/rec-build --output-on-failure
#   확정(Jetson aarch64): dev/release-worker.sh record   → managers/record/bin/record-manager + BUILD
#
# 구성: lib/libworker(사본, 원본 workers/_template/lib/libworker) · lib/record(① core/ 코어 ② 모듈) · src/main.cpp
#       · test/test_worker.cpp(실행 파일 통합 시험: 가짜 process-manager 소켓)
cmake_minimum_required(VERSION 3.16)
project(record_manager LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(BUILD_TESTING "Build tests" ON)
enable_testing()

add_subdirectory(lib/libworker)
add_subdirectory(lib/record)

add_executable(record-manager src/main.cpp)
target_link_libraries(record-manager PRIVATE record_module)
target_compile_options(record-manager PRIVATE -Wall -Wextra -Wpedantic)
install(TARGETS record-manager RUNTIME DESTINATION bin)

if(BUILD_TESTING)
    # 통합 시험: 가짜 process-manager 소켓에 붙어 기록 줄(예전 logger 모양)·find·끊김·SIGTERM 정리를 확인
    add_executable(test_worker test/test_worker.cpp)
    target_link_libraries(test_worker PRIVATE worker)
    add_test(NAME worker COMMAND test_worker ${CMAKE_CROSSCOMPILING_EMULATOR} $<TARGET_FILE:record-manager>)
endif()
