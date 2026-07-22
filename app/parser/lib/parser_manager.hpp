#ifndef UPC_PARSER_MANAGER_HPP
#define UPC_PARSER_MANAGER_HPP

#include "parser_output.hpp"
#include "parser_worker.hpp"

#include <string>

namespace upc::parser {

/** 管理当前唯一活跃的解析器实例；输出通过 RingBufferSink 交给下发层 */
class ParserManager {
public:
    static ParserManager& instance();

    void set_ring_buffer_sink(RingBufferSink sink);

    IParserWorker* active() const { return active_; }

    int  open_file(const std::string& path);
    int  open_file(const std::string& path, int total_lines_hint);
    void start_parse();
    void stop_parse();

private:
    ParserManager() = default;

    void notify_sink();

    RingBufferSink   sink_;
    IParserWorker*   active_ = nullptr;
};

} // namespace upc::parser

#endif /* UPC_PARSER_MANAGER_HPP */
