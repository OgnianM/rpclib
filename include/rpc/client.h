#pragma once
#include "common.h"


namespace rpc {

template <typename socket_t> struct client : protected detail::rpc_base<socket_t> {
    static constexpr bool is_ssl = std::is_same_v<socket_t, rpc::types::ssl_socket_t>;

    using detail::rpc_base<socket_t>::rpc_base;

    client(asio::io_context &ctx, const std::string &hostname, uint16_t port)
    requires(!is_ssl) : detail::rpc_base<socket_t>(detail::rpc_try_connect(ctx, hostname, port)) {}

    client(asio::io_context &ctx, const std::string &hostname, uint16_t port, asio::ssl::context &ssl_ctx)
    requires(is_ssl) : detail::rpc_base<socket_t>(detail::rpc_try_connect(ctx, hostname, port, ssl_ctx)) {}

    /**
     * @brief Calls a function on the server
     * @tparam Ret return type
     * @tparam Args argument types
     * @param function_name name of the function to call
     * @param args arguments to pass to the function
     * @return std::future<Ret> that will be fulfilled when the server responds
     */
    template<typename Ret, typename... Args>
    std::future<Ret> async_call(const std::string& function_name, Args &&...args) noexcept {
        using namespace rpc::detail;
        static_assert(!is_non_const_lvalue_ref<Ret>(), "cannot call functions that return non-const lvalue references");

        asio::error_code ec;
        rpc_command command { function_name, pack_any(std::forward<const Args &&>(args)...) };

#ifdef RPC_ALLOW_LVALUE_REFS
        auto arg_tuple = std::make_shared<std::tuple<Args &&...>>(std::forward<Args &&>(args)...);
#endif

        auto to_send = pack_any(command);
        if (to_send.size() > COMMAND_BUFFER_SIZE) {
            std::promise<Ret> promise;
            promise.set_exception(std::make_exception_ptr(
                    std::runtime_error("rpc::client::async_call: command too large - " +
                    std::to_string(to_send.size()) + " > " + std::to_string(COMMAND_BUFFER_SIZE))));
            return promise.get_future();
        }

        ec = this->write(to_send.data(), to_send.size());
        ASIO_ERROR_GUARD(ec, {});
        ec = this->write_enqueued();
        ASIO_ERROR_GUARD(ec, {});

        auto promise = std::make_shared<std::promise<Ret>>();
        auto future = promise->get_future();

        this->async_read([this, arg_tuple = std::move(arg_tuple), promise = std::move(promise)]
                                 (const asio::error_code &ec, std::size_t size) {
            try {
                if (ec) throw std::runtime_error(ec.message());

                if (!this->remote_exception.empty()) {
                    auto e = std::move(this->remote_exception);
                    throw std::runtime_error("[SERVER] " + e);
                }

                rpc_result result;
                try {
                    result = unpack_single<rpc_result>(this->buffer.data(), size);
                } catch(std::exception& e) {
                    throw std::runtime_error("Failed to unpack rpc_result: " + std::string(e.what()));
                }
#ifdef RPC_ALLOW_LVALUE_REFS
                unpack_non_const_refs(*arg_tuple, result.lvalue_refs);
#endif
                try {
                    if constexpr (!std::is_same_v<Ret, void>) {
                        auto return_value = unpack_single<Ret>(result.return_value.c_str(), result.return_value.size());
                        this->read_enqueued();
                        promise->set_value(std::move(return_value));
                    } else {
                        this->read_enqueued();
                        promise->set_value();
                    }
                } catch(std::exception& e) {
                    throw std::runtime_error("Failed to unpack return value: " + std::string(e.what()));
                }
            } catch(std::exception& e) {
                try {
                    promise->set_exception(std::current_exception());
                } catch(std::exception& promise_ex) {
                    RPC_MSG(RPC_ERROR, "Failed to set exception on promise %s, (original exception %s)",
                            promise_ex.what(), e.what());
                }
            }
        });
        return future;
    }

    /// @return The functions bound on the remote host
    std::vector<std::string> get_functions() {
        return async_call<std::vector<std::string>>("get_bound_functions").get();
    }
};
}; // namespace rpc