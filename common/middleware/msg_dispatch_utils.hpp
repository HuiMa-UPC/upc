#ifndef MSG_DISPATCH_UTILS_HPP
#define MSG_DISPATCH_UTILS_HPP

#include "msg_runtime.hpp"

#include <vector>

namespace middleware::detail {

template <typename SubT, typename HandlerT>
inline void collect_matching_handlers(const std::vector<SubT>& subscribers,
                                      const Message& msg,
                                      std::vector<HandlerT>& handlers)
{
    for(const auto& sub : subscribers) {
        const bool type_match =
            (sub.type == MsgType{0}) || (sub.type == msg.type);
        const bool target_match =
            (msg.target_id == ModuleId{0}) ||
            (sub.module_id == msg.target_id);
        if(type_match && target_match && static_cast<bool>(sub.handler)) {
            handlers.push_back(sub.handler);
        }
    }
}

} // namespace middleware::detail

#endif
