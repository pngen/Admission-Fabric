#pragma once

// Admission Fabric - multidimensional resource model.
//
// Admission is never a single-scalar decision. A request that fits compute
// but not accelerator memory is not feasible; a request that fits memory but
// cannot satisfy its latency obligation is not admissible; a request that
// fits local resources but needs an unavailable capability must be rejected.
// This header defines the bounded resource dimension set, the vector that
// carries per-resource quantities, the versioned snapshot, and the accounting
// container that enforces the coherence invariants.

#include <array>
#include <cstdint>
#include <set>
#include <string>
#include <vector>
#include <algorithm>
#include "admission_fabric/checked.hpp"
#include "admission_fabric/error.hpp"
#include "admission_fabric/id.hpp"
#include "admission_fabric/units.hpp"

namespace admission_fabric {

// The bounded resource dimension set. Adding a dimension here is a deliberate
// change to the accounting model; dimensions are never dynamic.
enum class ResourceId : std::uint8_t {
    AcceleratorMemory = 0,   // device (accelerator) memory, bytes
    HostMemory = 1,          // pageable host memory, bytes
    PinnedHostMemory = 2,    // pinned (non-pageable) host memory, bytes
    PersistentCache = 3,     // persistent storage / cache capacity, bytes
    AcceleratorCompute = 4,  // abstract accelerator compute units
    CpuSupport = 5,          // host CPU support load (seconds of compute)
    TransferBandwidth = 6,   // transfer bytes allowance
    ExecutionSlots = 7,      // concurrent execution slots (count)
    SequenceSlots = 8,       // concurrent sequence slots (count)
    ModelResidency = 9,      // number of resident model slots (count)
    KvCapacity = 10,         // KV cache bytes
    TensorState = 11,        // intermediate / tensor-state bytes
    Count = 12               // sentinel: number of dimensions
};

constexpr std::uint8_t kResourceCount = static_cast<std::uint8_t>(ResourceId::Count);

// Human-readable name for a resource dimension (for explanations / CLI).
inline const char* resource_name(ResourceId id) {
    switch (id) {
        case ResourceId::AcceleratorMemory: return "accelerator_memory";
        case ResourceId::HostMemory: return "host_memory";
        case ResourceId::PinnedHostMemory: return "pinned_host_memory";
        case ResourceId::PersistentCache: return "persistent_cache";
        case ResourceId::AcceleratorCompute: return "accelerator_compute";
        case ResourceId::CpuSupport: return "cpu_support";
        case ResourceId::TransferBandwidth: return "transfer_bandwidth";
        case ResourceId::ExecutionSlots: return "execution_slots";
        case ResourceId::SequenceSlots: return "sequence_slots";
        case ResourceId::ModelResidency: return "model_residency";
        case ResourceId::KvCapacity: return "kv_capacity";
        case ResourceId::TensorState: return "tensor_state";
        case ResourceId::Count: return "count";
    }
    return "unknown";
}

inline bool resource_is_bytes(ResourceId id) {
    switch (id) {
        case ResourceId::AcceleratorMemory:
        case ResourceId::HostMemory:
        case ResourceId::PinnedHostMemory:
        case ResourceId::PersistentCache:
        case ResourceId::KvCapacity:
        case ResourceId::TensorState:
            return true;
        default:
            return false;
    }
}

// Provenance of a resource value. Never treat stale or predicted information
// as current without policy approval.
enum class Provenance : std::uint8_t {
    Measured = 1,
    Configured = 2,
    Derived = 3,
    Predicted = 4,
    Stale = 5,
    Unavailable = 6
};

inline const char* provenance_name(Provenance p) {
    switch (p) {
        case Provenance::Measured: return "measured";
        case Provenance::Configured: return "configured";
        case Provenance::Derived: return "derived";
        case Provenance::Predicted: return "predicted";
        case Provenance::Stale: return "stale";
        case Provenance::Unavailable: return "unavailable";
    }
    return "unknown";
}

// Operational pressure level per resource dimension.
enum class PressureLevel : std::uint8_t {
    None = 0,
    Low = 1,
    Normal = 2,
    High = 3,
    Critical = 4
};

inline const char* pressure_name(PressureLevel p) {
    switch (p) {
        case PressureLevel::None: return "none";
        case PressureLevel::Low: return "low";
        case PressureLevel::Normal: return "normal";
        case PressureLevel::High: return "high";
        case PressureLevel::Critical: return "critical";
    }
    return "unknown";
}

// A fixed-size vector of per-resource quantities.
class ResourceVector {
public:
    using value_type = std::uint64_t;

    constexpr ResourceVector() { values_.fill(0); }

    [[nodiscard]] constexpr value_type get(ResourceId id) const {
        return values_[static_cast<std::size_t>(id)];
    }
    constexpr void set(ResourceId id, value_type v) {
        values_[static_cast<std::size_t>(id)] = v;
    }

    [[nodiscard]] const std::array<value_type, kResourceCount>& data() const { return values_; }
    [[nodiscard]] std::array<value_type, kResourceCount>& data() { return values_; }

    [[nodiscard]] bool is_zero() const {
        for (auto v : values_) if (v != 0) return false;
        return true;
    }
    [[nodiscard]] bool any_nonzero() const { return !is_zero(); }

    // Checked add: returns false on overflow or if a dimension would wrap.
    [[nodiscard]] bool try_add(const ResourceVector& other, ResourceVector& out) const {
        for (std::size_t i = 0; i < kResourceCount; ++i) {
            std::uint64_t r = 0;
            if (!checked_add(values_[i], other.values_[i], r)) return false;
            out.values_[i] = r;
        }
        return true;
    }
    // Checked subtract: returns false if any dimension underflows.
    [[nodiscard]] bool try_sub(const ResourceVector& other, ResourceVector& out) const {
        for (std::size_t i = 0; i < kResourceCount; ++i) {
            std::uint64_t r = 0;
            if (!checked_sub(values_[i], other.values_[i], r)) return false;
            out.values_[i] = r;
        }
        return true;
    }
    // Checked absolute subtraction result (for error magnitude when release
    // does not fit) — returns the vector that is the positive difference.
    [[nodiscard]] ResourceVector diff_of(const ResourceVector& other) const {
        ResourceVector r;
        for (std::size_t i = 0; i < kResourceCount; ++i) {
            r.values_[i] = values_[i] >= other.values_[i] ? values_[i] - other.values_[i]
                                                          : other.values_[i] - values_[i];
        }
        return r;
    }

    // True if every dimension of 'this' is <= the corresponding dimension of
    // 'limit'.
    [[nodiscard]] bool fits_in(const ResourceVector& limit) const {
        for (std::size_t i = 0; i < kResourceCount; ++i) {
            if (values_[i] > limit.values_[i]) return false;
        }
        return true;
    }

    // Saturating componentwise add (for derived reporting).
    [[nodiscard]] ResourceVector sat_add(const ResourceVector& other) const {
        ResourceVector r;
        for (std::size_t i = 0; i < kResourceCount; ++i)
            r.values_[i] = admission_fabric::sat_add(values_[i], other.values_[i]);
        return r;
    }
    // Saturating componentwise subtract (for derived headroom).
    [[nodiscard]] ResourceVector sat_sub(const ResourceVector& other) const {
        ResourceVector r;
        for (std::size_t i = 0; i < kResourceCount; ++i)
            r.values_[i] = admission_fabric::sat_sub(values_[i], other.values_[i]);
        return r;
    }

    // Componentwise maximum / minimum.
    [[nodiscard]] ResourceVector max_with(const ResourceVector& other) const {
        ResourceVector r;
        for (std::size_t i = 0; i < kResourceCount; ++i)
            r.values_[i] = (std::max)(values_[i], other.values_[i]);
        return r;
    }
    [[nodiscard]] ResourceVector min_with(const ResourceVector& other) const {
        ResourceVector r;
        for (std::size_t i = 0; i < kResourceCount; ++i)
            r.values_[i] = (std::min)(values_[i], other.values_[i]);
        return r;
    }

    friend bool operator==(const ResourceVector& a, const ResourceVector& b) { return a.values_ == b.values_; }
    friend bool operator!=(const ResourceVector& a, const ResourceVector& b) { return !(a == b); }
    friend bool operator<(const ResourceVector& a, const ResourceVector& b) { return a.values_ < b.values_; }

private:
    std::array<value_type, kResourceCount> values_{};
};

// A request's multidimensional resource demand.
class ResourceRequirement {
public:
    ResourceRequirement() = default;
    explicit ResourceRequirement(ResourceVector demand) : demand_(demand) {}

    [[nodiscard]] const ResourceVector& demand() const { return demand_; }
    void set_demand(ResourceVector demand) { demand_ = demand; }
    [[nodiscard]] bool is_zero() const { return demand_.is_zero(); }

private:
    ResourceVector demand_;
};

// Capability tokens. Capabilities are represented as canonical string tokens
// so the capability set is extensible; typed helpers produce canonical tokens.
class Capabilities {
public:
    void add(std::string token) { tokens_.insert(std::move(token)); }
    [[nodiscard]] bool contains(const std::string& token) const { return tokens_.count(token) != 0; }
    [[nodiscard]] bool contains_all(const Capabilities& required) const {
        for (const auto& t : required.tokens_) if (!contains(t)) return false;
        return true;
    }
    [[nodiscard]] const std::set<std::string>& tokens() const { return tokens_; }
    [[nodiscard]] std::size_t size() const { return tokens_.size(); }

    static std::string backend(const std::string& b) { return "backend=" + b; }
    static std::string compute_capability(const std::string& cc) { return "cuda:cc=" + cc; }
    static std::string min_vram(ByteCount bytes) { return "cuda:min_vram=" + std::to_string(bytes); }
    static std::string dtype(const std::string& d) { return "dtype=" + d; }
    static std::string quantization(const std::string& q) { return "quant=" + q; }
    static std::string runtime_feature(const std::string& f) { return "runtime=" + f; }
    static std::string execution_family(const std::string& f) { return "family=" + f; }
    static std::string adapter(const std::string& a) { return "adapter=" + a; }
    static std::string protocol_version(const std::string& v) { return "protocol=" + v; }

private:
    std::set<std::string> tokens_;
};

// A versioned, provenance-aware view of a node/device's capacity state.
struct ResourceSnapshot {
    ResourceSnapshotGeneration generation;
    TimePoint timestamp;
    AgentId agent_id;
    AgentBootId boot_id;
    std::string node_name;
    std::uint32_t device_index{0};
    std::string device_name;

    ResourceVector total;
    ResourceVector used;
    ResourceVector reserved;
    ResourceVector reclaimable;      // zero when unknown / not reclaimable

    Capabilities capabilities;
    bool healthy{true};

    std::vector<std::string> model_residency_hints;
    std::vector<std::string> transfer_path_hints;

    std::array<PressureLevel, kResourceCount> pressure{};
    std::array<Provenance, kResourceCount> provenance{};

    // Derived headroom: total - used - reserved, saturating at zero.
    [[nodiscard]] ResourceVector headroom() const {
        ResourceVector remain = total.sat_sub(used);
        return remain.sat_sub(reserved);
    }
    // Derived available after also considering reclaimable, if any.
    [[nodiscard]] ResourceVector available_including_reclaimable() const {
        return headroom().sat_add(reclaimable);
    }
    [[nodiscard]] bool is_stale_as_of(const TimePoint& now, Nanoseconds max_age) const {
        return now - timestamp > max_age;
    }
    [[nodiscard]] bool has_unavailable_dimension() const {
        for (auto p : provenance) if (p == Provenance::Unavailable) return true;
        return false;
    }
};

// Bounded resource accounting container for one node/device.
//
// Invariants maintained:
//   * used + reserved <= total (unless bounded overcommit, applied externally);
//   * no dimension ever goes negative;
//   * every mutation is overflow-checked.
class ResourceAccount {
public:
    void set_total(const ResourceVector& total) { total_ = total; }
    [[nodiscard]] const ResourceVector& total() const { return total_; }
    [[nodiscard]] const ResourceVector& used() const { return used_; }
    [[nodiscard]] const ResourceVector& reserved() const { return reserved_; }
    [[nodiscard]] ResourceVector headroom() const { return total_.sat_sub(used_).sat_sub(reserved_); }
    [[nodiscard]] ResourceVector available() const { return headroom(); }

    // Reserve 'demand'. Returns false if any dimension would exceed available
    // (no overcommit) or overflow. On failure nothing is changed.
    [[nodiscard]] bool reserve(const ResourceVector& demand) {
        // Enforce: used + (reserved + demand) <= total, all overflow-checked.
        ResourceVector next;
        if (!reserved_.try_add(demand, next)) return false;
        ResourceVector sum;
        if (!used_.try_add(next, sum) || !sum.fits_in(total_)) return false;
        reserved_ = next;
        return true;
    }

    // Move 'demand' from reserved to used (provisional -> committed). Returns
    // false if reserved does not contain enough of any dimension.
    [[nodiscard]] bool commit(const ResourceVector& demand) {
        if (!demand.fits_in(reserved_)) return false;
        ResourceVector r2, u2;
        if (!reserved_.try_sub(demand, r2)) return false;
        if (!used_.try_add(demand, u2)) return false;
        reserved_ = r2;
        used_ = u2;
        return true;
    }

    // Roll back 'demand' from reserved (exact inverse of reserve). Returns false
    // on double-rollback (reserved insufficient) or underflow.
    [[nodiscard]] bool rollback(const ResourceVector& demand) {
        if (!demand.fits_in(reserved_)) return false;
        ResourceVector r2;
        if (!reserved_.try_sub(demand, r2)) return false;
        reserved_ = r2;
        return true;
    }

    // Release 'demand' from used (committed work finished). Returns false on
    // double-release (used insufficient) or underflow.
    [[nodiscard]] bool release(const ResourceVector& demand) {
        if (!demand.fits_in(used_)) return false;
        ResourceVector u2;
        if (!used_.try_sub(demand, u2)) return false;
        used_ = u2;
        return true;
    }

    // Accounting coherence probe.
    [[nodiscard]] bool coherent() const {
        ResourceVector sum;
        if (!used_.try_add(reserved_, sum)) return false;
        return sum.fits_in(total_);
    }

private:
    ResourceVector total_;
    ResourceVector used_;
    ResourceVector reserved_;
};

} // namespace admission_fabric
