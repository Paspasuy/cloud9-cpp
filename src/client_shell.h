#ifndef CLOUD9_CLIENT_SHELL_H
#define CLOUD9_CLIENT_SHELL_H

#include <string>
#include <vector>
#include <iostream>
#include <iomanip>
#include <filesystem>
#include <fstream>
#include "cloud_common.h"
#include "cloud_client.h"


std::string parse_command(const std::string &command, std::vector<std::string> &store) {
    std::string current;
    bool slash = false;
    for (size_t pos = 0; pos < command.size(); pos++) {
        char c = command[pos];
        if (c == '\\') {
            if (slash) {
                current += "\\";
                slash = false;
            } else slash = true;
        } else if (c == ' ') {
            if (slash) {
                current += " ";
                slash = false;
            } else {
                if (!current.empty()) {
                    store.push_back(current);
                    current = "";
                }
            }
        } else {
            if (slash) {
                return std::string("unknown escape sequence: \\") + c;
            } else current += c;
        }
    }
    if (slash) return "unfinished escape sequence";
    if (!current.empty()) store.push_back(current);
    return "";
}

bool is_absolute_path(const std::string &path) {
    return path[0] == CLOUD_PATH_HOME || path[0] == CLOUD_PATH_NODE;
}

std::string get_absolute_path_base(const std::string &path) {
    return path.substr(0, path.find(CLOUD_PATH_DIV));
}

Node get_absolute_path_base_node(CloudClient *client, const std::string &base) {
    if (base[0] == CLOUD_PATH_NODE) {
        return string2node(base.substr(1));
    } else if (base[0] == CLOUD_PATH_HOME) {
        return client->get_home(base.substr(1));
    } else throw std::invalid_argument("invalid base " + base);
}

Node get_relative_path_node(CloudClient *client, Node base, const std::string &path) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (size_t pos = 0; pos < path.length(); pos++) {
        if (path[pos] == '/') {
            parts.push_back(path.substr(start, pos - start));
            start = pos + 1;
        }
    }
    parts.push_back(path.substr(start, path.size()));
    Node current = base;
    for (const std::string &part : parts) {
        if (part.empty()) continue;
        else if (part == "..") client->get_parent(current, &current);
        else {
            bool found = false;
            client->list_directory(current, [&](const std::string &name, Node child) {
                if (name == part) {
                    found = true;
                    current = child;
                }
            });
            if (!found) throw std::runtime_error("'" + part + "' not found");
        }
    }
    return current;
}

Node get_path_node(CloudClient *client, Node cwd, const std::string &path) {
    if (is_absolute_path(path)) {
        std::string base = get_absolute_path_base(path);
        return get_relative_path_node(client, get_absolute_path_base_node(client, base), path.substr(base.length()));
    } else return get_relative_path_node(client, cwd, path);
}

std::string get_node_path(CloudClient *client, Node node) {
    Node parent;
    bool has_parent;
    has_parent = client->get_parent(node, &parent);
    if (has_parent) {
        std::string name;
        try {
            client->list_directory(parent, [&name, node](const std::string &child_name, Node child) {
                if (child == node)
                    name = child_name;
            });
        } catch (CloudRequestError &error) {
            if (error.status == REQUEST_ERR_FORBIDDEN)
                return get_node_path(client, parent) + CLOUD_PATH_DIV + CLOUD_PATH_UNKNOWN;
            else throw;
        }
        return get_node_path(client, parent) + CLOUD_PATH_DIV + name;
    } else {
        return "";
    }
}

#define PROGRESSBAR_SIZE 20

void print_loading_status(size_t done, size_t all, size_t start_time) {
    size_t cur_time = get_current_time_ms();
    double seconds = double(cur_time - start_time) / 1000;
    size_t speed = double(done) / seconds;
    double part = (all == 0) ? 1 : (double(done) / double(all));
    size_t segments = part * PROGRESSBAR_SIZE;
    std::string progress = "[";
    for (size_t i = 0; i < segments; i++) progress += "=";
    for (size_t i = 0; i < PROGRESSBAR_SIZE - segments; i++) progress += " ";
    progress += "]";
    auto[done_h, done_p] = human_readable_size(done);
    auto[all_h, all_p] = human_readable_size(all);
    auto[speed_h, speed_p] = human_readable_size(speed);
    std::ostringstream out;
    out << std::fixed << std::setprecision(1);
    out << done_h << " " << done_p << "B";
    out << "/";
    out << all_h << " " << all_p << "B";
    out << " ";
    out << progress;
    out << " ";
    out << (part * 100);
    out << "% ";
    out << speed_h << " " << speed_p << "B";
    out << "/s ";
    out << human_readable_time((cur_time - start_time) / 1000) << "/";
    out << human_readable_time(all / (speed + 1));
    std::cout << "\r\033[K\033[1F\033[1E";
    std::cout << out.str();
    std::cout.flush();
}

#define STATUS_DELAY 500

void put_file(CloudClient *client, const std::string &src, Node dst, bool info, size_t block_size) {
    size_t size = std::filesystem::file_size(src);
    std::ifstream stream(src);
    auto fd = client->fd_open(dst, NODE_FD_MODE_WRITE);
    char *buffer = new char[block_size];
    size_t done = 0;
    auto start_time = get_current_time_ms();
    size_t last_status_time = start_time;
    try {
        client->fd_write_long(fd, size, buffer, [&]() -> uint32_t {
            if (info) {
                if (get_current_time_ms() - last_status_time > STATUS_DELAY) {
                    print_loading_status(done, size, start_time);
                    last_status_time = get_current_time_ms();
                }
            }
            stream.read(buffer, block_size);
            uint32_t read = stream.gcount();
            done += read;
            return read;
        });
    } catch (...) {
        delete[] buffer;
        throw;
    }
    if (info) print_loading_status(done, size, start_time);
    delete[] buffer;
    client->fd_close(fd);
    if (info) std::cout << std::endl;
}

void put_node(CloudClient *client, const std::string &file, Node dst_dir, bool info, size_t block_size, bool recursive,
              const std::string &dst_dir_path) {
    std::string name = std::filesystem::absolute(std::filesystem::path(file)).filename();
    if (!std::filesystem::exists(file)) {
        std::cerr << "put: '" << file << "' does not exist" << std::endl;
        return;
    }
    if (std::filesystem::is_regular_file(file)) {
        if (info) std::cout << file << "\t-->\t" << dst_dir_path << name << std::endl;
        Node dst = client->make_node(dst_dir, name, NODE_TYPE_FILE);
        put_file(client, file, dst, info, block_size);
    } else if (std::filesystem::is_directory(file)) {
        if (recursive) {
            if (info) std::cout << "mkdir " << dst_dir_path << name << std::endl;
            Node dst = client->make_node(dst_dir, name, NODE_TYPE_DIRECTORY);
            for (const auto &child : std::filesystem::directory_iterator(file)) {
                put_node(client, child.path(), dst, info, block_size, recursive, dst_dir_path + name + CLOUD_PATH_DIV);
            }
        } else std::cout << "put: non-recursive, skipping directory " << file << std::endl;
    } else {
        std::cout << "put: skipping other file " << file << std::endl;
    }
}

void get_file(CloudClient *client, Node src, const std::string &dst, bool info, size_t block_size) {
    NodeInfo node_info = client->get_node_info(src);
    std::ofstream stream(dst);
    auto fd = client->fd_open(src, NODE_FD_MODE_READ);
    size_t done = 0;
    auto start_time = get_current_time_ms();
    size_t last_status_time = start_time;
    char *buffer = new char[block_size];
    try {
        client->fd_read_long(fd, node_info.size, buffer, block_size, [&](uint32_t read) {
            stream.write(buffer, read);
            done += read;
            if (info) {
                if (get_current_time_ms() - last_status_time > STATUS_DELAY) {
                    print_loading_status(done, node_info.size, start_time);
                    last_status_time = get_current_time_ms();
                }
            }
        });
    } catch (...) {
        delete[] buffer;
        throw;
    }
    delete[] buffer;
    if (info) print_loading_status(node_info.size, node_info.size, start_time);
    client->fd_close(fd);
    if (info) std::cout << std::endl;
}

void get_node(CloudClient *client, Node node, const std::string &dst_dir, bool info, size_t block_size, bool recursive,
              const std::string &node_path, const std::string &node_name) {
    NodeInfo node_info = client->get_node_info(node);
    if (node_info.type == NODE_TYPE_FILE) {
        if (info) std::cout << dst_dir << node_name << "\t<--\t" << node_path << std::endl;
        if (std::filesystem::exists(dst_dir + node_name)) throw std::runtime_error("file exists");
        get_file(client, node, dst_dir + node_name, info, block_size);
    } else if (node_info.type == NODE_TYPE_DIRECTORY) {
        if (recursive) {
            if (info) std::cout << "mkdir " << dst_dir << node_name << std::endl;
            std::filesystem::create_directory(dst_dir + node_name);
            client->list_directory(node, [=](const std::string &child_name, Node child) {
                get_node(client, child, dst_dir + node_name + PATH_DIV, info, block_size, recursive,
                         node_path + CLOUD_PATH_DIV + child_name, child_name);
            });
        } else std::cout << "get: non-recursive, skipping directory " << node_path << std::endl;
    }
}

std::string get_node_name(CloudClient *client, Node node) {
    Node parent;
    if (client->get_parent(node, &parent)) {
        std::string name;
        client->list_directory(parent, [&name, node](const std::string &child_name, Node child) {
            if (child == node) {
                name = child_name;
            }
        });
        return name;
    } else {
        return CLOUD_PATH_HOME + client->get_node_owner(node);
    }
}

std::string node_desc(CloudClient *client, Node node, bool hidden, bool long_list) {
    std::string result;
    std::string name = get_node_name(client, node);;
    if (name.find('.') == 0 && !hidden) return "";
    NodeInfo info = client->get_node_info(node);
    if (long_list) {
        if (info.type == NODE_TYPE_FILE) result += '-';
        else if (info.type == NODE_TYPE_DIRECTORY) result += 'd';
        else result += '?';
        result += rights2string(info.rights);
        result += '\t';
        result += client->get_node_group(node);
        result += '\t';
        std::string size_s = std::to_string(info.size);
        result += size_s;
        result += '\t';
        if (size_s.length() < 8) result += '\t';
    }
    result += name;
    if (info.type == NODE_TYPE_DIRECTORY) result += CLOUD_PATH_DIV;
    result += '\n';
    return result;
}

void list_commands() {
    std::cout << "Available commands:" << std::endl;
    std::cout << " " << "ls" "\t\t" "cd" "\t\t" "pwd" << std::endl;
    std::cout << " " << "mkdir" "\t\t" "node" "\t\t" "put" << std::endl;
    std::cout << " " << "get" "\t\t" "chmod" "\t\t" "group" << std::endl;
    std::cout << " " << "rm" "\t\t" "chown" "\t\t" "mv" << std::endl;
    std::cout << " " << "cp" "\t\t" "rn" "\t\t" "help" << std::endl;
    std::cout << std::endl;
    std::cout << "Type 'help help' for further information." << std::endl;
}

void print_help(std::string cmd) {
    bool all = cmd == "*";
    bool ok = false;
    if (all | cmd == "ls") {
        ok = true;
        std::cout << "ls [OPTIONS]... [DIRS]..." << std::endl;
        std::cout << "\t" "List files in directories" << std::endl;
        std::cout << "\t" " If no DIRS specified, list files in current directory" << std::endl;
        std::cout << "\t" " Options:" << std::endl;
        std::cout << "\t" "  " "-a" "\t" "list hidden files" << std::endl;
        std::cout << "\t" "  " "-l" "\t" "show additional information" << std::endl;
    }
    if (all | cmd == "cd") {
        ok = true;
        std::cout << "cd [DIR]" << std::endl;
        std::cout << "\t" "Set current working directory (CWD) to DIR" << std::endl;
        std::cout << "\t" " If no DIR specified, sets it to your home." << std::endl;
        std::cout << "\t" " Cloud9 paths are differ from your system FS paths." << std::endl;
        std::cout << "\t" " Every node (file or directory) has its own 16-byte ID." << std::endl;
        std::cout << "\t" " IDs are represented with 32 hexadecimal digits (i. e. 4e7cdabec0a073b085987a2f71d14157).\n";
        std::cout << "\t" " Any path starting with ~ or # is absolute, others are relative to the CWD." << std::endl;
        std::cout << "\t" " Path that starts with #<NODE_ID> means that it is relative to node with id NODE_ID.\n";
        std::cout << "\t" " I. e. path '#4e7cdabec0a073b085987a2f71d14157/folder1' represents path "
                  << "to the folder 'folder1' which is located in the directory with ID 4e7cdabec0a073b085987a2f71d14157."
                  << std::endl;
        std::cout << "\t" " Any path which starts with ~<USER> represent path relative to USER's home directory."
                  << std::endl;
    }
    if (all | cmd == "pwd") {
        ok = true;
        std::cout << "pwd" << std::endl;
        std::cout << "\t" "Print path to the CWD" << std::endl;
    }
    if (all | cmd == "mkdir") {
        ok = true;
        std::cout << "mkdir <PATH>" << std::endl;
        std::cout << "\t" "Create new directory" << std::endl;
    }
    if (all | cmd == "node") {
        ok = true;
        std::cout << "node [PATH]" << std::endl;
        std::cout << "\t" "Print ID of the node located at PATH" << std::endl;
        std::cout << "\t" " PATH defaults to the CWD" << std::endl;
    }
    if (all | cmd == "put") {
        ok = true;
        std::cout << "put [OPTIONS]... <FILES>... <DIRECTORY>" << std::endl;
        std::cout << "\t" "Upload local FILES to the remote DIRECTORY" << std::endl;
        std::cout << "\t" " Options:" << std::endl;
        std::cout << "\t" "  " "-r" "\t" "operate recursively" << std::endl;
        std::cout << "\t" "  " "-s" "\t" "operate silently" << std::endl;
        std::cout << "\t" "  " "-b=<N>" "\t" "read up to N bytes at a time, default is 640 KiB" << std::endl;
    }
    if (all | cmd == "get") {
        ok = true;
        std::cout << "get [OPTIONS]... <FILES>... <DIRECTORY>" << std::endl;
        std::cout << "\t" "Download remote FILES to the local DIRECTORY" << std::endl;
        std::cout << "\t" " Options:" << std::endl;
        std::cout << "\t" "  " "-r" "\t" "operate recursively" << std::endl;
        std::cout << "\t" "  " "-s" "\t" "operate silently" << std::endl;
        std::cout << "\t" "  " "-b=<N>" "\t" "write up to N bytes at a time, default is 640 KiB" << std::endl;
    }
    if (all | cmd == "chmod") {
        ok = true;
        std::cout << "chmod <RIGHTS> <NODE>" << std::endl;
        std::cout << "\t" "Set NODE's RIGHTS" << std::endl;
        std::cout << "\t" " Every node has four rights: group read, group write, any read, any write." << std::endl;
        std::cout << "\t" " Full set of rights is represented with 1111, none of them - with 0000" << std::endl;
        std::cout << "\t" " Only the owner of the node could change its rights and unconditionally do anything else.\n";
    }
    if (all | cmd == "group") {
        ok = true;
        std::cout << "group" << std::endl;
        std::cout << "\t" "Print all the groups which you are the member of" << std::endl;
        std::cout << "\t" " Any user has its own group (just one) and could belong to several other users' groups.\n";
    }
    if (all | cmd == "group") {
        ok = true;
        std::cout << "group invite <USERS>..." << std::endl;
        std::cout << "\t" "Make USERS members of your group" << std::endl;
    }
    if (all | cmd == "group") {
        ok = true;
        std::cout << "group kick <USERS>..." << std::endl;
        std::cout << "\t" "Remove USERS from your group" << std::endl;
    }
    if (all | cmd == "rm") {
        ok = true;
        std::cout << "rm <NODES>..." << std::endl;
        std::cout << "\t" << "Remove NODES" << std::endl;
        std::cout << "\t" << " Non-empty directories removal is unsupported" << std::endl;
    }
    if (all | cmd == "chown") {
        ok = true;
        std::cout << "chown <USER> <NODES>..." << std::endl;
        std::cout << "\t" "Set NODES' group to USER" << std::endl;
        std::cout << "\t" " Only the owner of the node could change its group" << std::endl;
    }
    if (all | cmd == "mv") {
        ok = true;
        std::cout << "mv <NODE> <DIRECTORY>" << std::endl;
        std::cout << "\t" "Move NODE to the DIRECTORY" << std::endl;
    }
    if (all | cmd == "cp") {
        ok = true;
        std::cout << "cp <NODE> <NAME>" << std::endl;
        std::cout << "\t" "Create NODE's clone with name NAME" << std::endl;
    }
    if (all | cmd == "rn") {
        ok = true;
        std::cout << "rn <NODE> <NAME>" << std::endl;
        std::cout << "\t" "Rename NODE to NAME" << std::endl;
    }
    if (all | cmd == "help") {
        ok = true;
        std::cout << "help" << std::endl;
        std::cout << "\t" "Print available commands" << std::endl;
    }
    if (all | cmd == "help") {
        ok = true;
        std::cout << "help <COMMANDS>..." << std::endl;
        std::cout << "\t" "Print help about each of COMMANDS" << std::endl;
    }
    if (!ok) std::cerr << "help: no such command '" << cmd << "'" << std::endl;
}

int shell(CloudClient *client, NetConnection *connection, const std::string &login, const std::string &host) {
    const std::map<std::string, std::function<void(CloudClient *, Node &, std::vector<std::string> &)>> commands{
            {"ls",   [](CloudClient *client, Node &cwd, std::vector<std::string> &args) {
                std::string target;
                std::string options;
                for (auto &a : args) {
                    if (!a.empty() && a[0] == '-') {
                        options += a.substr(1);
                    } else {
                        if (!target.empty()) {
                            std::cerr << "ls: too much arguments" << std::endl;
                            return;
                        } else target = a;
                    }
                }
                bool hidden = false;
                bool long_list = false;
                for (auto o : options) {
                    if (o == 'l') {
                        long_list = true;
                    } else if (o == 'a') {
                        hidden = true;
                    } else {
                        std::cerr << "ls: unknown option '" << o << "'" << std::endl;
                        return;
                    }
                }
                if (target.empty() || target.find(CLOUD_PATH_DIV) == target.length() - 1) {
                    Node node = target.empty() ? cwd : get_path_node(client, cwd, target);
                    std::vector<std::pair<std::string, Node>> children;
                    client->list_directory(node, [&](const std::string &name, Node child) {
                        children.emplace_back(name, child);
                    });
                    std::sort(children.begin(), children.end());
                    for (auto[name, child] : children) {
                        std::cout << node_desc(client, child, hidden, long_list);
                    }
                } else {
                    std::cout << node_desc(client, get_path_node(client, cwd, target), hidden, long_list);
                }
            }},
            {"cd",    [](CloudClient *client, Node &cwd, std::vector<std::string> &args) {
                if (args.empty()) {
                    cwd = client->get_home();
                } else if (args.size() == 1) {
                    cwd = get_path_node(client, cwd, args[0]);
                } else std::cerr << "cd: too many arguments" << std::endl;
            }},
            {"pwd",   [](CloudClient *client, Node &cwd, std::vector<std::string> &args) {
                if (!args.empty()) std::cerr << "pwd: too many arguments" << std::endl;
                else {
                    std::cout << CLOUD_PATH_HOME << client->get_node_owner(cwd) << get_node_path(client, cwd)
                              << std::endl;
                }
            }},
            {"mkdir", [](CloudClient *client, Node &cwd, std::vector<std::string> &args) {
                if (args.empty()) std::cerr << "mkdir: not enough arguments" << std::endl;
                else if (args.size() == 1) {
                    std::string path = args[0];
                    auto i = path.find_last_of(CLOUD_PATH_DIV);
                    Node parent = cwd;
                    std::string name = path;
                    if (i != std::string::npos) {
                        parent = get_path_node(client, cwd, path.substr(0, i));
                        name = path.substr(i + 1);
                    }
                    client->make_node(parent, name, NODE_TYPE_DIRECTORY);
                } else std::cerr << "mkdir: too many arguments" << std::endl;
            }},
            {"node",  [](CloudClient *client, Node &cwd, std::vector<std::string> &args) {
                if (args.empty()) std::cout << "#" << node2string(cwd) << std::endl;
                else if (args.size() == 1) {
                    std::string result = node2string(get_path_node(client, cwd, args[0]));
                    std::cout << "#" << result << std::endl;
                } else std::cerr << "node: too many arguments" << std::endl;
            }},
            {"put",   [](CloudClient *client, Node &cwd, std::vector<std::string> &args) {
                std::vector<std::string> options;
                std::vector<std::string> files;
                for (auto &arg : args) {
                    if (arg.find("-") == 0) options.push_back(arg.substr(1));
                    else files.push_back(arg);
                }
                bool info = true;
                size_t block_size = DEFAULT_DATA_BUFFER_SIZE;
                bool recursive = false;
                for (auto &option : options) {
                    if (option == "s") info = false;
                    else if (option == "r") recursive = true;
                    else if (option.find("b=") == 0) {
                        if (!is_number(option.substr(2))) {
                            std::cerr << "Buffer size must be a number" << std::endl;
                            return;
                        }
                        block_size = std::stoll(option.substr(2));
                    } else {
                        std::cerr << "put: unknown option " << option << std::endl;
                        return;
                    }
                }
                if (files.size() < 2) {
                    std::cerr << "put: no destination directory specified" << std::endl;
                    return;
                }
                std::string dst_dir_path = files.back();
                Node dst_dir = get_path_node(client, cwd, dst_dir_path);
                dst_dir_path = CLOUD_PATH_HOME + client->get_node_owner(dst_dir) + get_node_path(client, dst_dir) +
                               CLOUD_PATH_DIV;
                files.pop_back();
                if (files.empty()) {
                    std::cerr << "put: no source files given" << std::endl;
                    return;
                }
                for (auto &file : files) {
                    put_node(client, file, dst_dir, info, block_size, recursive, dst_dir_path);
                }
            }},
            {"get",   [](CloudClient *client, Node &cwd, std::vector<std::string> &args) {
                std::vector<std::string> options;
                std::vector<std::string> files;
                for (auto &arg : args) {
                    if (arg.find("-") == 0) options.push_back(arg.substr(1));
                    else files.push_back(arg);
                }
                bool info = true;
                size_t block_size = DEFAULT_DATA_BUFFER_SIZE;
                bool recursive = false;
                for (auto &option : options) {
                    if (option == "s") info = false;
                    else if (option == "r") recursive = true;
                    else if (option.find("b=") == 0) {
                        if (!is_number(option.substr(2))) {
                            std::cerr << "Buffer size must be a number" << std::endl;
                            return;
                        }
                        block_size = std::stoll(option.substr(2));
                    } else {
                        std::cerr << "get: unknown option " << option << std::endl;
                        return;
                    }
                }
                if (files.empty()) {
                    std::cerr << "get: no destination directory specified" << std::endl;
                    return;
                }
                std::string dst_dir = files.back();
                files.pop_back();
                if (files.empty()) {
                    std::cerr << "get: no source files given" << std::endl;
                    return;
                }
                for (auto &file : files) {
                    Node node = get_path_node(client, cwd, file);
                    std::string path = get_node_path(client, node);
                    std::string name;
                    if (path.size() <= 1) name = client->get_node_owner(node);
                    else name = path.substr(path.find_last_of(CLOUD_PATH_DIV) + 1);
                    get_node(client, node, dst_dir + PATH_DIV, info, block_size, recursive,
                             CLOUD_PATH_HOME + client->get_node_owner(node) + path, name);
                }
            }},
            {"chmod", [](CloudClient *client, Node &cwd, std::vector<std::string> &args) {
                if (args.size() < 2) {
                    std::cerr << "chmod: not enough arguments" << std::endl;
                } else if (args.size() > 2) {
                    std::cerr << "chmod: too many arguments" << std::endl;
                } else {
                    std::string s_rights = args[0];
                    std::string path = args[1];
                    Node target = get_path_node(client, cwd, path);
                    if (s_rights.size() != 4 || s_rights.find_first_not_of("01") != std::string::npos) {
                        std::cerr << "chmod: invalid rights" << std::endl;
                        return;
                    }
                    uint8_t rights =
                            uint8_t(s_rights[0] == '1' ? NODE_RIGHTS_GROUP_READ : 0) |
                            uint8_t(s_rights[1] == '1' ? NODE_RIGHTS_GROUP_WRITE : 0) |
                            uint8_t(s_rights[2] == '1' ? NODE_RIGHTS_ALL_READ : 0) |
                            uint8_t(s_rights[3] == '1' ? NODE_RIGHTS_ALL_WRITE : 0);
                    client->set_node_rights(target, rights);
                }
            }},
            {"group", [login](CloudClient *client, Node &cwd, std::vector<std::string> &args) {
                if (args.empty()) {
                    std::cout << login << ":";
                    client->group_list([](const std::string &group) {
                        std::cout << " " << group;
                    });
                    std::cout << std::endl;
                } else {
                    std::string cmd = args[0];
                    if (cmd == "invite") {
                        std::for_each(args.begin() + 1, args.end(), [client, login](auto user) {
                            std::cout << user << " -> " << login << std::endl;
                            client->group_invite(user);
                        });
                    } else if (cmd == "kick") {
                        std::for_each(args.begin() + 1, args.end(), [client, login](auto user) {
                            std::cout << "kick " << user << " from " << login << std::endl;
                            client->group_kick(user);
                        });
                    } else {
                        std::cerr << "group: unknown subcommand" << std::endl;
                    }
                }
            }},
            {"rm",    [](CloudClient *client, Node &cwd, std::vector<std::string> &args) {
                if (args.empty()) std::cerr << "rm: not enough arguments" << std::endl;
                else {
                    for (std::string &path: args) {
                        Node node = get_path_node(client, cwd, path);
                        client->remove_node(node);
                    }
                }
            }},
            {"chown", [](CloudClient *client, Node &cwd, std::vector<std::string> &args) {
                if (args.size() < 2) {
                    std::cerr << "chown: not enough arguments" << std::endl;
                } else {
                    std::string group = args[0];
                    std::for_each(args.begin() + 1, args.end(), [client, cwd, group](auto path) {
                        std::cout << path << " -> " << group << std::endl;
                        Node node = get_path_node(client, cwd, path);
                        client->set_node_group(node, group);
                    });
                }
            }},
            {"mv",   [](CloudClient *client, Node &cwd, std::vector<std::string> &args) {
                if (args.size() < 2) std::cerr << "mv: not enough arguments" << std::endl;
                else {
                    Node new_parent = get_path_node(client, cwd, args.back());
                    std::for_each(args.begin(), args.end() - 1,
                                  [&cwd, &args, client, new_parent](const std::string &path) {
                                      std::cout << path << " -> " << args.back() << std::endl;
                                      client->move_node(get_path_node(client, cwd, path), new_parent);
                                  });
                }
            }},
            {"cp",   [](CloudClient *client, Node &cwd, std::vector<std::string> &args) {
                if (args.size() != 2) std::cerr << "cp: exactly 2 arguments expected" << std::endl;
                else {
                    Node node = get_path_node(client, cwd, args[0]);
                    std::string name = args[1];
                    client->copy_node(node, name);
                }
            }},
            {"rn",   [](CloudClient *client, Node &cwd, std::vector<std::string> &args) {
                if (args.size() != 2) std::cerr << "rn: exactly 2 arguments expected" << std::endl;
                else {
                    Node node = get_path_node(client, cwd, args[0]);
                    std::string name = args[1];
                    client->rename_node(node, name);
                }
            }},
            {"help", [](CloudClient *client, Node &cwd, std::vector<std::string> &args) {
                if (args.empty()) list_commands();
                else {
                    for (const std::string &cmd : args) {
                        print_help(cmd);
                    }
                }
            }}
    };
    Node cwd = client->get_home(login);
    std::string command;
    std::vector<std::string> command_store;
    bool fail = false;
    while (true) {
        std::cout << login << "@" << host << "$ ";
        if (!std::getline(std::cin, command)) {
            std::cout << std::endl;
            std::cout << "Logout, connection closed" << std::endl;
            break;
        }
        command_store.clear();
        std::string error = parse_command(command, command_store);
        if (error.empty()) {
            if (!command_store.empty()) {
                std::string command_name = command_store.front();
                command_store.erase(command_store.begin());
                if (commands.find(command_name) != commands.end()) {
                    try {
                        commands.at(command_name)(client, cwd, command_store);
                    } catch (std::runtime_error &error) {
                        std::cerr << "Error: " << error.what() << std::endl;
                    } catch (CloudRequestError &error) {
                        std::cerr << "Request failed: " << error.what() << std::endl;
                    }
                } else if (command == "exit") {
                    return fail;
                } else std::cerr << "No such command: " << command_name << std::endl;
            }
        } else std::cerr << "Failed to parse command: " << error << std::endl;
        if (!connection->is_valid()) {
            std::cerr << "Lost connection to '" << host << "'" << std::endl;
            fail = true;
            break;
        }
    }
    return fail;
}

#endif //CLOUD9_CLIENT_SHELL_H
