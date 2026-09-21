/**
 * @file n2_ble_transport.cpp
 * @brief BlueZ system bus GATT Client（扫描 N2_Machine、Connect、Notify 0x1B9A）。
 */

#include "n2_ble_transport.hpp"
#include "rs_log.hpp"

#ifndef UPC_GLAVO_HAS_BLUEZ
#define UPC_GLAVO_HAS_BLUEZ 0
#endif

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#if UPC_GLAVO_HAS_BLUEZ
#include <dbus/dbus.h>
#endif

namespace upc::glavo {
namespace {

constexpr const char* kBluezService = "org.bluez";
constexpr const char* kAdapterPath  = "/org/bluez/hci0";
constexpr const char* kDevName      = "N2_Machine";
constexpr const char* kSvcUuid      = "00004ffc-0000-1000-8000-00805f9b34fb";
constexpr const char* kCharUuid     = "00001b9a-0000-1000-8000-00805f9b34fb";

std::string to_lower_copy(std::string s)
{
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool uuid_match(const std::string& a, const char* b)
{
    return to_lower_copy(a) == to_lower_copy(std::string(b));
}

#if UPC_GLAVO_HAS_BLUEZ

std::string variant_to_string(DBusMessageIter* var)
{
    const int t = dbus_message_iter_get_arg_type(var);
    if (t == DBUS_TYPE_STRING || t == DBUS_TYPE_OBJECT_PATH) {
        const char* p = nullptr;
        dbus_message_iter_get_basic(var, &p);
        return p ? std::string(p) : std::string();
    }
    if (t == DBUS_TYPE_BOOLEAN) {
        dbus_bool_t v = FALSE;
        dbus_message_iter_get_basic(var, &v);
        return v ? "1" : "0";
    }
    return {};
}

bool variant_to_bool(DBusMessageIter* var, bool* out)
{
    if (dbus_message_iter_get_arg_type(var) != DBUS_TYPE_BOOLEAN) {
        return false;
    }
    dbus_bool_t v = FALSE;
    dbus_message_iter_get_basic(var, &v);
    *out = v == TRUE;
    return true;
}

std::string bytes_to_ascii(DBusMessageIter* var)
{
    if (dbus_message_iter_get_arg_type(var) != DBUS_TYPE_ARRAY) {
        return {};
    }
    DBusMessageIter arr;
    dbus_message_iter_recurse(var, &arr);
    std::string s;
    while (dbus_message_iter_get_arg_type(&arr) != DBUS_TYPE_INVALID) {
        if (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_BYTE) {
            unsigned char b = 0;
            dbus_message_iter_get_basic(&arr, &b);
            if (b != 0) {
                s.push_back(static_cast<char>(b));
            }
        }
        dbus_message_iter_next(&arr);
    }
    return s;
}

bool get_prop(DBusConnection* conn, const char* path, const char* iface,
              const char* name, DBusMessage** reply_out)
{
    DBusMessage* msg = dbus_message_new_method_call(
        kBluezService, path, "org.freedesktop.DBus.Properties", "Get");
    if (!msg) {
        return false;
    }
    dbus_message_append_args(msg,
        DBUS_TYPE_STRING, &iface,
        DBUS_TYPE_STRING, &name,
        DBUS_TYPE_INVALID);
    DBusError err;
    dbus_error_init(&err);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn, msg, 4000, &err);
    dbus_message_unref(msg);
    if (!reply) {
        if (dbus_error_is_set(&err)) {
            UPC_LOG_N2_BLE(Debug, "Get %s.%s on %s: %s", iface, name, path, err.message);
            dbus_error_free(&err);
        }
        return false;
    }
    *reply_out = reply;
    return true;
}

bool get_prop_bool(DBusConnection* conn, const char* path, const char* iface,
                   const char* name, bool* out)
{
    DBusMessage* reply = nullptr;
    if (!get_prop(conn, path, iface, name, &reply)) {
        return false;
    }
    DBusMessageIter it, var;
    dbus_message_iter_init(reply, &it);
    if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_VARIANT) {
        dbus_message_unref(reply);
        return false;
    }
    dbus_message_iter_recurse(&it, &var);
    const bool ok = variant_to_bool(&var, out);
    dbus_message_unref(reply);
    return ok;
}

DBusMessage* call_bluez(DBusConnection* conn, const char* path, const char* iface,
                        const char* method, int timeout_ms)
{
    DBusMessage* msg = dbus_message_new_method_call(kBluezService, path, iface, method);
    if (!msg) {
        return nullptr;
    }
    DBusError err;
    dbus_error_init(&err);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(
        conn, msg, timeout_ms, &err);
    dbus_message_unref(msg);
    if (!reply) {
        if (dbus_error_is_set(&err)) {
            UPC_LOG_N2_BLE(Warn, "%s.%s %s: %s", iface, method, path, err.message);
            dbus_error_free(&err);
        }
        return nullptr;
    }
    return reply;
}

bool call_connect_typed(DBusConnection* conn, const char* path, const char* addr_type)
{
    DBusMessage* msg = dbus_message_new_method_call(
        kBluezService, path, "org.bluez.Device1", "Connect");
    if (!msg) {
        return false;
    }
    dbus_message_append_args(msg, DBUS_TYPE_STRING, &addr_type, DBUS_TYPE_INVALID);
    DBusError err;
    dbus_error_init(&err);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn, msg, 15000, &err);
    dbus_message_unref(msg);
    if (reply) {
        dbus_message_unref(reply);
        return true;
    }
    if (dbus_error_is_set(&err)) {
        const bool unknown = std::strstr(err.message, "UnknownMethod") != nullptr
            || std::strstr(err.name, "UnknownMethod") != nullptr;
        UPC_LOG_N2_BLE(Warn, "Connect(s) %s type=%s: %s", path, addr_type, err.message);
        dbus_error_free(&err);
        if (!unknown) {
            return false;
        }
    }
    DBusMessage* reply2 = call_bluez(conn, path, "org.bluez.Device1", "Connect", 15000);
    if (!reply2) {
        return false;
    }
    dbus_message_unref(reply2);
    return true;
}

bool call_disconnect_typed(DBusConnection* conn, const char* path, const char* addr_type)
{
    DBusMessage* msg = dbus_message_new_method_call(
        kBluezService, path, "org.bluez.Device1", "Disconnect");
    if (!msg) {
        return false;
    }
    dbus_message_append_args(msg, DBUS_TYPE_STRING, &addr_type, DBUS_TYPE_INVALID);
    DBusError err;
    dbus_error_init(&err);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn, msg, 5000, &err);
    dbus_message_unref(msg);
    if (reply) {
        dbus_message_unref(reply);
        return true;
    }
    if (dbus_error_is_set(&err)) {
        dbus_error_free(&err);
    }
    DBusMessage* reply2 = call_bluez(conn, path, "org.bluez.Device1", "Disconnect", 5000);
    if (reply2) {
        dbus_message_unref(reply2);
    }
    return true;
}

void skip_container(DBusMessageIter* it)
{
    DBusMessageIter sub;
    dbus_message_iter_recurse(it, &sub);
    while (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_INVALID) {
        dbus_message_iter_next(&sub);
    }
}

void collect_string_array(DBusMessageIter* var, std::vector<std::string>* out)
{
    if (dbus_message_iter_get_arg_type(var) != DBUS_TYPE_ARRAY) {
        return;
    }
    DBusMessageIter arr;
    dbus_message_iter_recurse(var, &arr);
    while (dbus_message_iter_get_arg_type(&arr) != DBUS_TYPE_INVALID) {
        if (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRING) {
            const char* p = nullptr;
            dbus_message_iter_get_basic(&arr, &p);
            if (p) {
                out->emplace_back(p);
            }
        }
        dbus_message_iter_next(&arr);
    }
}

bool device_is_n2(const std::string& name, const std::vector<std::string>& uuids)
{
    if (name == kDevName) {
        return true;
    }
    for (const auto& u : uuids) {
        if (uuid_match(u, kSvcUuid)) {
            return true;
        }
    }
    return false;
}

#endif /* UPC_GLAVO_HAS_BLUEZ */

} // namespace

#if UPC_GLAVO_HAS_BLUEZ

struct N2BleTransport::Impl {
    DBusConnection* conn = nullptr;
    std::string char_path;
    std::mutex mu;
    std::string pending_ascii;
    bool discovering = false;

    static DBusHandlerResult filter(DBusConnection* c, DBusMessage* msg, void* data)
    {
        auto* self = static_cast<Impl*>(data);
        (void)c;
        if (!dbus_message_is_signal(msg, "org.freedesktop.DBus.Properties",
                                    "PropertiesChanged")) {
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        }
        const char* path = dbus_message_get_path(msg);
        if (!path || self->char_path.empty() || self->char_path != path) {
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        }
        DBusMessageIter it;
        if (!dbus_message_iter_init(msg, &it)) {
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_STRING) {
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        const char* iface = nullptr;
        dbus_message_iter_get_basic(&it, &iface);
        if (!iface || std::strcmp(iface, "org.bluez.GattCharacteristic1") != 0) {
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        dbus_message_iter_next(&it);
        if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY) {
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        DBusMessageIter arr;
        dbus_message_iter_recurse(&it, &arr);
        while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter ent, var;
            dbus_message_iter_recurse(&arr, &ent);
            if (dbus_message_iter_get_arg_type(&ent) != DBUS_TYPE_STRING) {
                dbus_message_iter_next(&arr);
                continue;
            }
            const char* key = nullptr;
            dbus_message_iter_get_basic(&ent, &key);
            dbus_message_iter_next(&ent);
            if (key && std::strcmp(key, "Value") == 0
                && dbus_message_iter_get_arg_type(&ent) == DBUS_TYPE_VARIANT) {
                dbus_message_iter_recurse(&ent, &var);
                const std::string ascii = bytes_to_ascii(&var);
                if (!ascii.empty()) {
                    std::lock_guard<std::mutex> lock(self->mu);
                    self->pending_ascii = ascii;
                }
            }
            dbus_message_iter_next(&arr);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    bool get_managed(DBusMessage** reply_out)
    {
        DBusMessage* msg = dbus_message_new_method_call(
            kBluezService, "/", "org.freedesktop.DBus.ObjectManager",
            "GetManagedObjects");
        if (!msg) {
            return false;
        }
        DBusError err;
        dbus_error_init(&err);
        DBusMessage* reply = dbus_connection_send_with_reply_and_block(conn, msg, 8000, &err);
        dbus_message_unref(msg);
        if (!reply) {
            if (dbus_error_is_set(&err)) {
                UPC_LOG_N2_BLE(Warn, "GetManagedObjects: %s", err.message);
                dbus_error_free(&err);
            }
            return false;
        }
        *reply_out = reply;
        return true;
    }

    bool walk_devices(N2BleTransport::Device* want_n2,
                      const std::string* want_char_under,
                      std::string* char_out)
    {
        DBusMessage* reply = nullptr;
        if (!get_managed(&reply)) {
            return false;
        }
        DBusMessageIter root, arr;
        if (!dbus_message_iter_init(reply, &root)
            || dbus_message_iter_get_arg_type(&root) != DBUS_TYPE_ARRAY) {
            dbus_message_unref(reply);
            return false;
        }
        dbus_message_iter_recurse(&root, &arr);
        bool found = false;
        while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter ent, ifaces;
            dbus_message_iter_recurse(&arr, &ent);
            if (dbus_message_iter_get_arg_type(&ent) != DBUS_TYPE_OBJECT_PATH) {
                dbus_message_iter_next(&arr);
                continue;
            }
            const char* opath = nullptr;
            dbus_message_iter_get_basic(&ent, &opath);
            dbus_message_iter_next(&ent);
            if (dbus_message_iter_get_arg_type(&ent) != DBUS_TYPE_ARRAY) {
                dbus_message_iter_next(&arr);
                continue;
            }
            dbus_message_iter_recurse(&ent, &ifaces);

            std::string name, address, addr_type, uuid;
            std::vector<std::string> uuids;
            bool has_dev = false;
            bool has_char = false;

            while (dbus_message_iter_get_arg_type(&ifaces) == DBUS_TYPE_DICT_ENTRY) {
                DBusMessageIter ient, props;
                dbus_message_iter_recurse(&ifaces, &ient);
                if (dbus_message_iter_get_arg_type(&ient) != DBUS_TYPE_STRING) {
                    dbus_message_iter_next(&ifaces);
                    continue;
                }
                const char* iface = nullptr;
                dbus_message_iter_get_basic(&ient, &iface);
                dbus_message_iter_next(&ient);
                const bool is_dev = iface && std::strcmp(iface, "org.bluez.Device1") == 0;
                const bool is_ch = iface && std::strcmp(iface, "org.bluez.GattCharacteristic1") == 0;
                if (is_dev) {
                    has_dev = true;
                }
                if (is_ch) {
                    has_char = true;
                }
                if ((is_dev || is_ch)
                    && dbus_message_iter_get_arg_type(&ient) == DBUS_TYPE_ARRAY) {
                    dbus_message_iter_recurse(&ient, &props);
                    while (dbus_message_iter_get_arg_type(&props) == DBUS_TYPE_DICT_ENTRY) {
                        DBusMessageIter pent, var;
                        dbus_message_iter_recurse(&props, &pent);
                        if (dbus_message_iter_get_arg_type(&pent) != DBUS_TYPE_STRING) {
                            dbus_message_iter_next(&props);
                            continue;
                        }
                        const char* key = nullptr;
                        dbus_message_iter_get_basic(&pent, &key);
                        dbus_message_iter_next(&pent);
                        if (dbus_message_iter_get_arg_type(&pent) != DBUS_TYPE_VARIANT) {
                            dbus_message_iter_next(&props);
                            continue;
                        }
                        dbus_message_iter_recurse(&pent, &var);
                        if (key && is_dev) {
                            if (std::strcmp(key, "Name") == 0) {
                                name = variant_to_string(&var);
                            } else if (std::strcmp(key, "Address") == 0) {
                                address = variant_to_string(&var);
                            } else if (std::strcmp(key, "AddressType") == 0) {
                                addr_type = variant_to_string(&var);
                            } else if (std::strcmp(key, "UUIDs") == 0) {
                                collect_string_array(&var, &uuids);
                            }
                        }
                        if (key && is_ch && std::strcmp(key, "UUID") == 0) {
                            uuid = variant_to_string(&var);
                        }
                        dbus_message_iter_next(&props);
                    }
                } else if (dbus_message_iter_get_arg_type(&ient) == DBUS_TYPE_ARRAY) {
                    skip_container(&ient);
                }
                dbus_message_iter_next(&ifaces);
            }

            if (want_n2 && has_dev && opath && device_is_n2(name, uuids)) {
                want_n2->path = opath;
                want_n2->address = address;
                want_n2->address_type = addr_type.empty() ? "public" : addr_type;
                want_n2->name = name;
                found = true;
            }
            if (want_char_under && char_out && has_char && opath
                && uuid_match(uuid, kCharUuid)
                && std::strstr(opath, want_char_under->c_str()) == opath) {
                *char_out = opath;
                found = true;
            }
            dbus_message_iter_next(&arr);
        }
        dbus_message_unref(reply);
        return found;
    }
};

bool N2BleTransport::compiled_with_bluez()
{
    return true;
}

N2BleTransport::N2BleTransport() = default;

N2BleTransport::~N2BleTransport()
{
    close();
}

bool N2BleTransport::open()
{
    if (impl_ && impl_->conn) {
        return true;
    }
    dbus_threads_init_default();
    DBusError err;
    dbus_error_init(&err);
    DBusConnection* conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
    if (!conn) {
        UPC_LOG_N2_BLE(Warn, "dbus system bus failed: %s",
                       dbus_error_is_set(&err) ? err.message : "?");
        if (dbus_error_is_set(&err)) {
            dbus_error_free(&err);
        }
        return false;
    }
    impl_ = new Impl();
    impl_->conn = conn;
    dbus_bus_add_match(conn,
        "type='signal',sender='org.bluez',"
        "interface='org.freedesktop.DBus.Properties',"
        "member='PropertiesChanged'",
        nullptr);
    dbus_connection_add_filter(conn, Impl::filter, impl_, nullptr);
    dbus_connection_flush(conn);
    dbus_bool_t powered = TRUE;
    DBusMessage* setp = dbus_message_new_method_call(
        kBluezService, kAdapterPath, "org.freedesktop.DBus.Properties", "Set");
    if (setp) {
        const char* iface = "org.bluez.Adapter1";
        const char* name = "Powered";
        DBusMessageIter it, var;
        dbus_message_iter_init_append(setp, &it);
        dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &iface);
        dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &name);
        dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, "b", &var);
        dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &powered);
        dbus_message_iter_close_container(&it, &var);
        DBusError e2;
        dbus_error_init(&e2);
        DBusMessage* r = dbus_connection_send_with_reply_and_block(conn, setp, 3000, &e2);
        dbus_message_unref(setp);
        if (r) {
            dbus_message_unref(r);
        } else if (dbus_error_is_set(&e2)) {
            dbus_error_free(&e2);
        }
    }
    UPC_LOG_N2_BLE(Info, "bluez bus open adapter=%s", kAdapterPath);
    return true;
}

void N2BleTransport::close()
{
    if (!impl_) {
        return;
    }
    stop_discovery();
    if (impl_->conn) {
        dbus_connection_remove_filter(impl_->conn, Impl::filter, impl_);
        dbus_connection_unref(impl_->conn);
    }
    delete impl_;
    impl_ = nullptr;
}

bool N2BleTransport::start_discovery()
{
    if (!impl_ || !impl_->conn) {
        return false;
    }
    DBusMessage* msg = dbus_message_new_method_call(
        kBluezService, kAdapterPath, "org.bluez.Adapter1", "SetDiscoveryFilter");
    if (msg) {
        DBusMessageIter it, arr, ent, var;
        dbus_message_iter_init_append(msg, &it);
        dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &arr);
        const char* tr_key = "Transport";
        const char* tr_val = "le";
        dbus_message_iter_open_container(&arr, DBUS_TYPE_DICT_ENTRY, nullptr, &ent);
        dbus_message_iter_append_basic(&ent, DBUS_TYPE_STRING, &tr_key);
        dbus_message_iter_open_container(&ent, DBUS_TYPE_VARIANT, "s", &var);
        dbus_message_iter_append_basic(&var, DBUS_TYPE_STRING, &tr_val);
        dbus_message_iter_close_container(&ent, &var);
        dbus_message_iter_close_container(&arr, &ent);
        dbus_message_iter_close_container(&it, &arr);
        DBusError err;
        dbus_error_init(&err);
        DBusMessage* r = dbus_connection_send_with_reply_and_block(impl_->conn, msg, 4000, &err);
        dbus_message_unref(msg);
        if (r) {
            dbus_message_unref(r);
        } else if (dbus_error_is_set(&err)) {
            UPC_LOG_N2_BLE(Debug, "SetDiscoveryFilter: %s", err.message);
            dbus_error_free(&err);
        }
    }
    DBusMessage* reply = call_bluez(impl_->conn, kAdapterPath, "org.bluez.Adapter1",
                                    "StartDiscovery", 4000);
    if (!reply) {
        return false;
    }
    dbus_message_unref(reply);
    impl_->discovering = true;
    UPC_LOG_N2_BLE(Info, "StartDiscovery");
    return true;
}

void N2BleTransport::stop_discovery()
{
    if (!impl_ || !impl_->conn || !impl_->discovering) {
        return;
    }
    DBusMessage* reply = call_bluez(impl_->conn, kAdapterPath, "org.bluez.Adapter1",
                                    "StopDiscovery", 4000);
    if (reply) {
        dbus_message_unref(reply);
    }
    impl_->discovering = false;
}

void N2BleTransport::poll(int timeout_ms)
{
    if (!impl_ || !impl_->conn) {
        return;
    }
    dbus_connection_read_write_dispatch(impl_->conn, timeout_ms < 0 ? 0 : timeout_ms);
}

bool N2BleTransport::find_n2_machine(Device* out)
{
    if (!impl_ || !out) {
        return false;
    }
    return impl_->walk_devices(out, nullptr, nullptr);
}

bool N2BleTransport::connect_device(const Device& dev)
{
    if (!impl_ || dev.path.empty()) {
        return false;
    }
    const char* t = dev.address_type.empty() ? "public" : dev.address_type.c_str();
    UPC_LOG_N2_BLE(Info, "Connect %s %s type=%s", dev.name.c_str(),
                   dev.address.c_str(), t);
    return call_connect_typed(impl_->conn, dev.path.c_str(), t);
}

bool N2BleTransport::disconnect_device(const Device& dev)
{
    if (!impl_ || dev.path.empty()) {
        return false;
    }
    const char* t = dev.address_type.empty() ? "public" : dev.address_type.c_str();
    return call_disconnect_typed(impl_->conn, dev.path.c_str(), t);
}

bool N2BleTransport::device_connected(const std::string& path)
{
    if (!impl_ || path.empty()) {
        return false;
    }
    bool v = false;
    if (!get_prop_bool(impl_->conn, path.c_str(), "org.bluez.Device1", "Connected", &v)) {
        return false;
    }
    return v;
}

bool N2BleTransport::services_resolved(const std::string& path)
{
    if (!impl_ || path.empty()) {
        return false;
    }
    bool v = false;
    if (!get_prop_bool(impl_->conn, path.c_str(), "org.bluez.Device1",
                       "ServicesResolved", &v)) {
        return false;
    }
    return v;
}

bool N2BleTransport::find_n2_characteristic(const std::string& device_path,
                                            std::string* char_path)
{
    if (!impl_ || !char_path) {
        return false;
    }
    return impl_->walk_devices(nullptr, &device_path, char_path);
}

bool N2BleTransport::start_notify(const std::string& char_path)
{
    if (!impl_) {
        return false;
    }
    impl_->char_path = char_path;
    DBusMessage* reply = call_bluez(impl_->conn, char_path.c_str(),
                                    "org.bluez.GattCharacteristic1", "StartNotify", 5000);
    if (!reply) {
        return false;
    }
    dbus_message_unref(reply);
    UPC_LOG_N2_BLE(Info, "StartNotify %s", char_path.c_str());
    return true;
}

bool N2BleTransport::stop_notify(const std::string& char_path)
{
    if (!impl_ || char_path.empty()) {
        return false;
    }
    DBusMessage* reply = call_bluez(impl_->conn, char_path.c_str(),
                                    "org.bluez.GattCharacteristic1", "StopNotify", 3000);
    if (reply) {
        dbus_message_unref(reply);
    }
    impl_->char_path.clear();
    return true;
}

bool N2BleTransport::take_notify_ascii(std::string* ascii)
{
    if (!impl_ || !ascii) {
        return false;
    }
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (impl_->pending_ascii.empty()) {
        return false;
    }
    *ascii = impl_->pending_ascii;
    impl_->pending_ascii.clear();
    return true;
}

bool N2BleTransport::read_ascii(const std::string& char_path, std::string* ascii)
{
    if (!impl_ || !ascii) {
        return false;
    }
    DBusMessage* msg = dbus_message_new_method_call(
        kBluezService, char_path.c_str(), "org.bluez.GattCharacteristic1", "ReadValue");
    if (!msg) {
        return false;
    }
    DBusMessageIter it, arr;
    dbus_message_iter_init_append(msg, &it);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &arr);
    dbus_message_iter_close_container(&it, &arr);
    DBusError err;
    dbus_error_init(&err);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(
        impl_->conn, msg, 4000, &err);
    dbus_message_unref(msg);
    if (!reply) {
        if (dbus_error_is_set(&err)) {
            dbus_error_free(&err);
        }
        return false;
    }
    DBusMessageIter rit;
    if (!dbus_message_iter_init(reply, &rit)
        || dbus_message_iter_get_arg_type(&rit) != DBUS_TYPE_ARRAY) {
        dbus_message_unref(reply);
        return false;
    }
    *ascii = bytes_to_ascii(&rit);
    dbus_message_unref(reply);
    return !ascii->empty();
}

#else /* !UPC_GLAVO_HAS_BLUEZ */

struct N2BleTransport::Impl {};

bool N2BleTransport::compiled_with_bluez() { return false; }
N2BleTransport::N2BleTransport() = default;
N2BleTransport::~N2BleTransport() { close(); }
bool N2BleTransport::open() { return false; }
void N2BleTransport::close() {}
bool N2BleTransport::start_discovery() { return false; }
void N2BleTransport::stop_discovery() {}
void N2BleTransport::poll(int) {}
bool N2BleTransport::find_n2_machine(Device*) { return false; }
bool N2BleTransport::connect_device(const Device&) { return false; }
bool N2BleTransport::disconnect_device(const Device&) { return false; }
bool N2BleTransport::device_connected(const std::string&) { return false; }
bool N2BleTransport::services_resolved(const std::string&) { return false; }
bool N2BleTransport::find_n2_characteristic(const std::string&, std::string*) { return false; }
bool N2BleTransport::start_notify(const std::string&) { return false; }
bool N2BleTransport::stop_notify(const std::string&) { return false; }
bool N2BleTransport::take_notify_ascii(std::string*) { return false; }
bool N2BleTransport::read_ascii(const std::string&, std::string*) { return false; }

#endif

} // namespace upc::glavo
