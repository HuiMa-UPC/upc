#include "parser_registry.hpp"
#include "gcode_worker.hpp"

namespace {

struct GcodeParserRegistrar {
    GcodeParserRegistrar()
    {
        upc::parser::ParserRegistry::instance().register_parser({
            .id = "gcode",
            .extensions = {".gcode", ".nc"},
            .factory = []() -> upc::parser::IParserWorker* {
                return &upc::parser::GcodeWorker::instance();
            },
        });
    }
};

[[gnu::used]] static GcodeParserRegistrar gcode_parser_registrar;

} // namespace
