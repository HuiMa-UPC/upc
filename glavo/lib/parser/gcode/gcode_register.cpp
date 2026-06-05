#include "parser_registry.hpp"
#include "gcode_worker.hpp"

namespace {

struct GcodeParserRegistrar {
    GcodeParserRegistrar()
    {
        services::ParserRegistry::instance().register_parser({
            .id = "gcode",
            .extensions = {".gcode", ".nc"},
            .factory = []() -> services::IParserWorker* {
                return &services::GcodeWorker::instance();
            },
        });
    }
};

[[gnu::used]] static GcodeParserRegistrar gcode_parser_registrar;

} // namespace
