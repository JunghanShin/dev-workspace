# 모듈 payload: ① 코어 payload_core(모듈 목록·묶음. libworker 모름) ② 모듈 payload_module(호스트에 붙임)
add_library(payload_core STATIC core/src/modules.cpp)
target_include_directories(payload_core PUBLIC core/include ${CMAKE_CURRENT_SOURCE_DIR}/../libworker/third_party)
target_compile_features(payload_core PUBLIC cxx_std_17)
target_compile_options(payload_core PRIVATE -Wall -Wextra -Wpedantic)

add_library(payload_module STATIC src/module.cpp)
target_include_directories(payload_module PUBLIC include)
target_link_libraries(payload_module PUBLIC payload_core worker)
target_compile_options(payload_module PRIVATE -Wall -Wextra -Wpedantic)

if(BUILD_TESTING)
    # 유닛 시험: 모듈 목록(매니저 빼기, 역할·건강 합치기), 묶음 글롭, 규칙 검사
    add_executable(test_payload_core test/test_core.cpp)
    target_link_libraries(test_payload_core PRIVATE payload_core)
    target_compile_options(test_payload_core PRIVATE -Wall -Wextra)
    add_test(NAME payload_core COMMAND test_payload_core)
endif()
