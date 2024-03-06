#pragma once
#include <cstdio>
#include <iostream>
#include <tuple>
#include <asio/ssl.hpp>
#include <asio.hpp>
#include <msgpack.hpp>

#define ASIO_ERROR_GUARD(ec) do { auto error = ec; if (error) throw error; } while (0);

namespace rpc {

constexpr size_t COMMAND_BUFFER_SIZE = (1 << 20);

namespace types {
    typedef asio::ip::tcp::socket socket_t;
    typedef asio::ssl::stream<asio::ip::tcp::socket> ssl_socket_t;
}; // namespace types

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
constexpr bool is_lvalue_ref() {
    return std::is_lvalue_reference_v<T> && !std::is_const_v<std::remove_reference_t<T>>;
}

template<typename tuple_t, int idx = 0>
constexpr bool has_lvalue_refs() {
    if constexpr (idx == std::tuple_size_v<tuple_t>) {
        return false;
    } else {
        if constexpr (is_lvalue_ref<std::tuple_element_t<idx, tuple_t>>()) {
            return true;
        } else {
            return has_lvalue_refs<tuple_t, idx + 1>();
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
void pack_lvalue_refs(tuple_t &t, std::vector<std::string> &packed) {
    if constexpr (idx < std::tuple_size_v<tuple_t>) {
        if constexpr (is_lvalue_ref<std::tuple_element_t<idx, tuple_t>>()) {
            packed.emplace_back(pack_any(std::get<idx>(t)));
        } else packed.emplace_back();
        pack_lvalue_refs<tuple_t, idx + 1>(t, packed);
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
void unpack_any(const char *packed, size_t packed_size, std::tuple<Args...> &unpacked) {
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
void unpack_lvalue_refs(tuple_t &t, std::vector<std::string> &packed) {
    if constexpr (idx < std::tuple_size_v<tuple_t>) {
        if constexpr (is_lvalue_ref<std::tuple_element_t<idx, tuple_t>>()) {
            if (idx < packed.size() && !packed[idx].empty()) {
                msgpack::unpack(packed[idx].c_str(), packed[idx].size()).get().convert(std::get<idx>(t));
            }
        }
        unpack_lvalue_refs<tuple_t, idx + 1>(t, packed);
    }
}

/**
* @brief A generic representation of a remote function call with packed
* arguments
*/
struct rpc_command {
    uint32_t uid;
    /// The function name
    std::string function;
    /// The packed arguments
    std::string args;

    MSGPACK_DEFINE (uid, function, args);
};

/**
* @brief A generic representation of a remote function call response
*/
struct rpc_result {
    uint32_t uid;
    /// The packed return value
    std::string return_value;
    /// The packed non-constant reference arguments
    std::vector<std::string> lvalue_refs;
    MSGPACK_DEFINE (uid, return_value, lvalue_refs);
};

/**
* @brief These functions are used to bypass msgpack, they write directly from
* and to user supplied buffers
* @note This is useful for large data transfers, where the overhead of msgpack
* is too high
* @details In the de/serialization function for your custom type, call
* enqueue_read/write with a buffer instead of feeding the data to msgpack
* @note The buffer must persist until the RPC has returned
* @note All enqueued reads/writes will be completed before the RPC returns
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


struct ISocket {
    virtual ~ISocket() = default;
    virtual void write(const asio::const_buffer& buffer) = 0;
    virtual void read(const asio::mutable_buffer& buffer) = 0;

    virtual void async_write(const asio::const_buffer& buffer, std::function<void(const asio::error_code&, std::size_t)> handler) = 0;
    virtual void async_read(const asio::mutable_buffer& buffer, std::function<void(const asio::error_code&, std::size_t)> handler) = 0;

    virtual void close() = 0;

    virtual asio::ip::tcp::socket& lowest_layer() = 0;
};

struct BasicSocket : ISocket {
    BasicSocket(asio::ip::tcp::socket& socket);
    void write(const asio::const_buffer& buffer) override;
    void read(const asio::mutable_buffer& buffer) override;

    void async_write(const asio::const_buffer& buffer, std::function<void(const asio::error_code&, std::size_t)> handler) override;
    void async_read(const asio::mutable_buffer& buffer, std::function<void(const asio::error_code&, std::size_t)> handler) override;

    void close() override;

    asio::ip::tcp::socket& lowest_layer() override;

    asio::ip::tcp::socket socket;
};

struct SSLSocket : ISocket {
    SSLSocket(asio::ssl::stream<asio::ip::tcp::socket>& socket);
    void write(const asio::const_buffer& buffer) override;
    void read(const asio::mutable_buffer& buffer) override;

    void async_write(const asio::const_buffer& buffer, std::function<void(const asio::error_code&, std::size_t)> handler) override;
    void async_read(const asio::mutable_buffer& buffer, std::function<void(const asio::error_code&, std::size_t)> handler) override;

    void close() override;

    asio::ip::tcp::socket& lowest_layer() override;

    asio::ssl::stream<asio::ip::tcp::socket> socket;
};


enum class FrameType : uint32_t {
    COMMAND,
    RESULT,
    EXCEPTION
};

struct rpc_frame {
    uint32_t size;
    FrameType type;
};

struct node {
    using handle = std::shared_ptr<node>;

    template<typename T>
    static std::shared_ptr<T> connect(asio::any_io_executor ctx,
                                      const asio::ip::tcp::endpoint &ep,
                                      std::optional<asio::ssl::context> ssl_ctx = std::nullopt) {
        asio::ip::tcp::socket sock(ctx);
        sock.connect(ep);

        std::shared_ptr<T> result;
        if (ssl_ctx) {
            asio::ssl::stream<asio::ip::tcp::socket> ssl_sock(std::move(sock), *ssl_ctx);
            ssl_sock.handshake(asio::ssl::stream_base::handshake_type::client);
            result = std::make_shared<T>(new SSLSocket(ssl_sock));
        } else {
            result = std::make_shared<T>(new BasicSocket(sock));
        }

        // Clients are externally managed, thus they do not contain a shared_ptr to themselves in their read loop
        result->async_read_frame(nullptr);
        return result;
    }

    template<typename T> requires(std::is_base_of_v<node, T>)
    static std::shared_ptr<T> create(ISocket* socket) {
        auto result = std::make_shared<T>(socket);
        // Servers are internally managed, thus they contain a shared_ptr to themselves in their read loop
        result->async_read_frame(result);
        return result;
    }

    node(ISocket* socket_);
    node(const node &) = delete;
    node(node &&) = default;
    ~node();

    void write_enqueued();
    void read_enqueued();

    void send_exception(const std::string &what);
    void send_command(const std::string& function, const std::string& args, std::function<void()> result_handler = []{});
    void send_result(const rpc_result& result);
    void process_frame();

    /**
     * @brief The first function called in the async_read chain, gets the number of bytes to read and the type of transfer
     * @param handler
     */
    void async_read_frame(handle self);

    /**
     * @brief Bind a function to the server
     * @tparam F the function type to be called on remote invocation
     * @param name the name with which the function will be registered, this is
     * used to identify the function on the remote side
     * @param f Function (or functor) pointer
     * @param this_ptr_ If the function is a member function, this is the pointer
     * to the object instance
     */
    template<typename F>
    void bind(const std::string &name, F &&f, void *this_ptr_ = nullptr) {
        using traits = function_traits<F>;
        if constexpr (std::is_member_function_pointer_v<F>) {
            if (this_ptr_ == nullptr) {
                throw std::runtime_error("this_ptr cannot be nullptr for member functions");
            }
        }
        static_assert(!is_lvalue_ref<typename traits::return_type>(),
                      "cannot bind functions that return non-const lvalue references");

        functions[name] = std::make_shared<std::function<void(uint32_t uid, std::string &)>>(
        [=, this](uint32_t uid, std::string &packed_args) -> void {
            auto self = functions[name];
            rpc_result res { .uid = uid };

            // This acts like a caller stack, only decayed types
            typename traits::decayed_parameter_tuple decayed_params;
            try {
                unpack_any(packed_args, decayed_params);
            } catch (std::exception& e) {
                throw std::runtime_error("Error unpacking arguments " + packed_args + ": " + e.what());
            }
            this->read_enqueued();

            // This acts like the callee stack, elements may be references
            auto params = std::apply([]<typename... Ts>(Ts &&...args) ->
                                             typename traits::parameter_tuple { return {args...}; }, decayed_params);

            auto function_call = [this_ptr_, f]<typename... Ts>(Ts &&...args) {
                if constexpr (std::is_member_function_pointer_v<F>) {
                    return (((typename traits::class_type *) this_ptr_)->*f)(std::forward<Ts &&>(args)...);
                } else {
                    return f(std::forward<Ts &&>(args)...);
                }
            };

            if constexpr (std::is_same_v<typename traits::return_type, void>) {
                std::apply(function_call, params);
                pack_lvalue_refs(params, res.lvalue_refs);
                send_result(res);
            } else {
                // Keep this alive until write_enqueued has finished, it may contain RPC buffers
                auto invocation_result = std::apply(function_call, params);
                res.return_value = pack_any(invocation_result);
                pack_lvalue_refs(params, res.lvalue_refs);
                send_result(res);
            }
        });
    }

    /**
     * @brief Calls a function on the server
     * @tparam Ret return type
     * @tparam Args argument types
     * @param function_name name of the function to call
     * @param args arguments to pass to the function
     * @return std::future<Ret> that will be fulfilled when the server responds
     */
    template<typename Ret, typename... Args>
    std::future<Ret> async_call(const std::string& function_name, Args &&...args) {
        static_assert(!is_lvalue_ref<Ret>(), "cannot call functions that return non-const lvalue references");

        auto arg_tuple = std::make_shared<std::tuple<Args &&...>>(std::forward<Args &&>(args)...);
        auto promise = std::make_shared<std::promise<Ret>>();
        auto future = promise->get_future();

        send_command(function_name, pack_any(std::forward<const Args &&>(args)...),
                     [this, arg_tuple = std::move(arg_tuple), promise = std::move(promise)]
        {
            try {
                rpc_result result = unpack_single<rpc_result>(this->buffer.data(), frame.size);
                unpack_lvalue_refs(*arg_tuple, result.lvalue_refs);
                if constexpr (!std::is_same_v<Ret, void>) {
                    auto return_value = unpack_single<Ret>(result.return_value.c_str(), result.return_value.size());
                    this->read_enqueued();
                    promise->set_value(std::move(return_value));
                } else {
                    this->read_enqueued();
                    promise->set_value();
                }
            }
            catch(std::exception& e) { promise->set_exception(std::current_exception()); }
        });
        return future;
    }


    std::unordered_map<uint32_t, std::function<void()>> pending_results;
    std::unordered_map<std::string, std::shared_ptr<std::function<void(uint32_t, std::string&)>>> functions;
    std::vector<char> buffer;
    ISocket* socket;
    rpc_frame frame;
    std::mutex mutex;
    uint32_t next_uid = 0;
};



namespace detail {
    template<typename socket_t> struct declare_ssl_context {};

    template<>
    struct declare_ssl_context<types::ssl_socket_t> {
        declare_ssl_context(asio::ssl::context &ctx) : ssl_context(std::move(ctx)) {}
        asio::ssl::context ssl_context;
    };
};

/**
 * @brief An abstraction over a basic acceptor, creates services for incoming connections
 * @tparam socket_t type of socket to use (either types::socket_t or types::ssl_socket_t)
 * @tparam EntrypointService_ A class derived from rpc::server, which will be used to handle incoming connections.
 * @note The service provider does not keep track of the
 * EntrypointService_ objects it creates, they are considered self-managed, or
 * rather, owned by the remote client.
 */
template <typename socket_t, typename EntrypointService> requires(std::is_base_of_v<node, EntrypointService>)
struct service_provider : detail::declare_ssl_context<socket_t> {
    static constexpr bool is_ssl = std::is_same_v<socket_t, rpc::types::ssl_socket_t>;

    /**
     * @brief Non-SSL constructor
     * @param ep The endpoint to bind to
     */
    explicit service_provider(const asio::ip::tcp::endpoint &ep) requires(!is_ssl) : acceptor(ctx, ep) {}

    /**
     * @brief SSL constructor
     * @param ep The endpoint to bind to
     * @param ssl_ctx SSL context
     */
    service_provider(const asio::ip::tcp::endpoint &ep, asio::ssl::context &ssl_ctx) requires(is_ssl)
            : detail::declare_ssl_context<socket_t>(ssl_ctx), acceptor(ctx, ep) {}

    ~service_provider() {
        ctx.stop();
        for (auto &t : io_threads) {
            t.join();
        }
    }

    void start(size_t threads = 1) {
        accept();

        io_threads.reserve(threads);
        for (int i = 0; i < threads; i++) {
            io_threads.template emplace_back([this] {
                while (true) {
                    try {
                        ctx.run();
                        break;
                    } catch (asio::error_code& e) {
                    }
                }
            });
        }
    }
    [[nodiscard]] asio::io_context &get_context() { return ctx; }
    [[nodiscard]] asio::ssl::context &get_ssl_context() requires(is_ssl) { return this->ssl_context; }

    std::function<void(std::shared_ptr<node> &)> on_service_created_callback;
private:
    void accept() {
        acceptor.async_accept([this](asio::error_code ec, asio::ip::tcp::socket peer) {
            ASIO_ERROR_GUARD(ec);

            // We can enqueue another async_accept, before processing the new connection.
            accept();

            std::shared_ptr<node> svc;
            if constexpr (is_ssl) {
                // SSL handshake
                rpc::types::ssl_socket_t sock(std::move(peer), this->ssl_context);
                sock.handshake(asio::ssl::stream_base::handshake_type::server, ec);
                ASIO_ERROR_GUARD(ec);

                svc = EntrypointService::template create<EntrypointService>((ISocket*)new SSLSocket(sock));
            } else {
                svc = EntrypointService::template create<EntrypointService>((ISocket*)new BasicSocket(peer));
            }

            if (on_service_created_callback) {
                on_service_created_callback(svc);
            }
        });
    }

    asio::io_context ctx;
    asio::ip::tcp::acceptor acceptor;
    std::vector<std::thread> io_threads;
};
}; // namespace rpc