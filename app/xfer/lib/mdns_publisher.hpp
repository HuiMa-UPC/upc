#ifndef UPC_XFR_MDNS_PUBLISHER_HPP
#define UPC_XFR_MDNS_PUBLISHER_HPP

#include <string>

namespace upc::xfr {

class MdnsPublisher {
public:
    explicit MdnsPublisher(std::string service_file);
    bool publish(const std::string& model,
                 const std::string& sn,
                 uint16_t port,
                 bool need_pair);
    void unpublish();

private:
    std::string service_file_;
};

} // namespace upc::xfr

#endif
