cmake_minimum_required(VERSION 3.25)
project(rpclib)

add_library(rpclib STATIC include/rpc/node.h src/node.cpp)

target_compile_features(rpclib PUBLIC cxx_std_20)

option(RPC_ALLOW_LVALUE_REFS "Allow lvalue references to be used as arguments for remote calls" ON)
option(BUILD_EXAMPLES "Build examples" OFF)
option(MSGPACK_USE_BOOST "Use boost for msgpack" OFF)

set(MSGPACK_BUILD_DOCS OFF)

add_subdirectory(external/msgpack)

set(RPCLIB_INCLUDE_DIRS external/asio/asio/include external/msgpack/include include/)
target_include_directories(rpclib PUBLIC ${RPCLIB_INCLUDE_DIRS})

target_link_libraries(rpclib PUBLIC msgpack-cxx ssl crypto)

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
