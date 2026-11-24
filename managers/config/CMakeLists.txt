# config-manager: 설정·버전 매니저(기본 탑재, managers/config/design.md). 이 폴더만으로 빌드한다.
#
#   cmake -S managers/config -B ~/config-build && cmake --build ~/config-build -j && ctest --test-dir ~/config-build --output-on-failure
#   확정(Jetson aarch64): dev/release-worker.sh config   → managers/config/bin/config-manager + BUILD
#
# 구성: lib/libworker(사본, 원본 workers/_template/lib/libworker) · lib/config(① core/ 버전·비교·남기기 ② 모듈) · src/main.cpp
#       · test/test_worker.cpp(실행 파일 통합 시험: 가짜 process-manager 소켓)
cmake_minimum_required(VERSION 3.16)
project(config_manager LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(BUILD_TESTING "Build tests" ON)
enable_testing()

add_subdirectory(lib/libworker)
add_subdirectory(lib/config)

add_executable(config-manager src/main.cpp)
target_link_libraries(config-manager PRIVATE config_module)
target_compile_options(config-manager PRIVATE -Wall -Wextra -Wpedantic)
install(TARGETS config-manager RUNTIME DESTINATION bin)

if(BUILD_TESTING)
    # 통합 시험: 가짜 process-manager와 임시 워크스페이스로 버전 목록·스냅숏·diff·되돌리기 확인
    add_executable(test_worker test/test_worker.cpp)
    target_link_libraries(test_worker PRIVATE worker)
    add_test(NAME worker COMMAND test_worker ${CMAKE_CROSSCOMPILING_EMULATOR} $<TARGET_FILE:config-manager>)
endif()
