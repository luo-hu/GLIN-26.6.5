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
find_package(Threads REQUIRED)
set(CMAKE_CXX_STANDARD 14)

set(GLIN_DEP_INCLUDE_DIRS
    ${GEOS_INCLUDE_DIR}
    ${Boost_INCLUDE_DIRS})
set(GLIN_CORE_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${GLIN_DEP_INCLUDE_DIRS})
set(GLIN_WKT_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    ${GLIN_DEP_INCLUDE_DIRS})

# SIMD
if(MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /O2 /arch:AVX2 /W1 /EHsc")
elseif (CMAKE_CXX_COMPILER_ID STREQUAL "Intel")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -xHost")
else()
    # clang and gcc
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -march=native -Wall -Wextra")
endif()


add_executable(test_glin test/unittest_main.cpp glin/hilbert/hilbert.h glin/hilbert/hilbert.cpp)
target_link_libraries(test_glin PRIVATE ${GEOS_LIBRARY} ${Boost_SYSTEM_LIBRARY})
target_include_directories(test_glin PUBLIC ${GLIN_CORE_INCLUDE_DIRS})

add_executable(test_glin_piece test/unittest_main.cpp glin/hilbert/hilbert.h glin/hilbert/hilbert.cpp)
target_compile_definitions(test_glin_piece PRIVATE PIECE)
target_link_libraries(test_glin_piece PRIVATE ${GEOS_LIBRARY} ${Boost_SYSTEM_LIBRARY})
target_include_directories(test_glin_piece PUBLIC ${GLIN_CORE_INCLUDE_DIRS})

add_executable(test_hire_internal_directory
    test/hire_internal_directory_test.cpp)
target_include_directories(test_hire_internal_directory PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/benchmark)

add_executable(test_hire_sfc_stage2 test/hire_sfc_stage2_test.cpp)
target_include_directories(test_hire_sfc_stage2 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/benchmark)

add_executable(test_hire_sfc_stage3_cost
    test/hire_sfc_stage3_cost_test.cpp)
target_include_directories(test_hire_sfc_stage3_cost PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/benchmark)

add_executable(test_hire_sfc_stage4_rcu
    test/hire_sfc_stage4_rcu_test.cpp)
target_include_directories(test_hire_sfc_stage4_rcu PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/benchmark)
target_link_libraries(test_hire_sfc_stage4_rcu PRIVATE Threads::Threads)

add_executable(test_hire_sfc_stage5_transform
    test/hire_sfc_stage5_transform_test.cpp)
target_include_directories(test_hire_sfc_stage5_transform PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/benchmark)
target_link_libraries(test_hire_sfc_stage5_transform PRIVATE Threads::Threads)

add_executable(test_hire_sfc_stage6_bulk
    test/hire_sfc_stage6_bulk_test.cpp)
target_include_directories(test_hire_sfc_stage6_bulk PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/benchmark)
target_link_libraries(test_hire_sfc_stage6_bulk PRIVATE Threads::Threads)

add_executable(test_hire_sfc_stage7_acceptance
    test/hire_sfc_stage7_acceptance_test.cpp)
target_include_directories(test_hire_sfc_stage7_acceptance PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/benchmark)
target_link_libraries(test_hire_sfc_stage7_acceptance PRIVATE Threads::Threads)

add_executable(bench_glin_wkt src/benchmark/bench_glin_wkt.cpp glin/hilbert/hilbert.h glin/hilbert/hilbert.cpp)
target_link_libraries(bench_glin_wkt PRIVATE ${GEOS_LIBRARY} ${Boost_SYSTEM_LIBRARY})
target_include_directories(bench_glin_wkt PUBLIC ${GLIN_WKT_INCLUDE_DIRS})

add_executable(bench_glin_wkt_piece src/benchmark/bench_glin_wkt.cpp glin/hilbert/hilbert.h glin/hilbert/hilbert.cpp)
target_compile_definitions(bench_glin_wkt_piece PRIVATE PIECE)
target_link_libraries(bench_glin_wkt_piece PRIVATE ${GEOS_LIBRARY} ${Boost_SYSTEM_LIBRARY})
target_include_directories(bench_glin_wkt_piece PUBLIC ${GLIN_WKT_INCLUDE_DIRS})

add_executable(generate_wkt_queries src/benchmark/generate_wkt_queries.cpp)
target_link_libraries(generate_wkt_queries PRIVATE ${GEOS_LIBRARY} ${Boost_SYSTEM_LIBRARY})
target_include_directories(generate_wkt_queries PUBLIC ${GLIN_DEP_INCLUDE_DIRS})

add_executable(generate_synthetic_points scripts/generate_synthetic_points.cpp)

add_executable(convert_binary_points_to_wkt scripts/convert_binary_points_to_wkt.cpp)

add_executable(generate_synthetic_rectangles scripts/generate_synthetic_rectangles.cpp)

add_executable(bench_boost_rtree_wkt src/benchmark/bench_boost_rtree_wkt.cpp)
target_link_libraries(bench_boost_rtree_wkt PRIVATE ${GEOS_LIBRARY} ${Boost_SYSTEM_LIBRARY})
target_include_directories(bench_boost_rtree_wkt PUBLIC ${GLIN_DEP_INCLUDE_DIRS})

add_executable(bench_geos_quadtree_wkt src/benchmark/bench_geos_quadtree_wkt.cpp)
target_link_libraries(bench_geos_quadtree_wkt PRIVATE ${GEOS_LIBRARY} ${Boost_SYSTEM_LIBRARY})
target_include_directories(bench_geos_quadtree_wkt PUBLIC ${GLIN_DEP_INCLUDE_DIRS})

add_executable(bench_interval_overlap_wkt src/benchmark/bench_interval_overlap_wkt.cpp glin/hilbert/hilbert.h glin/hilbert/hilbert.cpp)
target_link_libraries(bench_interval_overlap_wkt PRIVATE ${GEOS_LIBRARY} ${Boost_SYSTEM_LIBRARY})
target_include_directories(bench_interval_overlap_wkt PUBLIC ${GLIN_WKT_INCLUDE_DIRS})

add_executable(bench_dynamic_extent_wkt src/benchmark/bench_dynamic_extent_wkt.cpp glin/hilbert/hilbert.h glin/hilbert/hilbert.cpp)
target_link_libraries(bench_dynamic_extent_wkt PRIVATE ${GEOS_LIBRARY} ${Boost_SYSTEM_LIBRARY})
target_include_directories(bench_dynamic_extent_wkt PUBLIC ${GLIN_WKT_INCLUDE_DIRS})

add_executable(bench_dynamic_compare_wkt src/benchmark/bench_dynamic_compare_wkt.cpp glin/hilbert/hilbert.h glin/hilbert/hilbert.cpp)
target_compile_definitions(bench_dynamic_compare_wkt PRIVATE PIECE)
target_link_libraries(bench_dynamic_compare_wkt PRIVATE ${GEOS_LIBRARY} ${Boost_SYSTEM_LIBRARY})
target_include_directories(bench_dynamic_compare_wkt PUBLIC ${GLIN_WKT_INCLUDE_DIRS})

add_executable(bench_index_size_wkt src/benchmark/bench_index_size_wkt.cpp glin/hilbert/hilbert.h glin/hilbert/hilbert.cpp)
target_link_libraries(bench_index_size_wkt PRIVATE ${GEOS_LIBRARY} ${Boost_SYSTEM_LIBRARY})
target_include_directories(bench_index_size_wkt PUBLIC ${GLIN_WKT_INCLUDE_DIRS})

add_executable(export_zrange_cdf_wkt src/benchmark/export_zrange_cdf_wkt.cpp glin/hilbert/hilbert.h glin/hilbert/hilbert.cpp)
target_link_libraries(export_zrange_cdf_wkt PRIVATE ${GEOS_LIBRARY} ${Boost_SYSTEM_LIBRARY})
target_include_directories(export_zrange_cdf_wkt PUBLIC ${GLIN_WKT_INCLUDE_DIRS})

add_executable(bench_update_wkt src/benchmark/bench_update_wkt.cpp glin/hilbert/hilbert.h glin/hilbert/hilbert.cpp)
target_link_libraries(bench_update_wkt PRIVATE ${GEOS_LIBRARY} ${Boost_SYSTEM_LIBRARY})
target_include_directories(bench_update_wkt PUBLIC ${GLIN_WKT_INCLUDE_DIRS})

add_executable(bench_update_wkt_piece src/benchmark/bench_update_wkt.cpp glin/hilbert/hilbert.h glin/hilbert/hilbert.cpp)
target_compile_definitions(bench_update_wkt_piece PRIVATE PIECE)
target_link_libraries(bench_update_wkt_piece PRIVATE ${GEOS_LIBRARY} ${Boost_SYSTEM_LIBRARY})
target_include_directories(bench_update_wkt_piece PUBLIC ${GLIN_WKT_INCLUDE_DIRS})

add_executable(bench_hybrid_wkt_piece src/benchmark/bench_hybrid_wkt.cpp glin/hilbert/hilbert.h glin/hilbert/hilbert.cpp)
target_compile_definitions(bench_hybrid_wkt_piece PRIVATE PIECE)
target_link_libraries(bench_hybrid_wkt_piece PRIVATE ${GEOS_LIBRARY} ${Boost_SYSTEM_LIBRARY})
target_include_directories(bench_hybrid_wkt_piece PUBLIC ${GLIN_WKT_INCLUDE_DIRS})

enable_testing()
add_test(NAME test_glin COMMAND test_glin)
add_test(NAME test_glin_piece COMMAND test_glin_piece)
add_test(NAME test_hire_internal_directory COMMAND test_hire_internal_directory)
add_test(NAME test_hire_sfc_stage2 COMMAND test_hire_sfc_stage2)
add_test(NAME test_hire_sfc_stage3_cost COMMAND test_hire_sfc_stage3_cost)
add_test(NAME test_hire_sfc_stage4_rcu COMMAND test_hire_sfc_stage4_rcu)
add_test(NAME test_hire_sfc_stage5_transform COMMAND test_hire_sfc_stage5_transform)
add_test(NAME test_hire_sfc_stage6_bulk COMMAND test_hire_sfc_stage6_bulk)
add_test(NAME test_hire_sfc_stage7_acceptance COMMAND test_hire_sfc_stage7_acceptance)

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
  set_target_properties(test_glin test_glin_piece test_hire_internal_directory test_hire_sfc_stage2 test_hire_sfc_stage3_cost test_hire_sfc_stage4_rcu test_hire_sfc_stage5_transform test_hire_sfc_stage6_bulk test_hire_sfc_stage7_acceptance bench_glin_wkt bench_glin_wkt_piece generate_wkt_queries generate_synthetic_points convert_binary_points_to_wkt generate_synthetic_rectangles bench_boost_rtree_wkt bench_geos_quadtree_wkt bench_interval_overlap_wkt bench_dynamic_extent_wkt bench_dynamic_compare_wkt bench_index_size_wkt export_zrange_cdf_wkt bench_update_wkt bench_update_wkt_piece bench_hybrid_wkt_piece
    PROPERTIES BUILD_RPATH "${GLIN_TEST_LIBRARY_PATHS}")
  list(JOIN GLIN_TEST_LIBRARY_PATHS ":" GLIN_TEST_LIBRARY_PATH)
  set_tests_properties(test_glin test_glin_piece test_hire_internal_directory test_hire_sfc_stage2 test_hire_sfc_stage3_cost test_hire_sfc_stage4_rcu test_hire_sfc_stage5_transform test_hire_sfc_stage6_bulk test_hire_sfc_stage7_acceptance
    PROPERTIES ENVIRONMENT "LD_LIBRARY_PATH=${GLIN_TEST_LIBRARY_PATH}")
endif()
