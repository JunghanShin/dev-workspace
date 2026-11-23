# 워커 틀(example-worker). 이 폴더만으로 빌드한다(jetson-build --native --test, jetson-build --test).
# 워크스페이스 밖에서 쓸 때: workspace/ 폴더를 지우고 -DWORKER_WORKSPACE=OFF로 빌드(libworker의 옵션)
#
# 모듈 셋(워크스페이스 design.md 6.6.3·6.6.7):
#   source     데이터 만들기: 스레드, 데이터 버스·스트림 발행, 명령, 바로 적용, 자식 프로세스 예, 포트 SourcePort
#   processor  워커 안 처리: source의 포트로만 받는다(모듈 사이 연결)
#   peer       워커 간 받기: 설정 peer(다른 워커 id)의 데이터 버스·스트림·명령(워커 사이 연결)
# 필요 없는 모듈은 여기와 src/main.cpp에서 한 줄씩 지운다(틀 README "모듈 떼기")
cmake_minimum_required(VERSION 3.16)
project(example_worker LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_TESTING "Build tests" ON)
enable_testing()

add_subdirectory(lib/libworker)    # 실행 뼈대(+ WORKER_WORKSPACE면 워크스페이스 연동)
add_subdirectory(lib/libframebus)  # 데이터 버스(큰 데이터를 워커 사이에 복사 없이)
add_subdirectory(lib/source)
add_subdirectory(lib/processor)
add_subdirectory(lib/peer)

add_executable(example-worker src/main.cpp)
target_link_libraries(example-worker PRIVATE source processor peer)
target_compile_options(example-worker PRIVATE -Wall -Wextra -Wpedantic)
worker_io(example-worker ${CMAKE_CURRENT_SOURCE_DIR})  # 입출력 정의를 실행 파일에(`example-worker --io`)
install(TARGETS example-worker RUNTIME DESTINATION bin)

# 가짜 장치 프로그램(가짜 장치 모드, 프리셋 sim. 워크스페이스 design.md 6.6.8). 실행 파일 옆(bin/)에 설치
add_executable(example-sim tools/example-sim.cpp)
target_compile_options(example-sim PRIVATE -Wall -Wextra -Wpedantic)
install(TARGETS example-sim RUNTIME DESTINATION bin)

if(BUILD_TESTING)
    # 워커 통합 시험: 실행 파일을 띄워 명령·상태·지표를 확인하고, 둘을 띄워 워커 간 통신(데이터 버스·스트림·명령)을 확인
    add_executable(test_worker test/test_worker.cpp)
    target_link_libraries(test_worker PRIVATE worker)
    add_test(NAME worker COMMAND test_worker ${CMAKE_CROSSCOMPILING_EMULATOR} $<TARGET_FILE:example-worker>)
endif()
