#pragma once
#include "server.h"
#include <thread>

namespace rpc {

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
template <typename socket_t, template <typename> typename EntrypointService_>
requires((std::is_same_v<socket_t, rpc::types::socket_t> || std::is_same_v<socket_t, rpc::types::ssl_socket_t>) &&
          std::is_base_of_v<rpc::server<socket_t>, EntrypointService_<socket_t>>)
struct service_provider : detail::declare_ssl_context<socket_t> {
    using EntrypointService = EntrypointService_<socket_t>;
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

    ~service_provider() { ctx.stop(); }


    void start(size_t threads = 2) {
        RPC_MSG(RPC_INFO, "Starting server on port %u with %u thread(s)", acceptor.local_endpoint().port(), threads);
        accept();

        io_threads.reserve(threads);
        for (int i = 0; i < threads; i++) {
            io_threads.template emplace_back([this] { ctx.run(); });
        }
    }

    void on_service_created(std::function<void(std::shared_ptr<EntrypointService> &)> callback) {
        on_service_created_callback = callback;
    }

    [[nodiscard]] asio::io_context &get_context() { return ctx; }
    [[nodiscard]] asio::ssl::context &get_ssl_context() requires(is_ssl) { return this->ssl_context; }


private:
    void accept() {
        acceptor.async_accept([this](asio::error_code ec, asio::ip::tcp::socket peer) {
            ASIO_ERROR_GUARD(ec);

            // We can enqueue another async_accept, before processing the new connection.
            accept();

            std::shared_ptr<EntrypointService> svc;
            if constexpr (is_ssl) {
                // SSL handshake
                rpc::types::ssl_socket_t sock(std::move(peer), this->ssl_context);
                sock.handshake(asio::ssl::stream_base::handshake_type::server, ec);
                ASIO_ERROR_GUARD(ec);

                svc = EntrypointService::template create<EntrypointService>(std::move(sock));
            } else {
                svc = EntrypointService::template create<EntrypointService>(std::move(peer));
            }

            if (on_service_created_callback) {
                on_service_created_callback(svc);
            }
        });
    }

    std::function<void(std::shared_ptr<EntrypointService> &)> on_service_created_callback;
    asio::io_context ctx;
    asio::ip::tcp::acceptor acceptor;
    std::vector<std::jthread> io_threads;
};

}; // namespace rpc