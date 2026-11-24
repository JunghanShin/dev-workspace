# 모듈 status: ① 코어 status_core(건강 판정 엔진. libworker 모름) ② 모듈 status_module(호스트에 붙임: 구독·발행·명령)
add_library(status_core STATIC core/src/health.cpp)
target_include_directories(status_core PUBLIC core/include ${CMAKE_CURRENT_SOURCE_DIR}/../libworker/third_party)
target_compile_features(status_core PUBLIC cxx_std_17)
target_compile_options(status_core PRIVATE -Wall -Wextra -Wpedantic)

add_library(status_module STATIC src/module.cpp)
target_include_directories(status_module PUBLIC include)
target_link_libraries(status_module PUBLIC status_core worker)
target_compile_options(status_module PRIVATE -Wall -Wextra -Wpedantic)

if(BUILD_TESTING)
    # 유닛 시험: 워커 건강 표, 시스템 건강(필수 규칙·autostart 기본), 바뀔 때만 이벤트, 규칙 검사
    add_executable(test_status_core test/test_core.cpp)
    target_link_libraries(test_status_core PRIVATE status_core)
    target_compile_options(test_status_core PRIVATE -Wall -Wextra)
    add_test(NAME status_core COMMAND test_status_core)
endif()
