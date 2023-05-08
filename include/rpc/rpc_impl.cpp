#include "common.h"

namespace rpc {

using namespace detail;

namespace buffer {
thread_local std::vector<asio::mutable_buffer> enqueued_reads;
thread_local std::vector<std::pair<asio::const_buffer, std::function<void(void *)>>> enqueued_writes;

void enqueue_read(void *ptr, uint32_t count) {
    enqueued_reads.emplace_back(ptr, count);
}

void enqueue_write(void *ptr, uint32_t count, std::function<void(void *)> &&deleter) {
    enqueued_writes.emplace_back(asio::const_buffer(ptr, count), std::move(deleter));
}

template<typename socket_t>
asio::error_code write_enqueued(socket_t &socket) {
    asio::error_code ec;

    for (auto &[buffer, deleter]: enqueued_writes) {
        RPC_MSG(RPC_DEBUG, "Writing buffer %ull of size %u", buffer.data(), buffer.size());
        asio::write(socket, buffer, ec);
        deleter((void *) buffer.data());
    }
    enqueued_writes.clear();

    ASIO_ERROR_GUARD(ec, ec);
    return ec;
}

template<typename socket_t>
asio::error_code read_enqueued(socket_t &socket) {
    asio::error_code ec;
    for (auto &buffer: enqueued_reads) {
        RPC_MSG(RPC_DEBUG, "Reading into buffer %ull of size %u", buffer.data(),
                buffer.size());

        asio::read(socket, buffer, asio::transfer_exactly(buffer.size()), ec);
        if (ec) {
            enqueued_reads.clear();
            ASIO_ERROR_GUARD(ec, ec);
        }
    }
    enqueued_reads.clear();
    return ec;
}
}; // namespace rpc_buffer

// region Utils
namespace detail {
asio::ip::tcp::socket rpc_try_connect(asio::io_context &ctx, const std::string &hostname, uint16_t port) {
    asio::ip::tcp::resolver resolver(ctx);
    asio::error_code ec;
    auto ep = resolver.resolve(hostname, std::to_string(port), ec);

    if (ec) {
        throw std::runtime_error("Failed to resolve hostname: " + ec.message());
    }

    asio::ip::tcp::socket sock(ctx);
    asio::connect(sock, ep, ec);

    if (ec) {
        throw std::runtime_error("Failed to connect to " + hostname + ":" + std::to_string(port) + ": " + ec.message());
    }

    return sock;
}

types::ssl_socket_t rpc_try_connect(asio::io_context &ctx, const std::string &hostname,
                                                   uint16_t port, asio::ssl::context &ssl_ctx) {
    auto sock_ = rpc_try_connect(ctx, hostname, port);

    asio::error_code ec;
    types::ssl_socket_t ssl_sock(std::move(sock_), ssl_ctx);
    ssl_sock.handshake(asio::ssl::stream_base::handshake_type::client, ec);

    if (ec) {
        throw std::runtime_error("Failed to handshake with " + hostname + ":" +
        std::to_string(port) + ": " + ec.message());
    }

    return ssl_sock;
}
};
// endregion

// region Non-SSL definitions
template <> asio::error_code rpc_base<types::socket_t>::write_enqueued() { return buffer::write_enqueued(socket); }
template <> asio::error_code rpc_base<types::socket_t>::read_enqueued() { return buffer::read_enqueued(socket); }
template <> void rpc_base<types::socket_t>::destroy_socket() { this->socket.close(); }
// endregion

// region SSL definitions
template <> asio::error_code rpc_base<types::ssl_socket_t>::write_enqueued() { return buffer::write_enqueued(socket); }
template <> asio::error_code rpc_base<types::ssl_socket_t>::read_enqueued() { return buffer::read_enqueued(socket); }
template <> void rpc_base<types::ssl_socket_t>::destroy_socket() { this->socket.shutdown(); }
// endregion

}; // namespace rpc
