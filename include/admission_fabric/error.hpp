#pragma once

// Admission Fabric - error model.
//
// The runtime communicates failures with explicit, structured error codes
// rather than opaque exceptions. Every code has a stable integer value so it
// can be serialized, compared, and reported across process boundaries. The
// error code taxonomy maps one-to-one onto the prose reason codes used by
// admission decisions; a distance is kept because a *decision* reason (for
// example DEFER_CAPACITY) is an outcome the caller is meant to act on, whereas
// an *error* code describes a failure of the runtime to perform an operation.

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace admission_fabric {

enum class ErrorCode : std::int32_t {
    // --- success / generic -------------------------------------------------
    Ok = 0,
    InvalidArgument = 1,
    MalformedDescriptor = 2,
    UnknownError = 3,
    NotImplemented = 4,
    OperationNotPermitted = 5,
    OutOfRange = 6,

    // --- capacity / resources ---------------------------------------------
    InsufficientCapacity = 20,
    NegativeCapacity = 21,
    CapacityOverflow = 22,
    NoValidCapacity = 23,
    HostCapacityMismatch = 24,

    // --- quota -------------------------------------------------------------
    QuotaViolation = 30,
    QuotaGenerationMismatch = 31,

    // --- capability --------------------------------------------------------
    CapabilityMismatch = 40,
    UnsupportedDtype = 41,
    UnsupportedBackend = 42,

    // --- SLO / deadline ----------------------------------------------------
    SloInfeasible = 50,
    ExpiredDeadline = 51,
    DeadlineOverflow = 52,

    // --- stale authority ---------------------------------------------------
    StaleEpoch = 60,
    StaleBootIdentity = 61,
    StaleAdmissionGeneration = 62,
    StaleReservation = 63,
    StaleSnapshotGeneration = 64,
    StalePolicyGeneration = 65,
    StaleCapacityGeneration = 66,
    StaleInterpretation = 67,

    // --- reservation -------------------------------------------------------
    DoubleReservation = 70,
    DoubleRelease = 71,
    ReservationAfterRejection = 72,
    ReservationExpired = 73,
    ReservationIdentityCollision = 74,
    AdmissionWithoutReservation = 75,
    ReservationLeak = 76,
    ReservationOverflow = 77,
    ReservationRollbackError = 78,

    // --- policy ------------------------------------------------------------
    PolicyValidationError = 80,
    UnknownPolicyGeneration = 81,
    PolicyConflict = 82,

    // --- persistence -------------------------------------------------------
    PersistenceCorrupt = 90,
    PersistenceTruncated = 91,
    PersistenceInvalidCount = 92,
    PersistenceUnknownVersion = 93,
    PersistenceDuplicateId = 94,
    PersistenceNegativeValue = 95,
    PersistenceInconsistentAccounting = 96,
    PersistenceImpossibleTransition = 97,
    PersistenceChecksumMismatch = 98,
    PersistenceIoError = 99,

    // --- protocol ----------------------------------------------------------
    MalformedFrame = 110,
    TruncatedFrame = 111,
    UnknownMessage = 112,
    UnsupportedVersion = 113,
    FrameTooLarge = 114,
    ConnectionReset = 115,
    IoError = 116,
    SocketError = 117,
    AddressInUse = 118,

    // --- CUDA / hardware ---------------------------------------------------
    CudaError = 120,
    CudaAllocationFailure = 121,
    CudaKernelFailure = 122,
    CudaDeviceMismatch = 123,
    DeviceNotPresent = 124,

    // --- distributed / resource loss ---------------------------------------
    AgentLost = 130,
    CapacityFenced = 131,
    AgentAlreadyRegistered = 132,
    NoAgents = 133,
    AgentBootMismatch = 134,
};

inline const char* error_code_name(ErrorCode c) {
    switch (c) {
        case ErrorCode::Ok: return "Ok";
        case ErrorCode::InvalidArgument: return "InvalidArgument";
        case ErrorCode::MalformedDescriptor: return "MalformedDescriptor";
        case ErrorCode::UnknownError: return "UnknownError";
        case ErrorCode::NotImplemented: return "NotImplemented";
        case ErrorCode::OperationNotPermitted: return "OperationNotPermitted";
        case ErrorCode::OutOfRange: return "OutOfRange";
        case ErrorCode::InsufficientCapacity: return "InsufficientCapacity";
        case ErrorCode::NegativeCapacity: return "NegativeCapacity";
        case ErrorCode::CapacityOverflow: return "CapacityOverflow";
        case ErrorCode::NoValidCapacity: return "NoValidCapacity";
        case ErrorCode::HostCapacityMismatch: return "HostCapacityMismatch";
        case ErrorCode::QuotaViolation: return "QuotaViolation";
        case ErrorCode::QuotaGenerationMismatch: return "QuotaGenerationMismatch";
        case ErrorCode::CapabilityMismatch: return "CapabilityMismatch";
        case ErrorCode::UnsupportedDtype: return "UnsupportedDtype";
        case ErrorCode::UnsupportedBackend: return "UnsupportedBackend";
        case ErrorCode::SloInfeasible: return "SloInfeasible";
        case ErrorCode::ExpiredDeadline: return "ExpiredDeadline";
        case ErrorCode::DeadlineOverflow: return "DeadlineOverflow";
        case ErrorCode::StaleEpoch: return "StaleEpoch";
        case ErrorCode::StaleBootIdentity: return "StaleBootIdentity";
        case ErrorCode::StaleAdmissionGeneration: return "StaleAdmissionGeneration";
        case ErrorCode::StaleReservation: return "StaleReservation";
        case ErrorCode::StaleSnapshotGeneration: return "StaleSnapshotGeneration";
        case ErrorCode::StalePolicyGeneration: return "StalePolicyGeneration";
        case ErrorCode::StaleCapacityGeneration: return "StaleCapacityGeneration";
        case ErrorCode::StaleInterpretation: return "StaleInterpretation";
        case ErrorCode::DoubleReservation: return "DoubleReservation";
        case ErrorCode::DoubleRelease: return "DoubleRelease";
        case ErrorCode::ReservationAfterRejection: return "ReservationAfterRejection";
        case ErrorCode::ReservationExpired: return "ReservationExpired";
        case ErrorCode::ReservationIdentityCollision: return "ReservationIdentityCollision";
        case ErrorCode::AdmissionWithoutReservation: return "AdmissionWithoutReservation";
        case ErrorCode::ReservationLeak: return "ReservationLeak";
        case ErrorCode::ReservationOverflow: return "ReservationOverflow";
        case ErrorCode::ReservationRollbackError: return "ReservationRollbackError";
        case ErrorCode::PolicyValidationError: return "PolicyValidationError";
        case ErrorCode::UnknownPolicyGeneration: return "UnknownPolicyGeneration";
        case ErrorCode::PolicyConflict: return "PolicyConflict";
        case ErrorCode::PersistenceCorrupt: return "PersistenceCorrupt";
        case ErrorCode::PersistenceTruncated: return "PersistenceTruncated";
        case ErrorCode::PersistenceInvalidCount: return "PersistenceInvalidCount";
        case ErrorCode::PersistenceUnknownVersion: return "PersistenceUnknownVersion";
        case ErrorCode::PersistenceDuplicateId: return "PersistenceDuplicateId";
        case ErrorCode::PersistenceNegativeValue: return "PersistenceNegativeValue";
        case ErrorCode::PersistenceInconsistentAccounting: return "PersistenceInconsistentAccounting";
        case ErrorCode::PersistenceImpossibleTransition: return "PersistenceImpossibleTransition";
        case ErrorCode::PersistenceChecksumMismatch: return "PersistenceChecksumMismatch";
        case ErrorCode::PersistenceIoError: return "PersistenceIoError";
        case ErrorCode::MalformedFrame: return "MalformedFrame";
        case ErrorCode::TruncatedFrame: return "TruncatedFrame";
        case ErrorCode::UnknownMessage: return "UnknownMessage";
        case ErrorCode::UnsupportedVersion: return "UnsupportedVersion";
        case ErrorCode::FrameTooLarge: return "FrameTooLarge";
        case ErrorCode::ConnectionReset: return "ConnectionReset";
        case ErrorCode::IoError: return "IoError";
        case ErrorCode::SocketError: return "SocketError";
        case ErrorCode::AddressInUse: return "AddressInUse";
        case ErrorCode::CudaError: return "CudaError";
        case ErrorCode::CudaAllocationFailure: return "CudaAllocationFailure";
        case ErrorCode::CudaKernelFailure: return "CudaKernelFailure";
        case ErrorCode::CudaDeviceMismatch: return "CudaDeviceMismatch";
        case ErrorCode::DeviceNotPresent: return "DeviceNotPresent";
        case ErrorCode::AgentLost: return "AgentLost";
        case ErrorCode::CapacityFenced: return "CapacityFenced";
        case ErrorCode::AgentAlreadyRegistered: return "AgentAlreadyRegistered";
        case ErrorCode::NoAgents: return "NoAgents";
        case ErrorCode::AgentBootMismatch: return "AgentBootMismatch";
    }
    return "UnknownErrorCode";
}

// A structured error with a code and a human-readable message.
class Error {
public:
    Error() = default;
    explicit Error(ErrorCode code, std::string message = {})
        : code_(code), message_(std::move(message)) {}

    [[nodiscard]] ErrorCode code() const noexcept { return code_; }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }
    [[nodiscard]] bool ok() const noexcept { return code_ == ErrorCode::Ok; }
    [[nodiscard]] explicit operator bool() const noexcept { return !ok(); }

    static Error success() { return Error(ErrorCode::Ok); }
    static Error from(ErrorCode c, std::string msg = {}) { return Error(c, std::move(msg)); }

private:
    ErrorCode code_{ErrorCode::Ok};
    std::string message_;
};

// Status: a failure-or-success signal with no value payload.
class Status {
public:
    Status() = default;
    explicit Status(Error error) : error_(std::move(error)) {}
    static Status success() { return Status(); }
    static Status failure(Error error) { return Status(std::move(error)); }
    static Status failure(ErrorCode c, std::string msg = {}) { return Status(Error(c, std::move(msg))); }

    [[nodiscard]] bool ok() const noexcept { return error_.ok(); }
    [[nodiscard]] explicit operator bool() const noexcept { return ok(); }
    [[nodiscard]] const Error& error() const noexcept { return error_; }
    [[nodiscard]] ErrorCode code() const noexcept { return error_.code(); }
    [[nodiscard]] const std::string& message() const noexcept { return error_.message(); }

private:
    Error error_;
};

// Result<T>: either a value or an error. Intentionally minimal and move-aware.
template <typename T>
class Result {
public:
    Result() = default;  // represents a present default-constructed value
    Result(const T& value) : has_(true), value_(value) {}
    Result(T&& value) : has_(true), value_(std::move(value)) {}
    Result(Error error) : has_(false), error_(std::move(error)) {}

    static Result ok(T value) { return Result(std::move(value)); }
    static Result fail(Error error) { return Result(std::move(error)); }
    static Result fail(ErrorCode c, std::string msg = {}) { return Result(Error(c, std::move(msg))); }

    [[nodiscard]] bool has_value() const noexcept { return has_; }
    [[nodiscard]] explicit operator bool() const noexcept { return has_; }

    [[nodiscard]] T& value() { return value_; }
    [[nodiscard]] const T& value() const { return value_; }
    [[nodiscard]] T&& take() { has_ = false; return std::move(value_); }

    [[nodiscard]] const Error& error() const { return error_; }
    [[nodiscard]] ErrorCode code() const { return error_.code(); }
    [[nodiscard]] const std::string& message() const { return error_.message(); }

private:
    bool has_{true};
    T value_{};
    Error error_;
};

} // namespace admission_fabric
