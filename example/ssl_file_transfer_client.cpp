#include <rpc/client.h>
#include "File.h"


int main() {
    asio::ssl::context ssl_ctx(asio::ssl::context::tlsv12);
    asio::io_context io_ctx(1);
    asio::executor_work_guard<decltype(io_ctx.get_executor())> work{io_ctx.get_executor()};
    std::jthread t([&io_ctx]() { io_ctx.run(); });


    rpc::client<rpc::types::ssl_socket_t> client(io_ctx, "localhost", 1234, ssl_ctx);


    while (true) {
        std::string command;

        std::cout << "> ";
        std::cin >> command;


        try {
            if (command == "ls") {
                auto files = client.async_call<std::vector<std::string>>("ls").get();
                for (auto &i: files) {
                    std::cout << "-> " << i << '\n';
                }
            } else if (command == "cd") {
                std::string path;
                std::cin >> path;
                client.async_call<void>("cd", path).get();
            } else if (command == "authenticate") {
                std::string pass;
                std::cin >> pass;
                if (client.async_call<bool>("authenticate", pass).get()) {
                    std::cout << "Authenticated\n";
                } else {
                    std::cout << "Authentication failed\n";
                }
            } else if (command == "get") {
                std::string remote_path, local_path;
                std::cin >> remote_path >> local_path;
                auto file = client.async_call<File>("get", remote_path).get();
                std::ofstream out(local_path);
                out.write((char*)file.data, file.size);
            } else if (command == "put") {
                std::string remote_path, local_path;
                std::cin >> local_path >> remote_path;
                File f(local_path);
                client.async_call<void>("put", remote_path, f).get();
            }  else if (command == "lsf") {
                auto funcs = client.get_functions();
                for (auto& f : funcs) {
                    std::cout << "-> " << f << '\n';
                }
            } else if (command == "exit") {
                break;
            }
            else if (command == "fsize") {
                std::string path;
                std::cin >> path;
                std::cout << client.async_call<int>("fsize", path).get() << '\n';
            } else if (command == "help") {
                std::cout << "ls - list files\n"
                             "cd [dir] - change directory\n"
                             "authenticate [password] - authenticate\n"
                             "get [remote_path] [local_path] - get file\n"
                             "put [local_path] [remote_path] - put file\n"
                             "lsf - list functions\n"
                             "exit - exit\n"
                             "fsize [remote_file] - get file size\n"
                             "help - help\n";
            }
            else {
                std::cout << "Unknown command\n";
            }

        } catch(std::exception& e) {
            std::cout << "Command failed with " << e.what() << std::endl;
        }
    }

    std::cout << std::endl;


    io_ctx.stop();
    return 0;
}