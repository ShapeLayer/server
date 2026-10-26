cmake_minimum_required(VERSION 3.20)
project(server_slop LANGUAGES C)

# ------------------------------------------------------------------
# 공통 설정
# ------------------------------------------------------------------
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # clangd 등 IDE용 compile_commands.json

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(ENABLE_SANITIZERS "Build with AddressSanitizer + UndefinedBehaviorSanitizer" ON)
option(WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

# 모든 타깃이 공유하는 옵션. 타깃마다 링크해서 사용한다.
add_library(project_options INTERFACE)
target_compile_definitions(project_options INTERFACE
  _DEFAULT_SOURCE # -std=c11에서 glibc가 POSIX 선언을 숨기지 않도록 (macOS에는 영향 없음)
)
target_compile_options(project_options INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
  $<$<BOOL:${WARNINGS_AS_ERRORS}>:-Werror>
)
if(ENABLE_SANITIZERS)
  target_compile_options(project_options INTERFACE
    -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
  target_link_options(project_options INTERFACE -fsanitize=address,undefined)
endif()

# ------------------------------------------------------------------
# core: 재사용 라이브러리 (src/core/*.c)
# ------------------------------------------------------------------
file(GLOB CORE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.c)

# OS별 poller 구현은 하나만 빌드한다. (아직 파일이 없으면 아무 일도 일어나지 않음)
list(FILTER CORE_SOURCES EXCLUDE REGEX "/poller_[a-z]+\\.c$")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(POLLER_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/src/core/poller_epoll.c)
else()
  set(POLLER_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/src/core/poller_kqueue.c)
endif()
if(EXISTS ${POLLER_SOURCE})
  list(APPEND CORE_SOURCES ${POLLER_SOURCE})
endif()

add_library(core STATIC ${CORE_SOURCES})
target_include_directories(core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src) # #include "core/xxx.h"
target_link_libraries(core PUBLIC project_options)

# ------------------------------------------------------------------
# server 실행 파일
# ------------------------------------------------------------------
add_executable(server src/main.c)
target_link_libraries(server PRIVATE core)

# ------------------------------------------------------------------
# 테스트
# ------------------------------------------------------------------
include(CTest) # BUILD_TESTING 옵션 (기본 ON)과 enable_testing()

if(BUILD_TESTING)
  # 러너(main + 등록 테이블). OBJECT 라이브러리로 두어 각 테스트 실행 파일에 그대로 링크한다.
  add_library(test_framework OBJECT tests/framework/test_main.c)
  target_include_directories(test_framework PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_link_libraries(test_framework PUBLIC project_options)

  # tests/test_<module>.c 하나가 실행 파일 test_<module> 하나가 된다.
  file(GLOB TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_*.c)

  # Apple Clang의 ASan은 LeakSanitizer를 지원하지 않는다 (지정하면 즉시 abort).
  if(APPLE)
    set(TEST_ASAN_OPTIONS "abort_on_error=1")
  else()
    set(TEST_ASAN_OPTIONS "detect_leaks=1:abort_on_error=1")
  endif()

  foreach(test_src IN LISTS TEST_SOURCES)
    get_filename_component(test_exe ${test_src} NAME_WE) # test_ringbuf
    string(REGEX REPLACE "^test_" "" module ${test_exe})  # ringbuf

    add_executable(${test_exe} ${test_src})
    target_link_libraries(${test_exe} PRIVATE core test_framework)

    # 소스에서 줄 맨 앞의 TEST(name)을 찾아 케이스마다 ctest 항목으로 등록한다.
    # 테스트 파일을 고치면 CMake가 다시 구성되어 목록이 갱신된다.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${test_src})
    file(STRINGS ${test_src} test_lines REGEX "^TEST\\([A-Za-z0-9_]+\\)")

    foreach(line IN LISTS test_lines)
      string(REGEX REPLACE "^TEST\\(([A-Za-z0-9_]+)\\).*" "\\1" case ${line})
      add_test(NAME ${module}.${case} COMMAND ${test_exe} --exact ${case})
      set_tests_properties(${module}.${case} PROPERTIES
        LABELS ${module}
        TIMEOUT 10
        ENVIRONMENT "ASAN_OPTIONS=${TEST_ASAN_OPTIONS};UBSAN_OPTIONS=print_stacktrace=1"
      )
    endforeach()
  endforeach()

  # cmake --build build --target check : 빌드 후 전체 테스트 실행
  add_custom_target(check
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    USES_TERMINAL
  )
  foreach(test_src IN LISTS TEST_SOURCES)
    get_filename_component(test_exe ${test_src} NAME_WE)
    add_dependencies(check ${test_exe})
  endforeach()
endif()
