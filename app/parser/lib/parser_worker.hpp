#ifndef PARSER_WORKER_HPP
#define PARSER_WORKER_HPP

#include "slice_ring_buffer.hpp"
#include <cstdint>
#include <string>

namespace upc::parser {

/** 切片文件解析器抽象接口；各协议在 parsers/<name>/ 下实现 */
class IParserWorker {
public:
    virtual ~IParserWorker() = default;

    /**
     * 为下一次 open_file 提供总行数提示。
     * 默认实现为 no-op；不支持的解析器可忽略。
     */
    virtual void set_total_lines_hint(int total_lines) { (void)total_lines; }

    virtual int  open_file(const std::string& filepath) = 0;
    virtual void start_parse() = 0;
    virtual void stop_parse() = 0;
    virtual void close_file() = 0;

    virtual int  get_total_lines() const = 0;
    virtual int  get_parsed_lines() const = 0;
    virtual int  get_total_motion_packets() const = 0;
    virtual bool is_parsing() const = 0;
    virtual bool is_file_read_exhausted() const = 0;
    virtual bool is_file_open() const = 0;

    virtual RingBuffer& ring_buffer() = 0;
    virtual uint16_t get_layer_height_um() const = 0;
};

} // namespace upc::parser

#endif // PARSER_WORKER_HPP
