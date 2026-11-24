# mission-manager: 명령 송수신 매니저(기본 탑재, managers/mission/design.md). 이 폴더만으로 빌드한다.
#
#   cmake -S managers/mission -B ~/mission-build && cmake --build ~/mission-build -j && ctest --test-dir ~/mission-build --output-on-failure
#   확정(Jetson aarch64): dev/release-worker.sh mission   → managers/mission/bin/mission-manager + BUILD
#
# 구성: lib/libworker(사본, 원본 workers/_template/lib/libworker) · lib/mission(① core/ 명령 정리 엔진 ② 모듈) · src/main.cpp
#       · test/test_worker.cpp(실행 파일 통합 시험: 가짜 process-manager 소켓)
cmake_minimum_required(VERSION 3.16)
project(mission_manager LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(BUILD_TESTING "Build tests" ON)
enable_testing()

add_subdirectory(lib/libworker)
add_subdirectory(lib/mission)

add_executable(mission-manager src/main.cpp)
target_link_libraries(mission-manager PRIVATE mission_module)
target_compile_options(mission-manager PRIVATE -Wall -Wextra -Wpedantic)
install(TARGETS mission-manager RUNTIME DESTINATION bin)

if(BUILD_TESTING)
    # 통합 시험: 가짜 워커 명령 소켓(느린 응답)에 같은 역할 동시 명령은 차례로·다른 역할은 함께·큐가 차면 busy·origin 확인
    add_executable(test_worker test/test_worker.cpp)
    target_link_libraries(test_worker PRIVATE worker)
    add_test(NAME worker COMMAND test_worker ${CMAKE_CROSSCOMPILING_EMULATOR} $<TARGET_FILE:mission-manager>)
endif()
