cmake_minimum_required(VERSION 3.7)
project(rpclib)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "-fPIC")
add_library(rpclib STATIC include/rpc/node.hpp src/node.cpp)

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
    add_executable(basic_client_server example/basic_client_server.cpp)
    target_link_libraries(basic_client_server rpclib)
    target_include_directories(basic_client_server PUBLIC include)
endif()
