#ifndef UPC_CABIN_PROFILE_HPP
#define UPC_CABIN_PROFILE_HPP

#include <string>

namespace upc::visual {
class CabinDetector;

// Explicit override > existing station file > versioned built-in profile.
// A missing/invalid override or an invalid existing station file never silently
// falls back to a different profile. No file is written or installed at runtime.
bool load_cabin_profile(CabinDetector& detector, bool door,
                        const char* override_path, const std::string& station_path,
                        std::string& source, std::string& error);
} // namespace upc::visual
#endif
