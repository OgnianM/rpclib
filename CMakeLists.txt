cmake_minimum_required(VERSION 3.26)
project(rpclib)

set(CMAKE_CXX_STANDARD 20)

add_library(rpclib SHARED include/rpc/common.h include/rpc/client.h include/rpc/server.h
        include/rpc/rpc_impl.cpp include/rpc/service_provider.h)

option(RPC_ALLOW_LVALUE_REFS "Allow lvalue references to be used as arguments for remote calls" ON)
option(BUILD_EXAMPLES "Build examples" ON)
option(MSGPACK_USE_BOOST "Use boost for msgpack" OFF)

add_subdirectory(external/msgpack)

target_include_directories(rpclib PUBLIC external/asio/asio/include)
target_include_directories(rpclib PUBLIC external/msgpack/include)

target_link_libraries(rpclib PUBLIC msgpackc ssl crypto)

if (RPC_ALLOW_LVALUE_REFS)
    target_compile_definitions(rpclib PUBLIC RPC_ALLOW_LVALUE_REFS)
endif()


if (BUILD_EXAMPLES)
    add_executable(example_server example/File.h example/ssl_file_transfer_server.cpp)
    add_executable(example_client example/File.h example/ssl_file_transfer_client.cpp)

    target_link_libraries(example_server rpclib)
    target_link_libraries(example_client rpclib)

    target_include_directories(example_server PUBLIC include)
    target_include_directories(example_client PUBLIC include)
endif()
