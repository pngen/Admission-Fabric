# Security

Admission Fabric makes an authoritative promise before work enters expensive accelerator infrastructure. Its security posture centers on the integrity of that decision and on never letting untrusted input, stale authority, or malformed data corrupt admission or accounting state.

## Untrusted admission descriptors

- Descriptors are validated eagerly and rejected with reason code REJECT_MALFORMED before any prediction or reservation occurs.
- Token counts, deadlines, priorities and TTLs are bounded; absurd or negative values are rejected.
- Resource demand is produced by the bounded predictor from evidence, never silently manufactured from the descriptor.
- No pointer, raw struct, or executor payload crosses the API or the wire.

## Bounded resource claims

- Every potentially unbounded structure is capped: active requests, retained outcomes, deferred entries, tenants, models/profiles, predictor samples, policy generations, agents, reservations, observations, frame size, string length, token counts and resource dimensions.
- Overflow is checked (checked.hpp) so capacity arithmetic cannot silently wrap into a valid-looking admission.

## Malformed protocol handling

- Frames carry a magic, protocol version, type, payload length and correlation id in a 24-byte header.
- Receive loops handle partial frames; truncated, malformed, oversized (beyond 16 MiB), unknown-type and unsupported-version frames are rejected with explicit error codes.
- Winsock startup/cleanup, accepted-socket state and socket lifetime are handled centrally.

## Persistence integrity

- State is encoded field-by-field with a CRC32 trailer over a magic and version header.
- Loads reject truncation, corruption, unknown versions, impossible counts, duplicate identities and inconsistent reservation accounting.
- Writes are atomic: temporary file, write, flush, close, atomic replace.

## Replay and stale-authority rejection

- Messages that can change admission or reservation state carry a coordinator epoch, agent boot identity, admission generation, reservation id and snapshot generation.
- The coordinator validates them in strict order; stale epoch, boot identity, admission generation, reservation and snapshot generation are rejected.

## Reservation exhaustion and quota abuse

- Reservation is atomic and bounded; a request that cannot fully reserve a multi-resource vector is deferred, never partially reserved.
- Double-commit, double-release, reservation-after-rejection and admission-without-reservation are rejected.
- Quota envelopes are evaluated against the supplied generation; hard token-budget violations are rejected, resource headroom shortfalls are deferred.

## Prediction poisoning considerations

- Observations are validated before they are recorded; malformed or negative observations are dropped and cannot corrupt prediction history.
- Predictor confidence and learned adjustments are bounded and clamped; a single hostile observation cannot push an estimate to an unbounded value.
- Repeated under-prediction raises reserved margins rather than silently widening overcommit.

## DoS bounds

- All loops are bounded; frame size, string length, count and vector lengths are capped.
- There are no unbounded allocations from network input; a 16 MiB frame cap bounds per-message memory.

## Integer overflow prevention

- Capacity, token, duration and deadline arithmetic go through checked primitives; failures propagate as explicit error codes, never as silent wraparound.
- Predictions are clamped to be finite (no NaN or Inf).

## No raw pointer serialization and no executable network payloads

- Serialization is field-by-field and canonical; there is no reinterpret_cast of structs and no loading of a pointer or function pointer from disk or the wire.
- No network payload is ever executed or interpreted as code.

## CUDA trust boundary

- The CUDA backend validates device discovery, memory allocation, H2D, kernel execution, synchronization, D2H and cudaFree.
- Governed capacity is bounded below physical VRAM so the proof never approaches physical OOM.
- cudaGetDeviceProperties is used for capability and discovery; the backend never accepts arbitrary device pointers from untrusted input.

## Reporting

Please report security concerns privately to the maintainers before public disclosure. Do not include secrets, reproduced state files, or live credentials in a report.
