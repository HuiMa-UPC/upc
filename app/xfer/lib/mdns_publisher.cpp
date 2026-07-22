#include "mdns_publisher.hpp"

#include "rs_log.hpp"

#include <filesystem>
#include <fstream>

namespace upc::xfr {

MdnsPublisher::MdnsPublisher(std::string service_file)
    : service_file_(std::move(service_file))
{
}

bool MdnsPublisher::publish(const std::string& model,
                            const std::string& sn,
                            uint16_t port,
                            bool need_pair)
{
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(service_file_).parent_path(), ec);

    std::ofstream ofs(service_file_, std::ios::trunc);
    if (!ofs.good()) {
        UPC_LOG_XFR_SERVICE(Warn, "write mdns service failed: %s", service_file_.c_str());
        return false;
    }
    ofs << "<service-group>\n";
    ofs << "  <name replace-wildcards=\"yes\">Snowpod-" << sn << "</name>\n";
    ofs << "  <service>\n";
    ofs << "    <type>_snowpod._tcp</type>\n";
    ofs << "    <port>" << port << "</port>\n";
    ofs << "    <txt-record>ver=1</txt-record>\n";
    ofs << "    <txt-record>model=" << model << "</txt-record>\n";
    ofs << "    <txt-record>sn=" << sn << "</txt-record>\n";
    ofs << "    <txt-record>needpair=" << (need_pair ? "1" : "0") << "</txt-record>\n";
    ofs << "  </service>\n";
    ofs << "</service-group>\n";
    UPC_LOG_XFR_SERVICE(Info, "mdns service published: %s", service_file_.c_str());
    return true;
}

void MdnsPublisher::unpublish()
{
    std::error_code ec;
    std::filesystem::remove(service_file_, ec);
}

} // namespace upc::xfr
