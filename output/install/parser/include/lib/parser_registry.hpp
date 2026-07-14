#ifndef PARSER_REGISTRY_HPP
#define PARSER_REGISTRY_HPP

#include "parser_worker.hpp"
#include <functional>
#include <string>
#include <vector>

namespace upc::parser {

struct ParserRegistration {
    const char* id = nullptr;
    std::vector<std::string> extensions;
    std::function<IParserWorker*()> factory;
};

class ParserRegistry {
public:
    static ParserRegistry& instance();

    void register_parser(ParserRegistration reg);

    /** 按文件路径后缀选择解析器；无匹配返回 nullptr */
    IParserWorker* resolve(const std::string& filepath) const;

    /** 文件名是否匹配任一已注册后缀（后缀比较统一小写） */
    bool match_filename(const std::string& filename) const;

    bool has_any() const { return !entries_.empty(); }

    /** 聚合所有已注册后缀（小写，含 '.'） */
    std::vector<std::string> all_extensions() const;

    /** 返回任意一个已注册解析器实例（仅用于 init 阶段绑定 RingBuffer，不打开文件） */
    IParserWorker* any_worker() const;

private:
    ParserRegistry() = default;

    struct Entry {
        std::string id;
        std::vector<std::string> extensions;
        std::function<IParserWorker*()> factory;
    };

    std::vector<Entry> entries_;
};

/** 提取文件名后缀并转小写，无后缀返回空串 */
std::string file_extension_lower(const std::string& filename);

} // namespace upc::parser

#endif // PARSER_REGISTRY_HPP
