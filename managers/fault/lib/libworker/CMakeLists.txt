# libworker: C/C++ 워커 공통 뼈대. 워크스페이스에 의존하지 않는다.
# 워커마다 자기 폴더 lib/libworker/에 사본을 둔다(원본 틀: workers/_template/lib/libworker). 워커 CMakeLists에서:
#     add_subdirectory(lib/libworker)
#     target_link_libraries(<워커> PRIVATE worker)
# 사본은 그 워커의 것이다. 원본이 좋아졌을 때 가져올지는 워커마다 정한다.
cmake_minimum_required(VERSION 3.16)
project(libworker VERSION 0.16.0 LANGUAGES CXX)

# 워크스페이스 연동(heartbeat, 데이터 폴더 기준 소켓 경로). 워크스페이스 밖에서 쓰면 -DWORKER_WORKSPACE=OFF
option(WORKER_WORKSPACE "workspace integration (Manager heartbeat, data-folder socket paths)" ON)

if(NOT TARGET worker)
    find_package(Threads REQUIRED)
    if(WORKER_WORKSPACE)
        set(WORKER_INTEGRATION src/workspace/workspace.cpp)
    else()
        set(WORKER_INTEGRATION src/standalone.cpp)
    endif()
    add_library(worker STATIC
        src/config.cpp
        src/runtime.cpp
        src/command_server.cpp
        src/child.cpp
        src/client.cpp
        src/thread.cpp
        src/stage.cpp
        src/publisher.cpp
        src/peer.cpp
        src/guard.cpp
        src/inputs.cpp
        src/io.cpp
        src/manager_link.cpp
        src/host.cpp
        ${WORKER_INTEGRATION}
    )
    target_compile_definitions(worker PUBLIC WORKER_WORKSPACE=$<BOOL:${WORKER_WORKSPACE}>)
    target_compile_definitions(worker PRIVATE WORKER_LIBWORKER_VERSION="${PROJECT_VERSION}")
    target_compile_features(worker PUBLIC cxx_std_17)
    # JSON은 헤더 하나짜리 nlohmann/json v3.11.3(MIT). 워커도 같은 헤더를 쓴다
    target_include_directories(worker PUBLIC include third_party)
    target_compile_options(worker PRIVATE -Wall -Wextra -Wpedantic)
    # 스레드·큐(thread.hpp, queue.hpp)
    target_link_libraries(worker PUBLIC Threads::Threads)
endif()

# worker_io(<실행 파일 대상> <워커 폴더>): 워커 폴더의 입출력 정의를 실행 파일에 넣는다(`<워커> --io`, include/worker/io.hpp).
# 담는 파일: io.json, io/*.json, io/uses/*.json, lib/*/io/*.json(include 조각, libworker의 공통 정의 io/common.json).
# 파일이 바뀌면 cmake가 다시 돌아 생성 파일을 새로 쓴다
if(NOT COMMAND worker_io)
    function(worker_io target folder)
        file(GLOB io_files RELATIVE "${folder}"
             "${folder}/io.json" "${folder}/io/*.json" "${folder}/io/uses/*.json" "${folder}/lib/*/io/*.json")
        list(SORT io_files)
        set(src "// 생성 파일(libworker worker_io): 워커 폴더의 입출력 정의. 고치지 말 것\n#include \"worker/io.hpp\"\nnamespace {\nconst worker::IoFiles kFiles = {\n")
        foreach(f IN LISTS io_files)
            file(READ "${folder}/${f}" text)
            string(APPEND src "    {\"${f}\", R\"wio(${text})wio\"},\n")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${folder}/${f}")
        endforeach()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${folder}/io" "${folder}/io/uses")
        string(APPEND src "};\n[[maybe_unused]] const bool kRegistered = worker::register_io_files(kFiles);\n}  // namespace\n")
        set(out "${CMAKE_CURRENT_BINARY_DIR}/${target}_io.cpp")
        file(WRITE "${out}.new" "${src}")
        configure_file("${out}.new" "${out}" COPYONLY)
        target_sources(${target} PRIVATE "${out}")
    endfunction()
endif()

# 이 폴더에서 직접 빌드할 때만 단위 시험(jetson-build --test). 워커가 add_subdirectory로 쓸 때는 만들지 않는다
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    enable_testing()
    add_executable(test_libworker test/test_libworker.cpp)
    target_link_libraries(test_libworker PRIVATE worker)
    target_compile_options(test_libworker PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME libworker COMMAND test_libworker)
endif()
