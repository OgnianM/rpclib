#include <rpc/node.hpp>


/**
 * @brief A server with example functions
 */
struct basic_server : rpc::node {
    basic_server(rpc::ISocket* socket) : rpc::node(socket) {
        // Bind your functions here

        // Member functions
        bind("add", &basic_server::add, this);
        bind("strcat", &basic_server::strcat, this);
        bind("exception", &basic_server::exception, this);

        // Lambdas
        bind("echo", [](std::string msg) {
            return msg;
        });

        // Free functions
        //bind("malloc", malloc);
        //bind("open", open);
        //bind("read", read);
        //bind("write", write);
    }

    int add(int a, int b) {
        return a + b;
    }

    std::string strcat(const std::string& a, const std::string& b) {
        return a + b;
    }

    std::vector<int> exception() {
        throw std::runtime_error("This is a test exception");
    }
};


int main() {

    // ASIO-specific stuff
    asio::io_context context;
    // work guard
    asio::executor_work_guard<decltype(context.get_executor())> work{context.get_executor()};
    std::thread io_thread([&context]() {
        context.run();
    });

    rpc::service_provider<rpc::types::socket_t, basic_server> service_provider(
        context, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 12345)
    );
    service_provider.start();

    std::shared_ptr<basic_server> service_instance;
    // Optional callback for new connections, the service instance is considered owned by the client and will be kept
    // alive as long as the connection persists, though you may keep a pointer to the service and call its `stop`
    // method to kill it on the server-side.
    service_provider.on_service_created_callback = [&service_instance](std::shared_ptr<basic_server>& service) {
        std::cout << "New client connected!" << std::endl;
        service_instance = service;
    };



    /// !CLIENT SPECIFIC STUFF!

    // Connect to the service_provider
    auto client = rpc::node::connect(context.get_executor(), asio::ip::tcp::endpoint(
        asio::ip::make_address("127.0.0.1"), 12345));

    // This executes asynchronously on the remote, returns a future
    auto result = client->async_call<int>("add", 5, 10).get();
    std::cout << "5 + 10 = " << result << std::endl;

    // The remote throws an exception, which is propagated to the client through the future object
    auto exception_future = client->async_call<std::vector<int>>("exception");
    try {
        exception_future.get();
    } catch (rpc::peer_exception& e) {
        std::cout << "Caught remote exception: " << e.what() << std::endl;
    }
    /// @note The connection is still valid after the exception gets handled

    auto echo = client->async_call<std::string>("echo", "Hello World!");
    std::cout << "Echoed: " << echo.get() << std::endl;

    // Invalid function calls also get handled by the peer_exception system
    auto invalid_future = client->async_call<int>("non_existing_function");
    try {
        invalid_future.get();
    } catch (rpc::peer_exception &e) {
        std::cout << "Caught remote exception for invalid function: " << e.what() << std::endl;
    }

    // The "client-server" dynamic only determines who connects to whom, both peers can send and receive instructions
    // at the same time

    client->bind("client_function", [](int value) {
        std::cout << "Client function called with value: " << value << std::endl;
    });

    service_instance->async_call<void>("client_function", 42).get();


    context.stop();
    io_thread.join();
    return 0;
}