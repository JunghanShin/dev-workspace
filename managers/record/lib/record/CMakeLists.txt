# 모듈 record: ① 코어 record_core(플랫폼 기록 규칙·기록 파일·찾기. libworker 모름) ② 모듈 record_module(호스트에 붙임)
add_library(record_core STATIC core/src/platform.cpp core/src/store.cpp)
target_include_directories(record_core PUBLIC core/include ${CMAKE_CURRENT_SOURCE_DIR}/../libworker/third_party)
target_compile_features(record_core PUBLIC cxx_std_17)
target_compile_options(record_core PRIVATE -Wall -Wextra -Wpedantic)

add_library(record_module STATIC src/module.cpp)
target_include_directories(record_module PUBLIC include)
target_link_libraries(record_module PUBLIC record_core worker)
target_compile_options(record_module PRIVATE -Wall -Wextra -Wpedantic)

if(BUILD_TESTING)
    # 유닛 시험: 플랫폼 규칙(예전 logger와 같은 줄)·파일 나누기·지우기·이어 쓰기·찾기
    add_executable(test_record_core test/test_core.cpp)
    target_link_libraries(test_record_core PRIVATE record_core)
    target_compile_options(test_record_core PRIVATE -Wall -Wextra)
    add_test(NAME record_core COMMAND test_record_core)
endif()
