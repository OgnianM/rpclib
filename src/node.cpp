#include "rpc/node.h"

using namespace rpc;

thread_local std::vector<std::pair<asio::mutable_buffer, std::function<void()>>> enqueued_reads;
thread_local std::vector<std::pair<asio::const_buffer, std::function<void(void *)>>> enqueued_writes;


namespace rpc {
namespace buffer {

void enqueue_read(void *ptr, uint32_t count, std::function<void()> &&callback) {
    enqueued_reads.emplace_back(asio::mutable_buffer{ptr, count}, std::move(callback));
}

void enqueue_write(void *ptr, uint32_t count, std::function<void(void *)> &&deleter) {
    enqueued_writes.emplace_back(asio::const_buffer(ptr, count), std::move(deleter));
}


}; // namespace rpc_buffer
};

BasicSocket::BasicSocket(asio::ip::tcp::socket &socket) : socket(std::move(socket)) { }

void BasicSocket::write(const asio::const_buffer &buffer) {
    asio::write(this->socket, buffer);
}

void BasicSocket::read(const asio::mutable_buffer &buffer) {
    asio::read(this->socket, buffer, asio::transfer_exactly(buffer.size()));
}

void BasicSocket::async_write(const asio::const_buffer &buffer,
                              std::function<void(const asio::error_code &, std::size_t)> handler) {
    asio::async_write(this->socket, buffer, std::move(handler));
}

void BasicSocket::async_read(const asio::mutable_buffer &buffer,
                             std::function<void(const asio::error_code &, std::size_t)> handler) {
    asio::async_read(this->socket, buffer, asio::transfer_exactly(buffer.size()), std::move(handler));
}

void BasicSocket::close() {
    asio::error_code ec;
    this->socket.close(ec);
    ASIO_ERROR_GUARD(ec);
}

asio::ip::tcp::socket &BasicSocket::lowest_layer() {
    return this->socket;
}

SSLSocket::SSLSocket(asio::ssl::stream<asio::ip::tcp::socket> &socket) : socket(std::move(socket)) { }

void SSLSocket::write(const asio::const_buffer &buffer) {
    asio::write(this->socket, buffer);
}

void SSLSocket::read(const asio::mutable_buffer &buffer) {
    asio::read(this->socket, buffer, asio::transfer_exactly(buffer.size()));
}

void SSLSocket::async_write(const asio::const_buffer &buffer,
                            std::function<void(const asio::error_code &, std::size_t)> handler) {
    asio::async_write(this->socket, buffer, std::move(handler));
}

void SSLSocket::async_read(const asio::mutable_buffer &buffer,
                           std::function<void(const asio::error_code &, std::size_t)> handler) {
    asio::async_read(this->socket, buffer, asio::transfer_exactly(buffer.size()), std::move(handler));
}

void SSLSocket::close() {
    asio::error_code ec;
    this->socket.lowest_layer().close(ec);
    ASIO_ERROR_GUARD(ec);
}

asio::ip::tcp::socket &SSLSocket::lowest_layer() {
    return this->socket.next_layer();
}


node::node(rpc::ISocket *socket_) : socket(socket_) {
    buffer.resize(COMMAND_BUFFER_SIZE);
}

node::~node() { if(socket) delete socket; }

void node::write_enqueued() {
    for (auto &[buffer, deleter]: enqueued_writes) {
        socket->write(buffer);
        deleter((void *) buffer.data());
    }
    enqueued_writes.clear();
}

void node::read_enqueued() {
    for (auto &[buffer, callback]: enqueued_reads) {
        try {
            socket->read(buffer);
        } catch (...) {
            enqueued_reads.clear();
            std::rethrow_exception(std::current_exception());
        }
        callback();
    }
    enqueued_reads.clear();
}

void node::send_exception(const std::string &what) {
    std::lock_guard<std::mutex> lock(mutex);
    rpc_frame frame{.size = static_cast<uint32_t>(what.size()), .type = FrameType::EXCEPTION};
    socket->write(asio::buffer(&frame, sizeof(frame)));
    socket->write(asio::const_buffer(what.c_str(), what.size()));
}

void node::send_command(const std::string &function, const std::string &args, std::function<void()> result_handler) {
    std::lock_guard<std::mutex> lock(mutex);
    auto uid = next_uid++;
    rpc_command bincmd {.uid = uid, .function = function, .args = args};
    auto command = pack_any(bincmd);
    rpc_frame frame{.size = static_cast<uint32_t>(command.size()), .type = FrameType::COMMAND};
    pending_results[uid] = std::move(result_handler);
    socket->write(asio::buffer(&frame, sizeof(frame)));
    socket->write(asio::const_buffer(command.data(), command.size()));
    this->write_enqueued();
}

void node::send_result(const rpc::rpc_result &result) {
    std::lock_guard<std::mutex> lock(mutex);
    auto packed = pack_any(result);
    rpc_frame frame{.size = static_cast<uint32_t>(packed.size()), .type = FrameType::RESULT};
    socket->write(asio::buffer(&frame, sizeof(frame)));
    socket->write(asio::const_buffer(packed.data(), packed.size()));
    this->write_enqueued();
}

void node::process_frame() {
    std::lock_guard<std::mutex> lock(mutex);
    switch (frame.type) {
        case FrameType::COMMAND: {
            auto command = unpack_single<rpc_command>(buffer.data(), frame.size);
            auto it = functions.find(command.function);
            if (it != functions.end()) {
                try {
                    (*it->second)(command.uid, command.args);
                } catch (std::exception &e) {
                    send_exception(e.what());
                }
            } else {
                send_exception("Function not found.");
            }
            break;
        }
        case FrameType::RESULT: {
            auto result = unpack_single<rpc_result>(buffer.data(), frame.size);

            auto it = pending_results.find(result.uid);
            if (it != pending_results.end()) {
                it->second();
                pending_results.erase(it);
            } else {
                send_exception("No pending result found.");
            }

            break;
        }
        case FrameType::EXCEPTION: {
            throw std::runtime_error("[Peer exception] " + std::string(buffer.data(), frame.size));
        }

        default:
            send_exception("Unknown frame type.");
    }
}

/**
 * @brief The first function called in the async_read chain, gets the number of bytes to read and the type of transfer
 * @param handler
 */
void node::async_read_frame(handle self) {
    socket->async_read(asio::mutable_buffer(&frame, sizeof(frame)), [this, self = std::move(self)](
            const asio::error_code &ec, std::size_t size) {
        ASIO_ERROR_GUARD(ec);

        if (frame.size >= COMMAND_BUFFER_SIZE) {
            send_exception("Requested buffer size is too long.");
            async_read_frame(self);
            return;
        }

        if (frame.size > 0) {
            socket->async_read(asio::mutable_buffer(buffer.data(), frame.size), [this, self = std::move(self)](
                    const asio::error_code &ec, std::size_t size) {
                ASIO_ERROR_GUARD(ec);
                process_frame();
                async_read_frame(self);
            });
        } else {
            async_read_frame(self);
            return;
        }
    });
}

