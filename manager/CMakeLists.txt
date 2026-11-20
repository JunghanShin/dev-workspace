# process-manager: 워크스페이스 Manager의 C++ 판 (docs/process-manager.md). Python 판(workspace_manager/)과
# 같은 API·파일·인자. 시작 셸이 고르기 전까지는 Python 판이 기본.
#
#   cmake -S manager -B ~/pm-build -DCMAKE_BUILD_TYPE=Release && cmake --build ~/pm-build -j
#   ctest --test-dir ~/pm-build                                   # core 단위 시험
#   MANAGER_BIN=~/pm-build/process-manager python3 -m unittest test_manager_api test_manager_accept   # 수락 시험
#
# 구성: core/ 상태 규칙(시스템 호출 모름, 포트로 받음) · src/ 소켓·신호·파일·스레드 · lib/ 사본(libworker 일부, nlohmann)
cmake_minimum_required(VERSION 3.16)
project(process_manager LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(BUILD_TESTING "Build tests" ON)
enable_testing()
find_package(Threads REQUIRED)

# 사본: libworker의 스레드 감시·큐(워커처럼 원본을 고치지 않고 가져옴), nlohmann/json v3.11.3(MIT)
add_library(pm_lib STATIC lib/libworker/src/thread.cpp)
target_include_directories(pm_lib PUBLIC lib/libworker/include lib/third_party)
target_link_libraries(pm_lib PUBLIC Threads::Threads)

add_library(pm_core STATIC
    core/json_util.cpp
    core/log.cpp
    core/schema.cpp
    core/worker_files.cpp
    core/supervisor.cpp
)
target_include_directories(pm_core PUBLIC core lib/third_party)
target_compile_options(pm_core PRIVATE -Wall -Wextra -Wpedantic)

add_library(pm_src STATIC
    src/posix_fs.cpp
    src/store.cpp
    src/spawner.cpp
    src/resources.cpp
    src/publisher.cpp
    src/settings.cpp
    src/app.cpp
)
target_include_directories(pm_src PUBLIC src)
target_link_libraries(pm_src PUBLIC pm_core pm_lib)
target_compile_options(pm_src PRIVATE -Wall -Wextra -Wpedantic)

add_executable(process-manager src/main.cpp)
target_link_libraries(process-manager PRIVATE pm_src)
target_compile_options(process-manager PRIVATE -Wall -Wextra -Wpedantic)
install(TARGETS process-manager RUNTIME DESTINATION bin)

if(BUILD_TESTING)
    # core 단위 시험(가짜 시계·가짜 spawn·가짜 store). 저장소의 워커 스키마·프리셋도 검사한다
    add_executable(test_core test/test_core.cpp)
    target_link_libraries(test_core PRIVATE pm_src)
    target_compile_definitions(test_core PRIVATE PM_REPO_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/..")
    target_compile_options(test_core PRIVATE -Wall -Wextra)
    add_test(NAME core COMMAND test_core)
    # pm.store·pm.pub 스레드 시험(TSan으로도 돌림)
    add_executable(test_threads test/test_threads.cpp)
    target_link_libraries(test_threads PRIVATE pm_src)
    target_compile_options(test_threads PRIVATE -Wall -Wextra)
    add_test(NAME threads COMMAND test_threads)
endif()
