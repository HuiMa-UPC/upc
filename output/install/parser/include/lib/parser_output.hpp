#ifndef UPC_PARSER_OUTPUT_HPP
#define UPC_PARSER_OUTPUT_HPP

#include "slice_ring_buffer.hpp"

#include <functional>

namespace upc::parser {

/** 解析器 active 切换或 open_file 后，通知下游绑定 RingBuffer */
using RingBufferSink = std::function<void(RingBuffer*)>;

} // namespace upc::parser

#endif /* UPC_PARSER_OUTPUT_HPP */
