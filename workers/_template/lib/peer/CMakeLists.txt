# peer 모듈(예제: 워커 간 받기). 두 라이브러리:
#   peer_core : ① 유닛 SeqTracker(core/, 헤더만). libworker 의존 없음
#   peer      : ② 모듈. 상대 워커의 데이터 버스(InOrder)·스트림(Subscriber)·명령(CommandClient) — 워커 간 연결의 본보기
# 워커 CMakeLists에서 lib/libworker, lib/libframebus 다음에 add_subdirectory(lib/peer)

add_library(peer_core INTERFACE)
target_include_directories(peer_core INTERFACE core/include)
target_compile_features(peer_core INTERFACE cxx_std_17)

add_library(peer STATIC src/module.cpp)
target_include_directories(peer PUBLIC include)
target_link_libraries(peer PUBLIC peer_core worker framebus)
target_compile_options(peer PRIVATE -Wall -Wextra -Wpedantic)

if(BUILD_TESTING)
    add_executable(test_peer test/test_peer.cpp)
    target_link_libraries(test_peer PRIVATE peer)
    add_test(NAME peer COMMAND test_peer)
endif()
