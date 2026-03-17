cmake_minimum_required(VERSION 3.20)

project(graph-service
    VERSION 1.0.0
    DESCRIPTION "Tsurugi Graph Service"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(tateyama-shirakami QUIET)
if(NOT tateyama-shirakami_FOUND)
    find_package(tateyama REQUIRED)
endif()
find_package(sharksfin QUIET)
find_package(glog REQUIRED)
find_package(Protobuf REQUIRED)
find_package(GTest QUIET)

include(GNUInstallDirs)

# Proto files
set(PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/proto")
set(PROTO_FILES
    "${PROTO_DIR}/tateyama/proto/graph/request.proto"
    "${PROTO_DIR}/tateyama/proto/graph/response.proto"
)

set(Protobuf_IMPORT_DIRS "${PROTO_DIR}")
protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ${PROTO_FILES})

add_library(graph-service
    src/tateyama/framework/graph/service.cpp
    src/tateyama/framework/graph/resource.cpp
    src/tateyama/framework/graph/storage.cpp
    src/tateyama/framework/graph/core/parser.cpp
    src/tateyama/framework/graph/core/executor.cpp
    ${PROTO_SRCS}
)

target_include_directories(graph-service
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# Ensure transitive include dirs for takatori (needed by tateyama headers)
if(tateyama-shirakami_FOUND)
    find_path(TAKATORI_INCLUDE_DIR takatori/util/detect.h
        PATHS ${CMAKE_PREFIX_PATH}/include/takatori
        NO_DEFAULT_PATH)
    if(TAKATORI_INCLUDE_DIR)
        target_include_directories(graph-service PUBLIC ${TAKATORI_INCLUDE_DIR})
    endif()
endif()

if(tateyama-shirakami_FOUND)
    set(TATEYAMA_API_TARGET tateyama-shirakami-api)
    set(TATEYAMA_ENGINE_TARGET tateyama-shirakami-engine)
else()
    set(TATEYAMA_API_TARGET tateyama::api)
    set(TATEYAMA_ENGINE_TARGET tateyama::framework)
endif()

target_link_libraries(graph-service
    PUBLIC
        ${TATEYAMA_API_TARGET}
        ${TATEYAMA_ENGINE_TARGET}
        glog::glog
        protobuf::libprotobuf
)

# Test
enable_testing()

if(BUILD_TESTING AND GTest_FOUND)
    add_executable(graph_service_test
        test/tateyama/framework/graph/service_test.cpp
    )
    target_link_libraries(graph_service_test
        PRIVATE
            graph-service
            GTest::gtest_main
    )
    add_test(NAME graph_service_test COMMAND graph_service_test)
endif()

# Standalone mock-based tests (no GTest/sharksfin dependency)
set(GRAPH_SRC_FILES
    src/tateyama/framework/graph/storage.cpp
    src/tateyama/framework/graph/core/parser.cpp
    src/tateyama/framework/graph/core/executor.cpp
)

# Tests that use storage + executor (need mock sharksfin)
set(MOCK_TESTS
    executor_test
    executor_advanced_test
    executor_batch_test
    storage_standalone_test
    storage_label_test
    storage_navigation_test
    storage_property_test
    storage_iterator_test
    match_label_test
)

# Note: retry_test, service_test, service_full_test, storage_test require
# GTest/protobuf/tateyama framework and are built only in the full server context

# Tests that only use parser (no mock needed)
set(PARSER_ONLY_TESTS
    parser_standalone_test
    parser_full_test
    parser_edge_test
    parser_property_test
    query_cache_test
)

foreach(test_name ${MOCK_TESTS})
    add_executable(${test_name}
        test/tateyama/framework/graph/${test_name}.cpp
        ${GRAPH_SRC_FILES}
    )
    target_include_directories(${test_name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/test
        ${CMAKE_CURRENT_SOURCE_DIR}/test/tateyama/framework/graph
    )
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

foreach(test_name ${PARSER_ONLY_TESTS})
    add_executable(${test_name}
        test/tateyama/framework/graph/${test_name}.cpp
        src/tateyama/framework/graph/core/parser.cpp
    )
    target_include_directories(${test_name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/test
        ${CMAKE_CURRENT_SOURCE_DIR}/test/tateyama/framework/graph
    )
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

install(TARGETS graph-service EXPORT graph-service-targets)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# Install generated proto headers
install(FILES ${PROTO_HDRS} DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

# Install cmake config for find_package(graph-service)
install(EXPORT graph-service-targets
    FILE graph-service-config.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/graph-service
)
