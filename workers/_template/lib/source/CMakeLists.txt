# source 모듈(예제: 데이터 만들기). 두 라이브러리로 나눈다(워커 템플릿, 워크스페이스 design.md 6.6.4):
#   source_core : ① 유닛 Generator(core/). libworker 의존 없음 → 다른 프로그램에 core/만 가져가 쓸 수 있다
#   source      : ② 모듈(include/, src/). 스레드·데이터 버스·스트림·명령·자식 프로세스, 포트 SourcePort
# 워커 CMakeLists에서 lib/libworker, lib/libframebus 다음에 add_subdirectory(lib/source)

add_library(source_core STATIC core/src/generator.cpp)
target_include_directories(source_core PUBLIC core/include)
target_compile_features(source_core PUBLIC cxx_std_17)
target_compile_options(source_core PRIVATE -Wall -Wextra -Wpedantic)

add_library(source STATIC src/module.cpp)
target_include_directories(source PUBLIC include)
target_link_libraries(source PUBLIC source_core worker framebus)
target_compile_options(source PRIVATE -Wall -Wextra -Wpedantic)

if(BUILD_TESTING)
    add_executable(test_source test/test_source.cpp)
    target_link_libraries(test_source PRIVATE source)
    add_test(NAME source COMMAND test_source)
endif()
