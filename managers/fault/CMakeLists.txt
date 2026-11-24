# fault-manager: 이상 판단 매니저(기본 탑재, managers/fault/design.md). 이 폴더만으로 빌드한다.
#
#   cmake -S managers/fault -B ~/fault-build && cmake --build ~/fault-build -j && ctest --test-dir ~/fault-build --output-on-failure
#   확정(Jetson aarch64): dev/release-worker.sh fault   → managers/fault/bin/fault-manager + BUILD
#
# 구성: lib/libworker(사본, 원본 workers/_template/lib/libworker) · lib/fault(① core/ 이상 판단 엔진 ② 모듈) · src/main.cpp
#       · test/test_worker.cpp(실행 파일 통합 시험: 가짜 process-manager 소켓)
cmake_minimum_required(VERSION 3.16)
project(fault_manager LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(BUILD_TESTING "Build tests" ON)
enable_testing()

add_subdirectory(lib/libworker)
add_subdirectory(lib/fault)

add_executable(fault-manager src/main.cpp)
target_link_libraries(fault-manager PRIVATE fault_module)
target_compile_options(fault-manager PRIVATE -Wall -Wextra -Wpedantic)
install(TARGETS fault-manager RUNTIME DESTINATION bin)

if(BUILD_TESTING)
    # 통합 시험: 가짜 status 주제·가짜 mission 명령 소켓으로 이상 올림·대응 한 번·내림 확인
    add_executable(test_worker test/test_worker.cpp)
    target_link_libraries(test_worker PRIVATE worker)
    add_test(NAME worker COMMAND test_worker ${CMAKE_CROSSCOMPILING_EMULATOR} $<TARGET_FILE:fault-manager>)
endif()
