#include "parser_manager.hpp"
#include "parser_registry.hpp"

namespace upc::parser {

ParserManager& ParserManager::instance()
{
    static ParserManager mgr;
    return mgr;
}

void ParserManager::set_ring_buffer_sink(RingBufferSink sink)
{
    sink_ = std::move(sink);
}

void ParserManager::notify_sink()
{
    if(sink_ && active_) {
        sink_(&active_->ring_buffer());
    }
}

int ParserManager::open_file(const std::string& path)
{
    IParserWorker* worker = ParserRegistry::instance().resolve(path);
    if(!worker) {
        return -1;
    }

    if(active_ != worker) {
        if(active_) {
            active_->stop_parse();
        }
        active_ = worker;
        notify_sink();
    }

    return worker->open_file(path);
}

void ParserManager::start_parse()
{
    if(active_) {
        active_->start_parse();
    }
}

void ParserManager::stop_parse()
{
    if(active_) {
        active_->stop_parse();
    }
}

} // namespace upc::parser
