# replay 모듈(데이터 버스 녹화·재생). 두 라이브러리로 나눈다(워커 템플릿, 워크스페이스 design.md 6.6.4):
#   replay_core : ① 유닛 녹화 파일(core/: Writer·Reader·목록). libworker 의존 없음(JSON 헤더만)
#   replay      : ② 모듈(include/, src/). 녹화 스레드(replay.rec)·재생 스레드(replay.play), 명령·상태·지표
add_library(replay_core STATIC core/src/recording.cpp)
target_include_directories(replay_core PUBLIC core/include)
target_include_directories(replay_core SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../libworker/third_party)
target_compile_features(replay_core PUBLIC cxx_std_17)
target_compile_options(replay_core PRIVATE -Wall -Wextra -Wpedantic)

add_library(replay STATIC src/module.cpp)
target_include_directories(replay PUBLIC include)
target_link_libraries(replay PUBLIC replay_core worker framebus)
target_compile_options(replay PRIVATE -Wall -Wextra -Wpedantic)

if(BUILD_TESTING)
    add_executable(test_replay_recording test/test_recording.cpp)
    target_link_libraries(test_replay_recording PRIVATE replay_core)
    add_test(NAME replay_recording COMMAND test_replay_recording)
    add_executable(test_replay_module test/test_module.cpp)
    target_link_libraries(test_replay_module PRIVATE replay)
    add_test(NAME replay_module COMMAND test_replay_module)
endif()
