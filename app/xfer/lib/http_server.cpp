#include "http_server.hpp"

#include "rs_log.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <sstream>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace upc::xfr {

std::string HttpRequest::header(const std::string& key) const
{
    auto it = headers.find(key);
    if (it != headers.end()) {
        return it->second;
    }
    std::string lower = key;
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    it = headers.find(lower);
    if (it != headers.end()) {
        return it->second;
    }
    return {};
}

HttpServer::~HttpServer()
{
    stop();
}

bool HttpServer::start(uint16_t port, Handler handler)
{
    if (running_) {
        return false;
    }
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        return false;
    }
    int opt = 1;
    (void)setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    if (listen(listen_fd_, 16) != 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    handler_ = std::move(handler);
    running_ = true;
    accept_thread_ = std::thread(&HttpServer::run_accept, this);
    UPC_LOG_XFR_SERVICE(Info, "http listen on :%u", static_cast<unsigned>(port));
    return true;
}

void HttpServer::stop()
{
    if (!running_) {
        return;
    }
    running_ = false;
    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
    std::vector<std::thread> clients;
    {
        std::lock_guard<std::mutex> lock(client_threads_mtx_);
        clients.swap(client_threads_);
    }
    for (auto& t : clients) {
        if (t.joinable()) {
            t.join();
        }
    }
}

void HttpServer::run_accept()
{
    while (running_) {
        int fd = ::accept(listen_fd_, nullptr, nullptr);
        if (fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (!running_) {
                break;
            }
            continue;
        }
        std::thread t(&HttpServer::handle_client, this, fd);
        std::lock_guard<std::mutex> lock(client_threads_mtx_);
        client_threads_.push_back(std::move(t));
    }
}

void HttpServer::handle_client(int fd)
{
    HttpRequest req;
    if (!read_request(fd, &req)) {
        ::close(fd);
        return;
    }
    HttpResponse rsp;
    if (handler_) {
        rsp = handler_(req);
    } else {
        rsp.status = 500;
        rsp.body = "{\"code\":\"NO_HANDLER\",\"message\":\"no http handler\"}";
    }
    send_response(fd, rsp);
    ::close(fd);
}

bool HttpServer::read_request(int fd, HttpRequest* out)
{
    if (!out) {
        return false;
    }
    std::string raw;
    raw.reserve(8192);
    char buf[2048];
    std::size_t header_end = std::string::npos;
    while (raw.size() < 1024U * 1024U) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            return false;
        }
        raw.append(buf, static_cast<std::size_t>(n));
        header_end = raw.find("\r\n\r\n");
        if (header_end != std::string::npos) {
            break;
        }
    }
    if (header_end == std::string::npos) {
        return false;
    }

    const std::string head = raw.substr(0, header_end);
    std::istringstream hs(head);
    std::string line;
    if (!std::getline(hs, line)) {
        return false;
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    {
        std::istringstream ls(line);
        ls >> out->method;
        ls >> out->path;
        std::string version;
        ls >> version;
        if (out->method.empty() || out->path.empty()) {
            return false;
        }
    }

    while (std::getline(hs, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::size_t p = line.find(':');
        if (p == std::string::npos) {
            continue;
        }
        const std::string k = to_lower(trim_copy(line.substr(0, p)));
        const std::string v = trim_copy(line.substr(p + 1));
        out->headers[k] = v;
    }

    std::size_t content_length = 0;
    const auto it = out->headers.find("content-length");
    if (it != out->headers.end()) {
        content_length = static_cast<std::size_t>(std::strtoull(it->second.c_str(), nullptr, 10));
    }

    const std::size_t body_start = header_end + 4;
    if (raw.size() > body_start) {
        const std::size_t remain = raw.size() - body_start;
        out->body.insert(out->body.end(),
                         reinterpret_cast<const uint8_t*>(raw.data() + body_start),
                         reinterpret_cast<const uint8_t*>(raw.data() + body_start + remain));
    }
    while (out->body.size() < content_length) {
        const std::size_t want = std::min<std::size_t>(sizeof(buf), content_length - out->body.size());
        const ssize_t n = ::recv(fd, buf, want, 0);
        if (n <= 0) {
            return false;
        }
        out->body.insert(out->body.end(),
                         reinterpret_cast<const uint8_t*>(buf),
                         reinterpret_cast<const uint8_t*>(buf + n));
    }
    return true;
}

bool HttpServer::send_response(int fd, const HttpResponse& rsp)
{
    std::ostringstream oss;
    oss << "HTTP/1.1 " << rsp.status << " ";
    switch (rsp.status) {
        case 200: oss << "OK"; break;
        case 400: oss << "Bad Request"; break;
        case 401: oss << "Unauthorized"; break;
        case 404: oss << "Not Found"; break;
        case 409: oss << "Conflict"; break;
        case 410: oss << "Gone"; break;
        case 413: oss << "Payload Too Large"; break;
        case 422: oss << "Unprocessable Entity"; break;
        case 429: oss << "Too Many Requests"; break;
        case 500: oss << "Internal Server Error"; break;
        default:  oss << "Error"; break;
    }
    oss << "\r\n";
    oss << "Content-Type: " << rsp.content_type << "\r\n";
    oss << "Content-Length: " << rsp.body.size() << "\r\n";
    oss << "Connection: close\r\n\r\n";
    const std::string head = oss.str();

    auto send_all = [fd](const char* p, std::size_t left) -> bool {
        while (left > 0) {
            const ssize_t n = ::send(fd, p, left, 0);
            if (n <= 0) {
                return false;
            }
            p += n;
            left -= static_cast<std::size_t>(n);
        }
        return true;
    };

    if (!send_all(head.data(), head.size())) {
        return false;
    }
    if (!rsp.body.empty() &&
        !send_all(rsp.body.data(), rsp.body.size())) {
        return false;
    }
    return true;
}

std::string HttpServer::trim_copy(const std::string& s)
{
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b])) != 0) {
        ++b;
    }
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])) != 0) {
        --e;
    }
    return s.substr(b, e - b);
}

std::string HttpServer::to_lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

} // namespace upc::xfr
