#ifndef PARSER_MANAGER_HPP
#define PARSER_MANAGER_HPP

#include "parser_worker.hpp"
#include <string>

namespace services {

/** 管理当前唯一活跃的解析器实例 */
class ParserManager {
public:
    static ParserManager& instance();

    IParserWorker* active() const { return active_; }

    /** 若已有注册解析器，将其 RingBuffer 绑定到 TcpSender（init 阶段调用） */
    void bind_tcp_output();

    /** 按路径选择解析器并 open_file；切换协议时会 stop 旧实例 */
    int open_file(const std::string& path);

    void start_parse();
    void stop_parse();

private:
    ParserManager() = default;

    IParserWorker* active_ = nullptr;
};

} // namespace services

#endif // PARSER_MANAGER_HPP
