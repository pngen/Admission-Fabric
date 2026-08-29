#include "admission_fabric/persistence.hpp"

#include <fstream>

#ifdef _WIN32
    #include <windows.h>
#endif

namespace admission_fabric {

Status StateStore::build_payload(const AdmissionFabric& f, std::vector<std::uint8_t>& out, const PersistenceConfig& cfg) const {
    ByteWriter w;
    Status s = f.save_state(w);
    if (!s.ok()) return s;
    out = ByteWriter::frame(cfg.magic, cfg.version, w.data());
    return Status::success();
}

Status StateStore::apply_payload(AdmissionFabric& f, const std::vector<std::uint8_t>& payload, const PersistenceConfig& cfg) const {
    std::vector<std::uint8_t> body;
    std::string err;
    // A framed buffer ends with a 16-byte trailer; split and validate.
    if (!ByteReader::parse_frame(payload, cfg.magic, cfg.version, body, err))
        return Status::failure(ErrorCode::PersistenceChecksumMismatch, err);
    ByteReader r(body);
    Status s = f.restore_state(r);
    if (!s.ok()) return s;
    if (!r.ok()) return Status::failure(ErrorCode::PersistenceCorrupt, r.error());
    if (r.remaining() != 0) return Status::failure(ErrorCode::PersistenceCorrupt, "trailing bytes after state");
    return Status::success();
}

Status StateStore::save(const PersistenceConfig& cfg, const AdmissionFabric& f) const {
    std::vector<std::uint8_t> payload;
    Status s = build_payload(f, payload, cfg);
    if (!s.ok()) return s;

    // Build the destination directory if missing.
    if (auto parent = cfg.path.parent_path(); !parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
    }

    // 1. Write the temporary file.
    std::filesystem::path tmp = cfg.temp_path();
    {
        std::ofstream os(tmp, std::ios::binary | std::ios::trunc);
        if (!os) return Status::failure(ErrorCode::PersistenceIoError, "cannot open temp file");
        os.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
        os.flush();
        if (!os) return Status::failure(ErrorCode::PersistenceIoError, "write failed");
        os.close();
        if (!os) return Status::failure(ErrorCode::PersistenceIoError, "close failed");
    }

    // 2. Atomic replace.
#ifdef _WIN32
    if (!MoveFileExW(tmp.c_str(), cfg.path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return Status::failure(ErrorCode::PersistenceIoError, "atomic rename failed");
#else
    std::error_code ec;
    std::filesystem::rename(tmp, cfg.path, ec);
    if (ec) return Status::failure(ErrorCode::PersistenceIoError, "atomic rename failed");
#endif
    return Status::success();
}

Result<std::vector<std::uint8_t>> StateStore::load_payload(const PersistenceConfig& cfg) const {
    std::ifstream is(cfg.path, std::ios::binary);
    if (!is) return Result<std::vector<std::uint8_t>>::fail(ErrorCode::PersistenceIoError, "cannot open state file");
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(is)), std::istreambuf_iterator<char>());
    if (bytes.empty()) return Result<std::vector<std::uint8_t>>::fail(ErrorCode::PersistenceTruncated, "empty state file");
    std::vector<std::uint8_t> body;
    std::string err;
    if (!ByteReader::parse_frame(bytes, cfg.magic, cfg.version, body, err))
        return Result<std::vector<std::uint8_t>>::fail(ErrorCode::PersistenceChecksumMismatch, err);
    return Result<std::vector<std::uint8_t>>::ok(std::move(bytes));
}

Status StateStore::load(const PersistenceConfig& cfg, AdmissionFabric& f) const {
    auto r = load_payload(cfg);
    if (!r.has_value()) return Status::failure(r.code(), r.message());
    return apply_payload(f, r.value(), cfg);
}

std::size_t StateStore::payload_size(const AdmissionFabric& f, const PersistenceConfig& cfg) {
    StateStore st;
    std::vector<std::uint8_t> payload;
    if (!st.build_payload(f, payload, cfg).ok()) return 0;
    return payload.size();
}

} // namespace admission_fabric
