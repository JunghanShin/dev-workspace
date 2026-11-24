# 모듈 mission: ① 코어 mission_core(명령 → 역할, (대상, 역할) 대기 큐. libworker 모름) ② 모듈 mission_module(호스트에 붙임)
add_library(mission_core STATIC core/src/dispatch.cpp)
target_include_directories(mission_core PUBLIC core/include ${CMAKE_CURRENT_SOURCE_DIR}/../libworker/third_party)
target_compile_features(mission_core PUBLIC cxx_std_17)
target_compile_options(mission_core PRIVATE -Wall -Wextra -Wpedantic)

add_library(mission_module STATIC src/module.cpp)
target_include_directories(mission_module PUBLIC include)
target_link_libraries(mission_module PUBLIC mission_core worker)
target_compile_options(mission_module PRIVATE -Wall -Wextra -Wpedantic)

if(BUILD_TESTING)
    # 유닛 시험: 계약 → 역할, 한 번에 하나·대기·busy·timeout
    add_executable(test_mission_core test/test_core.cpp)
    target_link_libraries(test_mission_core PRIVATE mission_core)
    target_compile_options(test_mission_core PRIVATE -Wall -Wextra)
    add_test(NAME mission_core COMMAND test_mission_core)
endif()
