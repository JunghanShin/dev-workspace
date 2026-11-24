# 모듈 fault: ① 코어 fault_core(규칙·이상 판단 엔진. libworker 모름) ② 모듈 fault_module(호스트에 붙임)
add_library(fault_core STATIC core/src/engine.cpp)
target_include_directories(fault_core PUBLIC core/include ${CMAKE_CURRENT_SOURCE_DIR}/../libworker/third_party)
target_compile_features(fault_core PUBLIC cxx_std_17)
target_compile_options(fault_core PRIVATE -Wall -Wextra -Wpedantic)

add_library(fault_module STATIC src/module.cpp)
target_include_directories(fault_module PUBLIC include)
target_link_libraries(fault_module PUBLIC fault_core worker)
target_compile_options(fault_module PRIVATE -Wall -Wextra -Wpedantic)

if(BUILD_TESTING)
    # 유닛 시험: 조건별 올림·내림, for_s·hold_s, 워커마다, 대응 한 번, 개발 모드 건너뜀, status_lost, 규칙 검사
    add_executable(test_fault_core test/test_core.cpp)
    target_link_libraries(test_fault_core PRIVATE fault_core)
    target_compile_options(test_fault_core PRIVATE -Wall -Wextra)
    add_test(NAME fault_core COMMAND test_fault_core)
endif()
