/**
 * OVF 解析器静态注册入口。
 *
 * 通过全局 OvfParserRegistrar 在进程加载期将 OvfWorker 注册到 ParserRegistry，
 * 关联后缀 .ovf。链接本目标时必须使用 --whole-archive，否则链接器可能
 * 剥离未直接引用的 ovf_parser_registrar 符号，导致注册失效。
 */
#include "parser_registry.hpp"
#include "ovf_worker.hpp"

namespace {

/** 构造时向 ParserRegistry 注册 ovf 解析器 */
struct OvfParserRegistrar {
    OvfParserRegistrar()
    {
        upc::parser::ParserRegistry::instance().register_parser({
            .id = "ovf",
            .extensions = {".ovf"},
            .factory = []() -> upc::parser::IParserWorker* {
                return &upc::parser::OvfWorker::instance();
            },
        });
    }
};

/** gnu::used 防止 LTO/链接器剔除静态初始化对象 */
[[gnu::used]] static OvfParserRegistrar ovf_parser_registrar;

} // namespace
