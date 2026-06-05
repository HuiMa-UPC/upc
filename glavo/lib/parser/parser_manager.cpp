#include "parser_manager.hpp"
#include "parser_registry.hpp"
#include "tcp_sender.hpp"

namespace services {

ParserManager& ParserManager::instance()
{
    static ParserManager mgr;
    return mgr;
}

void ParserManager::bind_tcp_output()
{
    IParserWorker* worker = ParserRegistry::instance().any_worker();
    if(!worker) {
        return;
    }

    active_ = worker;
    TcpSender::instance().bind_ring_buffer(&worker->ring_buffer());
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
        TcpSender::instance().bind_ring_buffer(&worker->ring_buffer());
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

} // namespace services
