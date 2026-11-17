# replay-worker: 데이터 버스 녹화·재생(워크스페이스 design.md 6.7). 이 폴더만으로 빌드(jetson-build --native --test).
# 워크스페이스 밖에서 쓸 때: workspace/ 폴더를 지우고 -DWORKER_WORKSPACE=OFF로 빌드(libworker의 옵션)
#
# 모듈: replay(녹화: 다른 워커의 데이터 버스 → 파일, 재생: 파일 → 이 워커의 데이터 버스)
cmake_minimum_required(VERSION 3.16)
project(replay_worker LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_TESTING "Build tests" ON)
enable_testing()

add_subdirectory(lib/libworker)    # 실행 뼈대(+ WORKER_WORKSPACE면 워크스페이스 연동)
add_subdirectory(lib/libframebus)  # 데이터 버스
add_subdirectory(lib/replay)

add_executable(replay-worker src/main.cpp)
target_link_libraries(replay-worker PRIVATE replay)
target_compile_options(replay-worker PRIVATE -Wall -Wextra -Wpedantic)
install(TARGETS replay-worker RUNTIME DESTINATION bin)

if(BUILD_TESTING)
    # 워커 통합 시험: 가짜 생산자의 데이터 버스를 녹화 → 재생 → 받는 쪽이 같은 내용·순서인지
    add_executable(test_worker test/test_worker.cpp)
    target_link_libraries(test_worker PRIVATE worker framebus)
    add_test(NAME worker COMMAND test_worker ${CMAKE_CROSSCOMPILING_EMULATOR} $<TARGET_FILE:replay-worker>)
endif()
