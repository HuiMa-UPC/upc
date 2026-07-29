#pragma once

#include "open_vector_format.pb.h"

#include <string>

namespace open_vector_format {

/// Write an in-memory Job as an official streaming .ovf (magic LVF! + LUTs).
/// Compatible with OVFSliceViewer / OpenVectorFormatTools ReaderWriter.
/// @return true on success; on failure fills *errorOut when non-null.
bool writeOfficialOvfFile(const Job& job,
                          const std::string& path,
                          std::string* errorOut = nullptr);

/// Read an official streaming .ovf (or a raw Job protobuf dump) into Job.
bool readOfficialOvfFile(const std::string& path,
                         Job& jobOut,
                         std::string* errorOut = nullptr);

/// Convert a file to official streaming .ovf:
/// - already starts with LVF! → copy as-is (or rewrite in place via temp);
/// - otherwise try ParseFrom as a raw Job protobuf dump, then writeOfficialOvfFile.
bool convertToOfficialOvfFile(const std::string& inputPath,
                              const std::string& outputPath,
                              std::string* errorOut = nullptr);

}  // namespace open_vector_format
