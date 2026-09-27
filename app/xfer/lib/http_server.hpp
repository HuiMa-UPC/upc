#ifndef UPC_XFR_HTTP_SERVER_HPP
#define UPC_XFR_HTTP_SERVER_HPP

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace upc::xfr {

struct HttpRequest {
    std::string method;
    std::string path;
    std::string version;
    std::unordered_map<std::string, std::string> headers;
    std::vector<uint8_t> body;

    std::string header(const std::string& key) const;
};

struct HttpResponse {
    int status = 200;
    std::string content_type = "application/json";
    std::string body;
};

class HttpServer {
public:
    using Handler = std::function<HttpResponse(const HttpRequest&)>;

    HttpServer() = default;
    ~HttpServer();

    bool start(uint16_t port, Handler handler);
    void stop();

private:
    void run_accept();
    void handle_client(int fd);
    bool should_keep_alive(const HttpRequest& req) const;

    static bool read_request(int fd, HttpRequest* out);
    static bool send_response(int fd, const HttpResponse& rsp, bool keep_alive);
    static std::string trim_copy(const std::string& s);
    static std::string to_lower(std::string s);

    int listen_fd_ = -1;
    std::atomic<bool> running_{false};
    std::atomic<int> inflight_{0};
    std::mutex stop_mu_;
    std::condition_variable stop_cv_;
    std::thread accept_thread_;
    Handler handler_;
};

} // namespace upc::xfr

#endif
