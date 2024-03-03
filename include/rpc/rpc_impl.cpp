#include "common.h"

namespace rpc {

using namespace detail;

namespace buffer {
thread_local std::vector<std::pair<asio::mutable_buffer, std::function<void()>>> enqueued_reads;
thread_local std::vector<std::pair<asio::const_buffer, std::function<void(void *)>>> enqueued_writes;

void enqueue_read(void *ptr, uint32_t count, std::function<void()> &&callback) {
    enqueued_reads.emplace_back(asio::mutable_buffer{ptr, count}, std::move(callback));
}

void enqueue_write(void *ptr, uint32_t count, std::function<void(void *)> &&deleter) {
    enqueued_writes.emplace_back(asio::const_buffer(ptr, count), std::move(deleter));
}

template<typename socket_t>
void write_enqueued(socket_t &socket) {
    asio::error_code ec;

    for (auto &[buffer, deleter]: enqueued_writes) {
        RPC_MSG(RPC_DEBUG, "Writing buffer %ull of size %u", buffer.data(), buffer.size());
        asio::write(socket, buffer, ec);
        ASIO_ERROR_GUARD(ec);
        deleter((void *) buffer.data());
    }
    enqueued_writes.clear();
}

template<typename socket_t>
void read_enqueued(socket_t &socket) {
    asio::error_code ec;
    for (auto &[buffer, callback]: enqueued_reads) {
        RPC_MSG(RPC_DEBUG, "Reading into buffer %ull of size %u", buffer.data(),
                buffer.size());

        asio::read(socket, buffer, asio::transfer_exactly(buffer.size()), ec);
        if (ec) {
            enqueued_reads.clear();
            ASIO_ERROR_GUARD(ec);
        }
        callback();
    }
    enqueued_reads.clear();
}
}; // namespace rpc_buffer

// region Non-SSL definitions
template <> void rpc_base<types::socket_t>::write_enqueued() { return buffer::write_enqueued(socket); }
template <> void rpc_base<types::socket_t>::read_enqueued() { return buffer::read_enqueued(socket); }
template <> void rpc_base<types::socket_t>::destroy_socket() { this->socket.close(); }
template <> void rpc_base<types::socket_t>::write(void *buffer, uint32_t buffer_size) {
    write_impl(buffer, buffer_size);
}

template<> void rpc_base<types::socket_t>::async_read(std::function<void(const asio::error_code &ec, std::size_t size)> &&f) {
    async_read_impl(std::move(f));
}

// endregion

// region SSL definitions
template <> void rpc_base<types::ssl_socket_t>::write_enqueued() { return buffer::write_enqueued(socket); }
template <> void rpc_base<types::ssl_socket_t>::read_enqueued() { return buffer::read_enqueued(socket); }
template <> void rpc_base<types::ssl_socket_t>::destroy_socket() { this->socket.shutdown(); }
template <> void rpc_base<types::ssl_socket_t>::write(void *buffer, uint32_t buffer_size) {
    write_impl(buffer, buffer_size);
}

template<> void rpc_base<types::ssl_socket_t>::async_read(std::function<void(const asio::error_code &ec, std::size_t size)> &&f) {
    async_read_impl(std::move(f));
}
// endregion
}; // namespace rpc
