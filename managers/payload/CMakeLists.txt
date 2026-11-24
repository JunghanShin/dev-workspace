# payload-manager: 모듈 운용 매니저(기본 탑재, managers/payload/design.md). 이 폴더만으로 빌드한다.
#
#   cmake -S managers/payload -B ~/payload-build && cmake --build ~/payload-build -j && ctest --test-dir ~/payload-build --output-on-failure
#   확정(Jetson aarch64): dev/release-worker.sh payload   → managers/payload/bin/payload-manager + BUILD
#
# 구성: lib/libworker(사본, 원본 workers/_template/lib/libworker) · lib/payload(① core/ 모듈 목록 ② 모듈) · src/main.cpp
#       · test/test_worker.cpp(실행 파일 통합 시험: 가짜 process-manager 소켓)
cmake_minimum_required(VERSION 3.16)
project(payload_manager LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(BUILD_TESTING "Build tests" ON)
enable_testing()

add_subdirectory(lib/libworker)
add_subdirectory(lib/payload)

add_executable(payload-manager src/main.cpp)
target_link_libraries(payload-manager PRIVATE payload_module)
target_compile_options(payload-manager PRIVATE -Wall -Wextra -Wpedantic)
install(TARGETS payload-manager RUNTIME DESTINATION bin)

if(BUILD_TESTING)
    # 통합 시험: 가짜 process-manager(구독·쓰기)와 가짜 status 주제로 모듈 목록·건강·켜고 끄기·묶음·매니저 거절 확인
    add_executable(test_worker test/test_worker.cpp)
    target_link_libraries(test_worker PRIVATE worker)
    add_test(NAME worker COMMAND test_worker ${CMAKE_CROSSCOMPILING_EMULATOR} $<TARGET_FILE:payload-manager>)
endif()
