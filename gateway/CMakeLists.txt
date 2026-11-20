# gateway: 워크스페이스 REST의 C++ 판 (docs/gateway.md 6장 1~2단계). Python 판(rest/workspace_rest)과 같은
# 경로·응답·상태 코드·SSE 모양·인자·환경 변수. 신분 확인은 WORKSPACE_AUTH=off만(지금 REST와 같음).
#
#   cmake -S gateway -B ~/gw-build -DCMAKE_BUILD_TYPE=Release && cmake --build ~/gw-build -j
#   ctest --test-dir ~/gw-build --output-on-failure                          # core·스레드 단위 시험
#   cd rest && REST_BIN=~/gw-build/gateway python3 -m unittest test_rest_accept -v   # 수락 시험(Python 판은 REST_BIN 없이)
#
#   TSan: cmake -S gateway -B ~/gw-build-tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DGW_TSAN=ON && cmake --build ~/gw-build-tsan -j
#         TSAN_OPTIONS=halt_on_error=1 setarch $(uname -m) -R ~/gw-build-tsan/test_threads
#
# 구성: core/ 규칙(경로 표·SSE 합치기·지난 지표·비밀값 가림, 시스템 호출 없음) · src/ HTTP·소켓·스레드·파일
#       · lib/ 사본(libworker 스레드 감시·큐, nlohmann/json v3.11.3, cpp-httplib v0.18.3 — 모두 MIT)
cmake_minimum_required(VERSION 3.16)
project(gateway LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(BUILD_TESTING "Build tests" ON)
option(GW_TSAN "ThreadSanitizer build" OFF)
if(GW_TSAN)
    add_compile_options(-fsanitize=thread -g -O1)
    add_link_options(-fsanitize=thread)
endif()
enable_testing()
find_package(Threads REQUIRED)

add_library(gw_lib STATIC lib/libworker/src/thread.cpp)
target_include_directories(gw_lib PUBLIC lib/libworker/include lib/third_party)
target_link_libraries(gw_lib PUBLIC Threads::Threads)
# cpp-httplib: select 대신 poll(FD_SETSIZE 1024 한계 없음). 모든 번역 단위가 같은 값이어야 해서 여기서 정함
target_compile_definitions(gw_lib PUBLIC CPPHTTPLIB_USE_POLL)

add_library(gw_core STATIC
    core/text.cpp
    core/routes.cpp
    core/ui_rules.cpp
    core/stream_state.cpp
    core/history.cpp
    core/profile_rules.cpp
    core/board_parse.cpp
    core/pipeline.cpp
)
target_include_directories(gw_core PUBLIC core lib/third_party)
target_compile_options(gw_core PRIVATE -Wall -Wextra -Wpedantic)

add_library(gw_src STATIC
    src/log.cpp
    src/sys.cpp
    src/proc.cpp
    src/hub.cpp
    src/reads.cpp
    src/ui.cpp
    src/productbuild.cpp
    src/server.cpp
)
target_include_directories(gw_src PUBLIC src)
target_link_libraries(gw_src PUBLIC gw_core gw_lib)
target_compile_options(gw_src PRIVATE -Wall -Wextra)

add_executable(gateway src/main.cpp)
target_link_libraries(gateway PRIVATE gw_src)
target_compile_options(gateway PRIVATE -Wall -Wextra -Wpedantic)
install(TARGETS gateway RUNTIME DESTINATION bin)

if(BUILD_TESTING)
    # core 단위 시험(시스템 호출 없음)
    add_executable(test_core test/test_core.cpp)
    target_link_libraries(test_core PRIVATE gw_core)
    target_compile_options(test_core PRIVATE -Wall -Wextra)
    add_test(NAME core COMMAND test_core)
    # 스레드 시험: Hub·Upstream·Subscriber(가짜 워커 소켓), SSE 간격, 동시 요청 상한(TSan으로도 돌림)
    add_executable(test_threads test/test_threads.cpp)
    target_link_libraries(test_threads PRIVATE gw_src)
    target_compile_options(test_threads PRIVATE -Wall -Wextra)
    add_test(NAME threads COMMAND test_threads)
endif()
