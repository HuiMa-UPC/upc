#ifndef GLAVO_PARSER_BRIDGE_HPP
#define GLAVO_PARSER_BRIDGE_HPP

namespace upc::glavo {

/** 将 upc::parser 的 RingBuffer 输出绑定到 TcpSender */
void glavo_parser_bind_init();

} // namespace upc::glavo

#endif /* GLAVO_PARSER_BRIDGE_HPP */
