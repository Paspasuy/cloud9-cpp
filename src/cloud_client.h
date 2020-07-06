#ifndef CLOUD9_CLOUD_CLIENT_H
#define CLOUD9_CLOUD_CLIENT_H

#include <string>
#include <mutex>
#include <functional>
#include <thread>
#include <condition_variable>
#include <map>
#include "networking.h"
#include "cloud_common.h"

typedef struct {
    uint8_t type;
    uint64_t size;
} NodeInfo;

class CloudClient final {
private:
    struct ServerResponse {
        uint16_t status = 0;
        uint64_t size = 0;
        char *body = nullptr;
    };
    NetConnection *const connection;
    std::mutex api_lock;
    std::thread listener;
    std::condition_variable response_notifier;
    std::map<uint32_t, ServerResponse> responses;
    uint32_t current_id = 0;
    bool shutting_down = false;
    bool connected = true;
public:
    CloudClient(NetConnection *net, const std::string &login, std::string (*password_callback)(void *), void *ud);

    ~CloudClient();

    Node get_home(const std::string &user = "");

    void list_directory(Node node, const std::function<void(std::string, Node)> &callback);

    bool get_parent(Node node, Node *parent);

    Node make_node(Node parent, const std::string &name, uint8_t type);

    std::string get_node_owner(Node node);

    uint8_t fd_open(Node node, uint8_t mode);

    void fd_close(uint8_t fd);

    uint32_t fd_read(uint8_t fd, uint32_t n, void *bytes);

    void fd_write(uint8_t fd, uint32_t n, const void *bytes);

    NodeInfo get_node_info(Node node);

    void listener_routine();

    ServerResponse wait_response(uint32_t id, std::unique_lock<std::mutex> &locker);
};

class CloudInitError : public std::exception {
public:
    const uint16_t status;
    const std::string desc;

    explicit CloudInitError(uint16_t status);

public:
    const char *what() const noexcept override;
};

class CloudRequestError : public std::exception {
public:
    const uint16_t status;
    const std::string info;
    const std::string desc;

    explicit CloudRequestError(uint16_t status, std::string info = "");

public:
    const char *what() const noexcept override;
};

#endif //CLOUD9_CLOUD_CLIENT_H
