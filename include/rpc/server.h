#pragma once
#include "common.h"

namespace rpc {
template <typename socket_t>
struct server : protected detail::rpc_base<socket_t>,
                public std::enable_shared_from_this<server<socket_t>> {
    /**
     * @brief Bind a function to the server
     * @tparam F the function type to be called on remote invocation
     * @param name the name with which the function will be registered, this is
     * used to identify the function on the remote side
     * @param f Function (or functor) pointer
     * @param this_ptr_ If the function is a member function, this is the pointer
     * to the object instance
     * @note if this_ptr_ is a nullptr, rpc::server::this_ptr will be used for
     * member functions, which can be set with rpc_server::set_this_ptr
     */
    template<typename F>
    void bind(const std::string &name, F &&f, void *this_ptr_ = nullptr) {
        using namespace detail;

        if (this_ptr_ == nullptr) {
            this_ptr_ = this_ptr;
        }

        using traits = function_traits<F>;

#ifndef RPC_ALLOW_LVALUE_REFS
        static_assert(!has_non_const_lvalue_refs<typename traits::parameter_tuple>(),
            "Binding functions with non-const lvalue reference arguments is disabled "
            "you can enable it by defining RPC_ALLOW_LVALUE_REFS");
#endif

        static_assert(!is_non_const_lvalue_ref<typename traits::return_type>(),
                      "cannot bind functions that return non-const lvalue references");

        functions[name] = std::make_shared<std::function<void(std::string const &)>>(
        [=, this](const std::string &packed_args) -> void {

            // The bound function may change the contents of `functions`
            // It may even deallocate itself, so keep a shared_ptr copy
            // inside the function scope
            auto self = functions[name];

            constexpr bool is_member_function = std::is_member_function_pointer_v<F>;

            // This acts like a caller stack, only decayed types
            typename traits::decayed_parameter_tuple decayed_params;
            unpack_any(packed_args, decayed_params);
            this->read_enqueued();

            // This acts like the callee's stack, elements may be references
            auto params = std::apply([]<typename... Ts>(Ts &&...args) ->
                    typename traits::parameter_tuple { return {args...}; }, decayed_params);

            rpc_result res;

            if constexpr (std::is_same_v<typename traits::return_type, void>) {
                if constexpr (is_member_function) {
                    std::apply([this_ptr_, f]<typename... Ts>(Ts &&...args) {
                        (((typename traits::class_type *) this_ptr_)->*f)(std::forward<Ts &&>(args)...);
                    }, params);
                } else {
                    std::apply(f, params);
                }
#ifdef RPC_ALLOW_LVALUE_REFS
                pack_non_const_refs(params, res.lvalue_refs);
#endif
                auto result = pack_any(res);
                this->write(result.data(), result.size());
                this->write_enqueued();
            } else {
                // Keep this alive until write_enqueued has finished, it may contain RPC buffers
                auto invokation_result = [&]() {
                    if constexpr (is_member_function) {
                        return std::apply([this_ptr_, f]<typename... Ts>(Ts &&...args) {
                            return (((typename traits::class_type *) this_ptr_)->*f)(std::forward<Ts &&>(args)...);
                        }, params);
                    } else {
                        return std::apply(f, params);
                    }
                }();

                res.return_value = pack_any(invokation_result);
#ifdef RPC_ALLOW_LVALUE_REFS
                pack_non_const_refs(params, res.lvalue_refs);
#endif
                auto result = pack_any(res);
                this->write(result.data(), result.size());
                this->write_enqueued();
            }

        });
    }

    /**
     * @param callback The function to call if the client calls an unknown function
     * @note if this function is not provided, the server will throw an exception back to the client
     */
    void set_unknown_function_called_callback(
            const std::function<void(const std::string &funcs,const std::string &args)> &callback) {
        unknown_function_called = callback;
    }


    /**
     * @brief Set the this pointer for member functions, can be overridden in bind
     * by setting this_ptr_ to non-nullptr value
     * @param ptr pointer to the object instance
     */
    void set_this_ptr(void *ptr) { this_ptr = ptr; }

    /**
     * @brief Clears bound functions
     * @note adds get_bound_functions and get_command_buffer_size to the list of bound functions
     */
    void clear_bound() {
        functions.clear();
        bind("get_bound_functions", &server::get_bound_functions, this);
        bind("get_command_buffer_size", &server::get_command_buffer_size, this);
    }

    void erase_bound(const std::string &name) { functions.erase(name); }

    /// @brief Construct a new rpc_server object from a socket
    template<typename T>
    static inline std::shared_ptr<T> create(socket_t &&sock) {
        auto result = std::shared_ptr<T>(new T(std::forward<socket_t &&>(sock)));
        result->listen_for_commands(result);
        return result;
    }

    void destroy_server() { this->destroy_socket(); }

    /// @return A vector of all functions accessible to the remote rpc_client
    std::vector<std::string> get_bound_functions() const {
        std::vector<std::string> result;
        result.reserve(functions.size());

        for (auto &f: functions) {
            result.push_back(f.first);
        }

        return result;
    }

    uint32_t get_command_buffer_size() const { return COMMAND_BUFFER_SIZE; }

protected:
    server(socket_t &&socket_) : detail::rpc_base<socket_t>(std::move(socket_)) { clear_bound(); }

    void *this_ptr = this;
    std::unordered_map<std::string, std::shared_ptr<std::function<void(const std::string &)>>> functions;
    std::function<void(const std::string &, const std::string &)> unknown_function_called;

    /**
     * @brief Listens for commands from the remote rpc_client
     * @param this_shared A shared_ptr to this object, at least one such ptr will
     * be held until the listen loop terminates
     */
    void listen_for_commands(std::shared_ptr<server<socket_t>> this_shared) {
        this->async_read([this, this_shared = std::move(this_shared)](
                const asio::error_code &ec, std::size_t bytes_transferred) {
            ASIO_ERROR_GUARD(ec);

            if (this->remote_exception) {
                RPC_MSG(RPC_DEBUG, "Client sent an exception %s, closing connection.", this->remote_exception->c_str());
                return;
            }

            try {
                auto command = detail::unpack_single<detail::rpc_command>(this->buffer.data(), bytes_transferred);
                auto fn_ = functions.find(command.function);

                if (fn_ != functions.end()) {
                    (*fn_->second)(command.args);
                } else {
                    if (unknown_function_called) {
                        unknown_function_called(command.function, command.args);
                    } else {
                        throw std::runtime_error("Unknown function called");
                    }
                }
            } catch (std::exception &e) {
                this->send_exception(e.what());
            }
            listen_for_commands(this_shared);
        });
    }
};

}; // namespace rpc