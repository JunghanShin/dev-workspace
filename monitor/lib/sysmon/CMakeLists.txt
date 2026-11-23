# 모듈 sysmon: ① 코어 sysmon_core(tegrastats 한 줄 → 지표, /proc·/sys 지표. libworker 모름) ② 모듈 sysmon_module(호스트에 붙임)
add_library(sysmon_core STATIC core/src/metrics.cpp core/src/tegrastats.cpp core/src/extras.cpp)
target_include_directories(sysmon_core PUBLIC core/include)
target_compile_features(sysmon_core PUBLIC cxx_std_17)
target_compile_options(sysmon_core PRIVATE -Wall -Wextra -Wpedantic)

add_library(sysmon_module STATIC src/module.cpp)
target_include_directories(sysmon_module PUBLIC include)
target_link_libraries(sysmon_module PUBLIC sysmon_core worker)
target_compile_options(sysmon_module PRIVATE -Wall -Wextra -Wpedantic)

if(BUILD_TESTING)
    # 유닛 시험: JetPack 판별 tegrastats 줄, 가짜 /proc·/sys 나무 → 지표 이름·값
    add_executable(test_sysmon_core test/test_core.cpp)
    target_link_libraries(test_sysmon_core PRIVATE sysmon_core)
    target_compile_options(test_sysmon_core PRIVATE -Wall -Wextra)
    add_test(NAME sysmon_core COMMAND test_sysmon_core)
endif()
