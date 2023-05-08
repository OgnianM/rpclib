<h1> Small and lightweight Remote Procedure Call library for C++20</h1>



<h2> Server </h2>
The library is built around the idea of the service and the service provider, the service 
provider accepts incoming connections and creates services to handle the client requests.
A service is a class that inherits from the rpc::server class and implements custom behavior.

Example:
```cpp
template<typename socket_t>
struct hello_world_service : rpc::server {
    hello_world_service(socket_t&& socket) : rpc::server(socket) {
        this->bind("hello_world", &hello_world_service::hello_world);
    }
    
    std::string hello_world() {
        return "Hello World!";
    }
};
```
<h2> Binding different kinds of functions </h2>


```cpp
...
// lambdas/functors
this->bind("my_lambda", [...](int& x) { x = 42; });    
// global functions
this->bind("my_global_func", my_global_func);
// member functions
this->bind("my_member_func", &hello_world_service::my_member_func);
// other member functions
this->bind("other_member_func", &other_class::other_member_func, other_class_instance_ptr);
```

If an instance pointer is not specified for a member function, `rpc::server::this_ptr` will be used, 
it can be changed by calling `rpc::server::set_this_ptr(new_ptr)`, the default value is `this`.



<h2> rpc::service_provider </h2>

The `rpc::service_provider` can then expose this service to the network like so
```cpp
auto endpoint = asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 1234);
rpc::service_provider<rpc::types::socket_t, hello_world_service> provider(endpoint, thread_count);
provicer.start();
```
Optionally with SSL
```cpp
auto endpoint = asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 1234);
asio::ssl::context ssl_ctx(asio::ssl::context::tlsv12);
// Initialize your context here ...
rpc::service_provider<rpc::types::ssl_socket_t, hello_world_service> provider(endpoint, ssl_ctx, thread_count);
provider.start();
```

Once a client connects to the service_provider, a new hello_world_service will be created and bound to that client. <br>
Note that commands to a single service are synchronous, while commands to different services are asynchronous.
<h2> Client </h2>

```cpp
asio::io_context io_ctx;
// ...
rpc::client<rpc::types::socket_t> client(io_ctx, "localhost", 1234);
```

Optionally with SSL
```cpp
asio::io_context io_ctx;
// ...
asio::ssl::context ssl_ctx(asio::ssl::context::tlsv12);
// Initialize your context here ...
rpc::client<rpc::types::ssl_socket_t> client(io_ctx, "localhost", 1234, ssl_ctx);
```

```cpp
std::future<std::string> result = client.async_call<std::string>("hello_world");
std::cout << result.get() << '\n';
```
The construction of the client may throw if it fails to connect. <br>
`async_call` will never throw, any exceptions will be stored in the future object. <br>
Any exceptions thrown by the remote service will be rethrown locally when calling `future::get()`. <br>
Exceptions may also be generated when calling a non-existent function, invalid arguments, network errors, etc.

Service:
```cpp
...
    std::string hello_world() {
        throw std::runtime_error("Hello World!");
    }
...
```

Client:
```cpp
...
    std::future<std::string> result = client.async_call<std::string>("hello_world");
    try {
        std::cout << result.get() << '\n';
    } catch (const std::exception& e) {
        std::cout << e.what() << '\n';
    }
```

<h2> non-const lvalue& </h2>
Functions with non-const lvalue& arguments can be bound/called, these arguments will be transfered to the server and then back to the client.

Service:
```cpp
...
void hello_world(std::string& str) {
    str = "Hello World!";
}
...
```

Client:
```cpp
...
std::string str;
client.async_call<void>("hello_world", str).get();
std::cout << str << '\n';
```

Note that the `str` must be kept alive until `std::future::get()` is called, otherwise the behavior is undefined.



<h2> Serialization and RPC buffers </h2>
MsgPack is used for serialization, custom types being passsed as arguments/return values must have a packer/unpacker. <br>
The library provides a way to bypass serialization by using RPC buffers, which will be written directly to the socket. <br>
Custom types must additionally provide a default constructor, a move constructor and a move assignment operator. <br>

Command buffers have a limited size (2^20 bytes == 1MiB), any large buffers should go through the rpc::buffer interface.

An example custom type can be found in `example/File.h`

`rpc::buffer::enqueue_write()` and `rpc::buffer::enqueue_read()` will write/read data to/from the socket, 
the enqueued reads/writes will be executed once the packed message is passed.

Note that `rpc::buffer::euqneueue_write()` optionally takes a deallocator function:
`void enqueue_write(void *ptr, uint32_t count, std::function<void(void *)> &&deleter = [](void*) {});`

<h2> Example </h2>
A full file transfer client/server example is provided in `example/ssl_file_transfer_client.cpp` and `examples/ssl_file_transfer_server.cpp` <br>