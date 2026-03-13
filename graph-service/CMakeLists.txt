cmake_minimum_required(VERSION 3.20)

project(graph-service
    VERSION 1.0.0
    DESCRIPTION "Tsurugi Graph Service"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(tateyama REQUIRED)
find_package(glog REQUIRED)
find_package(Protobuf REQUIRED)
find_package(GTest REQUIRED)

include(GNUInstallDirs)

# Proto files
set(PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/proto")
set(PROTO_FILES
    "${PROTO_DIR}/tateyama/proto/graph/request.proto"
    "${PROTO_DIR}/tateyama/proto/graph/response.proto"
)

protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS "${PROTO_DIR}" ${PROTO_FILES})

add_library(graph-service
    src/tateyama/framework/graph/service.cpp
    ${PROTO_SRCS}
)

target_include_directories(graph-service
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(graph-service
    PUBLIC
        tateyama::api
        tateyama::framework
        glog::glog
        protobuf::libprotobuf
)

# Test
if(BUILD_TESTING)
    enable_testing()
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

install(TARGETS graph-service EXPORT graph-service-targets)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
