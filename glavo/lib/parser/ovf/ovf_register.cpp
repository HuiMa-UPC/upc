#include "parser_registry.hpp"
#include "ovf_worker.hpp"

namespace {

struct OvfParserRegistrar {
    OvfParserRegistrar()
    {
        services::ParserRegistry::instance().register_parser({
            .id = "ovf",
            .extensions = {".ovf"},
            .factory = []() -> services::IParserWorker* {
                return &services::OvfWorker::instance();
            },
        });
    }
};

[[gnu::used]] static OvfParserRegistrar ovf_parser_registrar;

} // namespace
