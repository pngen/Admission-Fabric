#pragma once

// Admission Fabric - version identifiers.
#define ADMISSION_FABRIC_VERSION_MAJOR 1
#define ADMISSION_FABRIC_VERSION_MINOR 0
#define ADMISSION_FABRIC_VERSION_PATCH 0
#define ADMISSION_FABRIC_VERSION_STRING "1.0.0"

namespace admission_fabric {
// Version of the runtime. Protocol/on-disk formats are versioned independently;
// this is the library version.
struct Version { int major; int minor; int patch; };
inline constexpr Version kVersion{1, 0, 0};
inline constexpr const char* version_string() { return ADMISSION_FABRIC_VERSION_STRING; }
// Returns the runtime version string; used by CLI/benchmark introspection.
const char* runtime_version();
} // namespace admission_fabric
