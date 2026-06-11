#include "parser_registry.hpp"

#include <algorithm>
#include <cctype>

namespace upc::parser {

namespace {

bool extension_matches(const std::string& filename, const std::string& ext_lower)
{
    if(ext_lower.empty() || filename.size() < ext_lower.size()) {
        return false;
    }
    const std::string file_ext = file_extension_lower(filename);
    return file_ext == ext_lower;
}

} // namespace

std::string file_extension_lower(const std::string& filename)
{
    const auto dot = filename.find_last_of('.');
    if(dot == std::string::npos || dot + 1 >= filename.size()) {
        return {};
    }

    std::string ext = filename.substr(dot);
    for(char& ch : ext) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return ext;
}

ParserRegistry& ParserRegistry::instance()
{
    static ParserRegistry registry;
    return registry;
}

void ParserRegistry::register_parser(ParserRegistration reg)
{
    if(!reg.id || reg.extensions.empty() || !reg.factory) {
        return;
    }

    Entry entry;
    entry.id = reg.id;
    entry.factory = std::move(reg.factory);
    entry.extensions.reserve(reg.extensions.size());

    for(std::string ext : reg.extensions) {
        if(ext.empty()) {
            continue;
        }
        if(ext[0] != '.') {
            ext.insert(ext.begin(), '.');
        }
        for(char& ch : ext) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        entry.extensions.push_back(std::move(ext));
    }

    if(entry.extensions.empty()) {
        return;
    }

    entries_.push_back(std::move(entry));
}

IParserWorker* ParserRegistry::resolve(const std::string& filepath) const
{
    const std::string ext = file_extension_lower(filepath);
    if(ext.empty()) {
        return nullptr;
    }

    for(const Entry& entry : entries_) {
        for(const std::string& reg_ext : entry.extensions) {
            if(ext == reg_ext) {
                return entry.factory ? entry.factory() : nullptr;
            }
        }
    }
    return nullptr;
}

bool ParserRegistry::match_filename(const std::string& filename) const
{
    for(const Entry& entry : entries_) {
        for(const std::string& reg_ext : entry.extensions) {
            if(extension_matches(filename, reg_ext)) {
                return true;
            }
        }
    }
    return false;
}

std::vector<std::string> ParserRegistry::all_extensions() const
{
    std::vector<std::string> out;
    for(const Entry& entry : entries_) {
        for(const std::string& ext : entry.extensions) {
            if(std::find(out.begin(), out.end(), ext) == out.end()) {
                out.push_back(ext);
            }
        }
    }
    return out;
}

IParserWorker* ParserRegistry::any_worker() const
{
    for(const Entry& entry : entries_) {
        if(entry.factory) {
            return entry.factory();
        }
    }
    return nullptr;
}

} // namespace upc::parser
