#pragma once
#include <cstdio>
#include <iostream>
#include <tuple>
#include <asio/ssl.hpp>
#include <asio.hpp>
#include <msgpack.hpp>

using namespace asio::ip;

#ifndef RPC_MSG

inline void rpc_msg(FILE *type, const char *msg, ...) {
    if (type != nullptr) {
        va_list args;
        va_start(args, msg);
        vfprintf(type, msg, args);
        va_end(args);
    }
}
#define RPC_MSG(type, msg, ...) rpc_msg(type, "[" #type "] " msg "\n", ##__VA_ARGS__);
#define RPC_INFO stdout
#define RPC_DEBUG nullptr
#define RPC_WARNING stdout
#define RPC_ERROR stderr
#define ASIO_ERROR RPC_ERROR
#endif

#define ASIO_ERROR_GUARD(ec, ...)                                              \
  do {                                                                         \
    if (ec) {                                                                  \
      RPC_MSG(ASIO_ERROR, " File " __FILE__ "  Line: %i: %s",        \
               __LINE__, ec.message().c_str());                                 \
      return __VA_ARGS__;                                                      \
    }                                                                          \
  } while (0);

#define COMMAND_BUFFER_SIZE (1 << 20)


namespace rpc {
namespace types {
    typedef asio::ip::tcp::socket socket_t;
    typedef asio::ssl::stream<asio::ip::tcp::socket> ssl_socket_t;
}; // namespace types

namespace detail {
template<typename Sig> struct function_traits;

/// Base case
template<typename R, typename... Args>
struct function_traits<R(Args...)> {
    using decayed_parameter_tuple = std::tuple<std::decay_t<Args>...>;
    using parameter_tuple = std::tuple<Args...>;
    using return_type = std::remove_reference_t<R>;
};

template<typename R, typename... Args>
struct function_traits<R (*)(Args...)> : public function_traits<R(Args...)> { };

template<typename R, typename... Args>
struct function_traits<R (&)(Args...)> : public function_traits<R(Args...)> { };

/// @brief member function pointer
template<typename C, typename R, typename... Args>
struct function_traits<R (C::*)(Args...)> : public function_traits<R(Args...)> {
    using class_type = C;
};

/// @brief const member function pointer
template<typename C, typename R, typename... Args>
struct function_traits<R (C::*)(Args...) const> : public function_traits<R(Args...)> {
    using class_type = C;
};

/// @brief member object pointer
template<typename C, typename R>
struct function_traits<R(C::*)> : public function_traits<R()> {
    using class_type = C;
};

/// @brief Functor
template<typename Function>
struct function_traits : public function_traits<decltype(&std::remove_reference_t<Function>::operator())> {
};


template<typename T>
constexpr bool is_non_const_lvalue_ref() {
    return std::is_lvalue_reference_v<T> && !std::is_const_v<std::remove_reference_t<T>>;
}

template<typename tuple_t, int idx = 0>
constexpr bool has_non_const_lvalue_refs() {
    if constexpr (idx == std::tuple_size_v<tuple_t>) {
        return false;
    } else {
        if constexpr (is_non_const_lvalue_ref<std::tuple_element_t<idx, tuple_t>>()) {
            return true;
        } else {
            return has_non_const_lvalue_refs<tuple_t, idx + 1>();
        }
    }
}

/**
* @brief Packs any number of arguments into a string with msgpack
* @tparam Args argument types
* @param any arguments
* @return msgpacked string
*/
template<typename... Args>
std::string pack_any(Args &&...any) {
    std::stringstream ss;
    if constexpr (sizeof...(any) == 1) {
        msgpack::pack(ss, std::forward<const Args &&>(any)...);
    } else {
        msgpack::pack(ss, std::tie(std::forward<const Args &&>(any)...));
    }
    return ss.str();
}


template<typename tuple_t, int idx = 0>
void pack_non_const_refs(tuple_t &t, std::vector<std::string> &packed) {
    if constexpr (idx < std::tuple_size_v<tuple_t>) {
        if constexpr (is_non_const_lvalue_ref<std::tuple_element_t<idx, tuple_t>>()) {
            packed.emplace_back(pack_any(std::get<idx>(t)));
        } else packed.emplace_back();
        pack_non_const_refs<tuple_t, idx + 1>(t, packed);
    }
}


/**
* @brief Unpacks a msgpacked C string into a tuple
* @tparam Args argument types
* @param packed the packed string
* @param packed_size packed string size
* @param unpacked resultant tuple
*/
template<typename... Args>
void unpack_any(const char *packed, size_t packed_size,
                std::tuple<Args...> &unpacked) {
    auto tmp = msgpack::unpack(packed, packed_size);
    if constexpr (sizeof...(Args) == 1) {
        std::get<0>(unpacked) =
                tmp.get().as<std::decay_t<decltype(std::get<0>(unpacked))>>();
    } else {
        tmp.get().convert(unpacked);
    }
}

/**
* @brief Unpacks a msgpacked std::string into a tuple
* @tparam Args argument types
* @param packed the packed std::string
* @return unpacked resultant tuple
*/
template<typename... Args>
std::tuple<Args...> unpack_any(std::string const &packed) {
    std::tuple<Args...> result;
    unpack_any(packed, result);
    return result;
}

template<typename... Args>
void unpack_any(const std::string &packed, std::tuple<Args...> &unpacked) {
    unpack_any(packed.c_str(), packed.size(), unpacked);
}

template<typename... Args>
std::tuple<Args...> unpack_any(const char *packed, size_t packed_size) {
    std::tuple<Args...> result;
    unpack_any(packed, packed_size, result);
    return result;
}

template<typename T>
T unpack_single(const char *packed, size_t packed_size) {
    auto tmp = msgpack::unpack(packed, packed_size);
    T result;
    tmp.get().convert(result);
    return result;
}

template<typename tuple_t, int idx = 0>
void unpack_non_const_refs(tuple_t &t, std::vector<std::string> &packed) {
    if constexpr (idx < std::tuple_size_v<tuple_t>) {
        if constexpr (is_non_const_lvalue_ref<std::tuple_element_t<idx, tuple_t>>()) {
            if (idx < packed.size() && !packed[idx].empty()) {
                msgpack::unpack(packed[idx].c_str(), packed[idx].size()).get().convert(std::get<idx>(t));
            }
        }
        unpack_non_const_refs<tuple_t, idx + 1>(t, packed);
    }
}

/**
* @brief A generic representation of a remote function call with packed
* arguments
*/
struct rpc_command {
    /// The function name
    std::string function;
    /// The packed arguments
    std::string args;

    MSGPACK_DEFINE (function, args);
};

/**
* @brief A generic representation of a remote function call response
*/
struct rpc_result {
    /// The packed return value
    std::string return_value;
    /// The packed non constant reference arguments
#ifdef RPC_ALLOW_LVALUE_REFS
    std::vector<std::string> lvalue_refs;
    MSGPACK_DEFINE (return_value, lvalue_refs);
#else
    MSGPACK_DEFINE(return_value);
#endif

};
}; // namespace detail

/**
* @brief These functions are used to bypass msgpack, they write directly from
* and to user supplied buffers
* @note This is useful for large data transfers, where the overhead of msgpack
* is too high
* @details In the serialization function for your custom type, call
* enqueue_write with a buffer instead of feeding the data to msgpack
* @details In the deserialization function for your custom type, call
* enqueue_read with a buffer instead of reading the data from msgpack
* @note The buffer must persist until the RPC has returned
* @note All enqueued reads/writes will be completed before the RPC returns
* @note The actual logic for the enqueued reads/writes is in the
* rpc_base::read/write_enqueued functions
*/
namespace buffer {
/**
* @brief Enqueues a write on the current thread's write queue
* @param ptr pointer to the buffer
* @param count bytes to write
* @param deleter optional deleter for the buffer
*/
void enqueue_write(void *ptr, uint32_t count, std::function<void(void *)> &&deleter = [](void *) {});

/**
* @brief Enqueues a read on the current thread's read queue
* @param ptr pointer to the buffer
* @param count bytes to read
* @param callback optional callback to call when the read is complete
*/
void enqueue_read(void *ptr, uint32_t count, std::function<void()> &&callback = []{});
}; // namespace rpc_buffer

namespace detail {
/**
* @brief Tries to connect to a remote host
* @param hostname the host to connect to
* @param port the port to connect to
* @return If the connection was successful, a populated
* std::optional<socket_t>, otherwise an empty std::optional<socket_t>
*/
asio::ip::tcp::socket rpc_try_connect(asio::io_context &ctx, const std::string &hostname, uint16_t port);

/**
* @brief Tries to connect to a remote host
* @param hostname the host to connect to
* @param port the port to connect to
* @return If the connection was successful, a populated
* std::optional<socket_t>, otherwise an empty std::optional<socket_t>
*/
types::ssl_socket_t rpc_try_connect(asio::io_context &ctx, const std::string &hostname,
                                    uint16_t port, asio::ssl::context &ssl_ctx);

template<typename socket_t> requires(std::is_same_v<socket_t, types::socket_t> ||
                                     std::is_same_v<socket_t, types::ssl_socket_t>)
struct rpc_base {
    rpc_base(socket_t &&socket_) : socket(std::move(socket_)) {}
    rpc_base(const rpc_base &) = delete;
    rpc_base(rpc_base &&) = default;

protected:

    void async_read(std::function<void(const asio::error_code &ec, std::size_t size)> &&f);
    asio::error_code write(void *buffer, uint32_t buffer_size);


    std::array<char, COMMAND_BUFFER_SIZE> buffer;

    socket_t socket;
    uint32_t message_size;

    void destroy_socket();

    asio::error_code write_enqueued();
    asio::error_code read_enqueued();

    std::string remote_exception;

    void send_exception(const std::string &what) {
        uint32_t msize = -1;
        asio::write(socket, asio::buffer(&msize, sizeof(msize)), asio::transfer_all());
        msize = what.size();
        asio::write(socket, asio::buffer(&msize, sizeof(msize)), asio::transfer_all());
        asio::write(socket, asio::buffer(what.data(), msize), asio::transfer_all());
    }

private:
    bool reading_exception = false;

    void async_read_impl(std::function<void(const asio::error_code &ec, std::size_t size)> &&f) {
        async_read_bytecount([f = std::move(f), this] {
            if (message_size == uint32_t(-1)) { // -1 denotes an exception, read the exception
                if (reading_exception) {
                    RPC_MSG(RPC_ERROR, "Remote sent exception within exception, terminating connection.");
                    return;
                }
                reading_exception = true;
                async_read_impl([f = std::move(f), this](const asio::error_code &ec, std::size_t size) {
                    ASIO_ERROR_GUARD(ec);
                    remote_exception.assign(buffer.data(), message_size);
                    reading_exception = false;
                    f({}, 0);
                });
            } else if (message_size > COMMAND_BUFFER_SIZE) {
                send_exception("Requested buffer size is too long.");
                return;
            } else if (message_size > 0) {
                asio::async_read(socket, asio::buffer(buffer.data(), message_size),
                                 asio::transfer_exactly(message_size), f);
            } else {
                RPC_MSG(RPC_DEBUG, "Received empty message");
                f({}, 0);
            }
        });
    }

    asio::error_code write_impl(void *buffer, uint32_t buffer_size) {
        asio::error_code ec;
        asio::write(socket, asio::buffer(&buffer_size, sizeof(buffer_size)), ec);
        ASIO_ERROR_GUARD(ec, ec);
        if (buffer_size > 0) {
            asio::write(socket, asio::buffer(buffer, buffer_size), ec);
            ASIO_ERROR_GUARD(ec, ec);
        }
        return {};
    }

    /**
     * @brief The first function called in the async_read chain, gets the number of bytes to read
     * @param handler
     */
    void async_read_bytecount(std::function<void()> &&handler) {
        asio::async_read(socket, asio::mutable_buffer(&message_size, sizeof(message_size)),
                         [handler = std::move(handler)](const asio::error_code &ec, std::size_t bytes_transferred) {
                             // Check if stream is truncated
                             ASIO_ERROR_GUARD(ec);
                             handler();
                         });
    }
};
}; // namespace detail
}; // namespace rpc