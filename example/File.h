#pragma once
#include <fstream>
#include <string>

struct File {
    File(const std::string& name) {
        std::ifstream ifs(name, std::ios::binary);
        if (!ifs.good()) {
            throw std::runtime_error("File not found");
        }
        ifs.seekg(0, std::ios::end);
        size = ifs.tellg();
        ifs.seekg(0, std::ios::beg);
        data = malloc(size);
        ifs.read((char*)data, size);
    }

    File(const File&) = delete;
    File& operator=(const File&) = delete;

    // region Required
    File() = default;

    File(File&& other) {
        data = other.data;
        size = other.size;
        other.data = nullptr;
        other.size = 0;
    }


    File& operator=(File&& other) {
        data = other.data;
        size = other.size;
        other.data = nullptr;
        other.size = 0;
        return *this;
    }
    // endregion

    ~File() {
        if (data) {
            free(data);
        }
    }
    void* data = nullptr;
    size_t size = 0;

    template<typename Packer>
    void msgpack_pack(Packer &msgpack_pk) const {
        msgpack::type::make_define_array(size).msgpack_pack(msgpack_pk);
        if (data) {
            rpc::buffer::enqueue_write(data, size);
        }
    }
    void msgpack_unpack(msgpack::object const &msgpack_o) {
        msgpack::type::make_define_array(size).msgpack_unpack(msgpack_o);
        if (size) {
            data = malloc(size);
            rpc::buffer::enqueue_read(data, size);
        }
    }
};
