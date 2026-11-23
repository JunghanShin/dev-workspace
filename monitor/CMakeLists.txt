# sysmon: 시스템 모니터(기본 탑재 워커 system-monitor, monitor/design.md). 이 폴더만으로 빌드한다.
#
#   cmake -S monitor -B ~/mon-build && cmake --build ~/mon-build -j && ctest --test-dir ~/mon-build --output-on-failure
#   확정(Jetson aarch64): dev/release-worker.sh monitor   → monitor/bin/sysmon + BUILD
#
# 구성: lib/libworker(사본, 원본 workers/_template/lib/libworker) · lib/sysmon(① core/ 코어 ② 모듈) · src/main.cpp
#       · test/test_worker.cpp(실행 파일 통합 시험: 가짜 tegrastats·수집기 파일)
cmake_minimum_required(VERSION 3.16)
project(sysmon LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(BUILD_TESTING "Build tests" ON)
enable_testing()

add_subdirectory(lib/libworker)  # 실행 뼈대(+ 워크스페이스 연동)
add_subdirectory(lib/sysmon)

add_executable(sysmon src/main.cpp)
target_link_libraries(sysmon PRIVATE sysmon_module)
target_compile_options(sysmon PRIVATE -Wall -Wextra -Wpedantic)
install(TARGETS sysmon RUNTIME DESTINATION bin)

if(BUILD_TESTING)
    # 통합 시험: 실행 파일을 띄워 READY·heartbeat 지표·출처(수집기/직접 실행)·exit code·SIGTERM 정리를 확인.
    # 가짜 tegrastats는 셸 스크립트라 Jetson QEMU 시험(jetson-build --test)에서는 건너뛴다(에뮬레이터를 받으면)
    add_executable(test_worker test/test_worker.cpp)
    target_link_libraries(test_worker PRIVATE worker)
    add_test(NAME worker COMMAND test_worker ${CMAKE_CROSSCOMPILING_EMULATOR} $<TARGET_FILE:sysmon>)
endif()
