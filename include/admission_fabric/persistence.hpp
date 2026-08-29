#pragma once

// Admission Fabric - durable state persistence.
//
// Coordinator state is persisted with an explicit, versioned, checksummed
// encoding. Writes are atomic: temporary file -> write -> flush -> close ->
// atomic replace. Loads reject truncation, corruption, unknown versions,
// impossible counts, duplicate identities and inconsistent reservation
// accounting. Recovery never invents capacity or success.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include "admission_fabric/engine.hpp"
#include "admission_fabric/error.hpp"
#include "admission_fabric/serialize.hpp"

namespace admission_fabric {

struct PersistenceConfig {
    std::filesystem::path path;
    std::uint32_t magic{0xAF1FA000u};
    std::uint32_t version{1u};
    std::filesystem::path temp_path() const { return std::filesystem::path(path.string() + ".tmp"); }
};

class StateStore {
public:
    // Build the framed, checksummed payload for an engine's current state.
    [[nodiscard]] Status build_payload(const AdmissionFabric& f, std::vector<std::uint8_t>& out, const PersistenceConfig& cfg) const;

    // Apply a validated payload to an engine (recovery). Returns the first
    // corruption/inconsistency error.
    [[nodiscard]] Status apply_payload(AdmissionFabric& f, const std::vector<std::uint8_t>& payload, const PersistenceConfig& cfg) const;

    // Atomic write: temp -> write -> flush -> close -> atomic replace.
    [[nodiscard]] Status save(const PersistenceConfig& cfg, const AdmissionFabric& f) const;

    // Read + validate the payload from disk.
    [[nodiscard]] Result<std::vector<std::uint8_t>> load_payload(const PersistenceConfig& cfg) const;

    // Convenience: load + restore an engine.
    [[nodiscard]] Status load(const PersistenceConfig& cfg, AdmissionFabric& f) const;

    // Sum of bytes for a payload (for benchmark sizing).
    [[nodiscard]] static std::size_t payload_size(const AdmissionFabric& f, const PersistenceConfig& cfg);
};

} // namespace admission_fabric
