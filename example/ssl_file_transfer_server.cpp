#include <rpc/service_provider.h>
#include <filesystem>
#include <fstream>
#include "File.h"

namespace fs = std::filesystem;




template<typename socket_t>
struct FileService : rpc::server<socket_t> {
    FileService(socket_t&& sock) : rpc::server<socket_t>(std::move(sock)) {
        this->bind("authenticate", &FileService::authenticate);
    }

    bool authenticate(const std::string& password) {
        if (password == "1234") {
            this->clear_bound();
            this->bind("ls", &FileService::ls);
            this->bind("fsize", &FileService::fsize);
            this->bind("get", &FileService::get);
            this->bind("put", &FileService::put);
            this->bind("cd", &FileService::cd);
            return true;
        }
        return false;
    }

    void cd(const std::string& path) {
        if (!fs::exists(root / path)) {
            throw std::runtime_error("Path does not exist");
        }
        root = root / path;
    }

    std::vector<std::string> ls() {
        std::vector<std::string> files;
        for (const auto& entry : fs::directory_iterator(root)) {
            files.push_back(entry.path().filename().string());
        }
        return files;
    }

    int fsize(const std::string& file_name) {
        return fs::file_size(root / file_name);
    }

    File get(const std::string& file_name) {
        return {root / file_name};
    }

    void put(const std::string& file_name, const File& file) {
        std::ofstream ofs(root / file_name, std::ios::binary | std::ios::trunc);
        if (!ofs.good()) {
            throw std::runtime_error("Failed to open file");
        }
        ofs.write((char*)file.data, file.size);
    }

    fs::path root = fs::current_path();
};


int main() {
    asio::ssl::context ssl_ctx(asio::ssl::context::tlsv12);
    ssl_ctx.use_certificate_file("../example/certs/certificate.crt", asio::ssl::context::pem);
    ssl_ctx.use_private_key_file("../example/certs/privateKey.key", asio::ssl::context::pem);

    auto ep = asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 1234);

    rpc::service_provider<rpc::types::ssl_socket_t, FileService> service_provider(ep, ssl_ctx);
    service_provider.start();

    std::string input;

    while (true) {
        std::cout << "Type \"stop\" to stop the server: ";
        std::cin >> input;
        if (input == "stop") {
            break;
        }
    }
    service_provider.get_context().stop();
}