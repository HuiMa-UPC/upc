#include "glavo_parser_bridge.hpp"
#include "parser_manager.hpp"
#include "tcp_sender.hpp"

namespace upc::glavo {

void glavo_parser_bind_init()
{
    upc::parser::ParserManager::instance().set_ring_buffer_sink(
        [](upc::parser::RingBuffer* rb) {
            TcpSender::instance().bind_ring_buffer(rb);
        });
}

} // namespace upc::glavo
