# processor 모듈(예제: 워커 안 처리). 두 라이브러리:
#   processor_core : ① 유닛 Window(core/). libworker 의존 없음
#   processor      : ② 모듈. source의 포트(source/port.hpp)만 안다 — 워커 안 모듈 연결의 본보기
# 워커 CMakeLists에서 lib/source 다음에 add_subdirectory(lib/processor)

add_library(processor_core STATIC core/src/window.cpp)
target_include_directories(processor_core PUBLIC core/include)
target_compile_features(processor_core PUBLIC cxx_std_17)
target_compile_options(processor_core PRIVATE -Wall -Wextra -Wpedantic)

add_library(processor STATIC src/module.cpp)
# 포트 헤더만 쓴다(source 모듈 라이브러리에 링크하지 않음)
target_include_directories(processor PUBLIC include ${CMAKE_CURRENT_SOURCE_DIR}/../source/include)
target_link_libraries(processor PUBLIC processor_core worker)
target_compile_options(processor PRIVATE -Wall -Wextra -Wpedantic)

if(BUILD_TESTING)
    add_executable(test_processor test/test_processor.cpp)
    target_link_libraries(test_processor PRIVATE processor)
    add_test(NAME processor COMMAND test_processor)
endif()
