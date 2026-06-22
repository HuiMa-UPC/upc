#include "parser_registry.hpp"
#include "ovf_worker.hpp"

namespace {

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

[[gnu::used]] static OvfParserRegistrar ovf_parser_registrar;

} // namespace
