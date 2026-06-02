cmake_minimum_required(VERSION 3.12)
project(glin)
set(CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake) # find geos path
if(APPLE) # This has to be manually setup
#    set(Boost_INCLUDE_DIR /usr/local/Cellar/boost/1.75.0/include)
    set(CMAKE_FIND_FRAMEWORK LAST)
    set(CMAKE_FIND_APPBUNDLE LAST)
endif()
set(Boost_USE_MULTITHREADED ON)
# FindGEOS.cmake is copied from https://github.com/valhalla/valhalla/blob/master/cmake/FindGEOS.cmake
find_package(GEOS REQUIRED)# if not find geo, cannot compile find geos.cmake
find_package(Boost REQUIRED)
set(CMAKE_CXX_STANDARD 14)


# SIMD
if(MSVC)
    set(CMAKE_CXX_FLAGS "/O2 /arch:AVX2 /W1 /EHsc")
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "Intel")
    set(CMAKE_CXX_FLAGS "-O3 -xHost")
else()
    # clang and gcc
    set(CMAKE_CXX_FLAGS "-O3 -march=native -Wall -Wextra")
endif()


add_executable(test_glin test/unittest_main.cpp glin/hilbert/hilbert.h glin/hilbert/hilbert.cpp)
target_link_libraries(test_glin PRIVATE ${GEOS_LIBRARY} ${Boost_SYSTEM_LIBRARY})
target_include_directories(test_glin PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core;${GEOS_INCLUDE_DIR};${Boost_INCLUDE_DIRS}>")

add_executable(test_glin_piece test/unittest_main.cpp glin/hilbert/hilbert.h glin/hilbert/hilbert.cpp)
target_compile_definitions(test_glin_piece PRIVATE PIECE)
target_link_libraries(test_glin_piece PRIVATE ${GEOS_LIBRARY} ${Boost_SYSTEM_LIBRARY})
target_include_directories(test_glin_piece PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/core;${GEOS_INCLUDE_DIR};${Boost_INCLUDE_DIRS}>")

add_executable(bench_glin_wkt src/benchmark/bench_glin_wkt.cpp glin/hilbert/hilbert.h glin/hilbert/hilbert.cpp)
target_link_libraries(bench_glin_wkt PRIVATE ${GEOS_LIBRARY} ${Boost_SYSTEM_LIBRARY})
target_include_directories(bench_glin_wkt PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR};${CMAKE_CURRENT_SOURCE_DIR}/src/core;${GEOS_INCLUDE_DIR};${Boost_INCLUDE_DIRS}>")

add_executable(bench_glin_wkt_piece src/benchmark/bench_glin_wkt.cpp glin/hilbert/hilbert.h glin/hilbert/hilbert.cpp)
target_compile_definitions(bench_glin_wkt_piece PRIVATE PIECE)
target_link_libraries(bench_glin_wkt_piece PRIVATE ${GEOS_LIBRARY} ${Boost_SYSTEM_LIBRARY})
target_include_directories(bench_glin_wkt_piece PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR};${CMAKE_CURRENT_SOURCE_DIR}/src/core;${GEOS_INCLUDE_DIR};${Boost_INCLUDE_DIRS}>")

add_executable(generate_wkt_queries src/benchmark/generate_wkt_queries.cpp)
target_link_libraries(generate_wkt_queries PRIVATE ${GEOS_LIBRARY} ${Boost_SYSTEM_LIBRARY})
target_include_directories(generate_wkt_queries PUBLIC "$<BUILD_INTERFACE:${GEOS_INCLUDE_DIR};${Boost_INCLUDE_DIRS}>")

add_executable(bench_boost_rtree_wkt src/benchmark/bench_boost_rtree_wkt.cpp)
target_link_libraries(bench_boost_rtree_wkt PRIVATE ${GEOS_LIBRARY} ${Boost_SYSTEM_LIBRARY})
target_include_directories(bench_boost_rtree_wkt PUBLIC "$<BUILD_INTERFACE:${GEOS_INCLUDE_DIR};${Boost_INCLUDE_DIRS}>")

enable_testing()
add_test(NAME test_glin COMMAND test_glin)
add_test(NAME test_glin_piece COMMAND test_glin_piece)

if(UNIX AND NOT APPLE)
  set(GLIN_TEST_LIBRARY_PATHS /usr/lib/x86_64-linux-gnu)
  foreach(prefix IN LISTS CMAKE_PREFIX_PATH)
    if(EXISTS "${prefix}/lib")
      list(APPEND GLIN_TEST_LIBRARY_PATHS "${prefix}/lib")
    endif()
  endforeach()
  if(DEFINED ENV{LD_LIBRARY_PATH})
    list(APPEND GLIN_TEST_LIBRARY_PATHS "$ENV{LD_LIBRARY_PATH}")
  endif()
  set_target_properties(test_glin test_glin_piece bench_glin_wkt bench_glin_wkt_piece generate_wkt_queries bench_boost_rtree_wkt
    PROPERTIES BUILD_RPATH "${GLIN_TEST_LIBRARY_PATHS}")
  list(JOIN GLIN_TEST_LIBRARY_PATHS ":" GLIN_TEST_LIBRARY_PATH)
  set_tests_properties(test_glin test_glin_piece
    PROPERTIES ENVIRONMENT "LD_LIBRARY_PATH=${GLIN_TEST_LIBRARY_PATH}")
endif()
