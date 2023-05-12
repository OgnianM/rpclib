#include <rpc/client.h>
#include "File.h"
#include <filesystem>
#include <thread>

namespace fs = std::filesystem;


int main(int argc, char **argv) {
    if (argc != 4) {
        std::cerr << "Usage: " << argv[0] << " <host> <port> <password>\n";
        return 1;
    }

    asio::ssl::context ssl_ctx(asio::ssl::context::tlsv12);
    asio::io_context io_ctx(1);
    asio::executor_work_guard<decltype(io_ctx.get_executor())> work{io_ctx.get_executor()};
    std::jthread t([&io_ctx]() { io_ctx.run(); });


    rpc::client<rpc::types::ssl_socket_t> client(io_ctx, argv[1], std::stoi(argv[2]), ssl_ctx);

    if (!client.async_call<bool>("authenticate", std::string(argv[3])).get()) {
        std::cout << "Authentication failed\n";
        io_ctx.stop();
        return 1;
    }

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
            } else if (command == "get") {
                std::string remote_path, local_path;
                std::cin >> remote_path >> local_path;
                auto file = client.async_call<File>("get", remote_path).get();
                std::ofstream out(local_path, std::ios::trunc);
                out.write((char*)file.data, file.size);
            } else if (command == "test") {
                int x = 0;
                client.async_call<void>("test", x).get();
                std::cout << "x = " << x << '\n';
            } else if (command == "get_dir") {
                std::string remote_path, local_path_;
                std::cin >> remote_path >> local_path_;
                auto file = client.async_call<std::vector<File>>("get_dir", remote_path).get();

                fs::path local_path = local_path_;

                for (auto &f: file) {
                    fs::create_directories((local_path / f.path).parent_path());
                    std::ofstream out(local_path / f.path, std::ios::trunc);
                    if (f.data) {
                        out.write((char *) f.data, f.size);
                    }
                }
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
            } else if (command == "pwd") {
                std::cout << client.async_call<std::string>("pwd").get() << '\n';
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