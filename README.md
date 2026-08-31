# Admission Fabric

Admission Fabric is an open-source, vendor-neutral runtime for deciding whether AI work should enter expensive accelerator infrastructure based on capacity reservation, quota, predicted resource demand, token budget, expected duration, and SLO feasibility.

Its core systems question is:

> Given current capacity, quotas, predicted resource demand, token budget, expected duration, and latency obligations, can this work be admitted safely now, should it be deferred, or must it be rejected?

Admission Fabric owns the decision boundary **before** expensive accelerator work enters the serving or execution system. Every admitted request consumes future capacity, resource headroom, latency slack and operational optionality; Admission Fabric exists to make that *promise* explicit, bounded, explainable and authoritative.

## Where the boundary is drawn

Admission Fabric is deliberately narrow. It is **not** a scheduler, a load balancer, a quota database, a resource broker, a latency monitor, an autoscaler or a generic rate limiter.

The distinction is fundamental:

- **Inference Scheduler** decides what admitted work runs next and where.
- **Latency Governor** governs already-expressed latency obligations at runtime.
- **Quota Fabric** governs persistent multi-tenant entitlements and consumption limits.
- **Resource Broker** arbitrates concrete scarce resources across active workloads.
- **Memory Pressure** detects when memory scarcity becomes operationally dangerous.
- **Admission Fabric** decides whether new work may cross the boundary into the expensive execution system in the first place.

Admission Fabric answers: *is this request feasible now, what resources would it require, which obligations must be reserved, is the capacity estimate trustworthy, would accepting this jeopardise already-admitted work, can it be deferred, is rejection mandatory, and what reservation and authority prove the decision is still valid?*

## Admission is an authoritative state transition

A request originates outside the expensive execution system and moves through an explicit, validated lifecycle: PROPOSED -> EVALUATING -> RESERVED -> ADMITTED -> RELEASED, with DEFERRED, REJECTED, EXPIRED and CANCELLED as first-class states.

Admission never reduces to queue insertion. Decisions are typed: ADMIT, DEFER or REJECT, each with a reason code (ADMIT_WITH_RESERVATION, ADMIT_WITH_RISK, DEFER_CAPACITY, DEFER_QUOTA, DEFER_SLO, REJECT_INFEASIBLE, REJECT_POLICY, REJECT_CAPABILITY, REJECT_QUOTA, REJECT_EXPIRED, ...). Every decision carries the request identity, the governing policy and resource-snapshot generations, the predicted requirement and duration, SLO feasibility, the quota result, the reservation that backs an admission, confidence, and a human-readable explanation.

## Architecture

A client submits an admission descriptor to the engine. The engine, under a versioned policy, predicts the peak memory footprint and duration, checks capability feasibility, evaluates the supplied quota envelope, determines SLO feasibility, and then performs an atomic capacity reservation against a provenance-aware resource snapshot. A positive decision returns a reservation id that proves the admission is still valid; a commit_admission transition makes it authoritative, and a later release frees the capacity.

The runtime is composed of:

- **Identity & authority** (id.hpp) - strong typed IDs so stale values cannot collide with fresh ones.
- **Resource model** (resource.hpp) - multidimensional demand, versioned provenance-aware ResourceSnapshot, and a ResourceAccount enforcing used + reserved <= total.
- **Descriptor & decision** (descriptor.hpp, decision.hpp) - the rich admission descriptor and typed admit/defer/reject decision with reason codes.
- **Prediction** (prediction.hpp) - a bounded predictor for peak/sustained memory footprint (fixed model, KV growth, tensor state, workspace, staging, margins), token-derived growth, duration, confidence, evidence count, provenance and cold-start fallback. Observations feed back to raise confidence and adjust the variable footprint.
- **SLO feasibility** (slo.hpp) - pre-admission feasibility from predicted duration, queue/transfer/reservation delay, deadline and uncertainty.
- **Quota inputs** (quota.hpp) - an externally supplied quota envelope evaluated for admission.
- **Reservation** (reservation.hpp) - atomic multi-resource reservation with exact rollback, idempotent release, double-commit/double-release rejection, reserved/used accounting, and bounded overcommit (NONE / BOUNDED / PREDICTION_AWARE).
- **Policy engine** (policy.hpp) - validated, versioned, serializable, atomically replaceable policy; decisions retain their governing generation.
- **Fairness** (fairness.hpp) - deterministic bounded fairness so a large request cannot starve smaller ones and vice versa.
- **Serialization** (serialize.hpp, codec.hpp) - canonical field-by-field encoding with CRC32 framing; readers reject truncation, corruption, unknown versions, impossible counts and trailing bytes.
- **Persistence** (persistence.hpp) - atomic temp-write -> flush -> close -> atomic-replace state store; recovery rejects corrupt/truncated/duplicate/inconsistent state.
- **Distributed** (coordinator.hpp, agent.hpp, net.hpp, protocol.hpp) - framed TCP over Winsock, a coordinator with an epoch, capacity agents with a boot identity, and strict stale-authority rejection.

## Admission inputs

| Input | Meaning |
|-------|---------|
| Capacity snapshot | versioned total/used/reserved capacity with provenance (measured/configured/derived/predicted/stale/unavailable) |
| Quota snapshot | externally supplied limits + consumption + generation |
| Token budget | input and maximum/expected output tokens, tenant token budget |
| SLO / deadline | latency class, absolute deadline, requested SLO percentile |
| Capability | backend, dtype, quantization, compute capability, runtime features |
| Prediction input | model/revision, tokens, batch, sequence count, dtype, quantization, historical observations |

## Building

Requires a C++20 compiler with CMake >= 3.22. On Windows an MSVC toolchain (VS 2022) and, for the CUDA backend, CUDA 13.1 with a compute-capability-120 device (an RTX 5090).

The core library is CUDA-optional and builds without any CUDA toolkit.

## Installing and consuming

From a downstream project:

cmake:
  find_package(AdmissionFabric CONFIG REQUIRED)
  target_link_libraries(app PRIVATE AdmissionFabric::admission_fabric)

See examples/ for 15 runnable scenarios and benchmarks/ for a completed-work benchmark.

## Tests

- tests/unit - engine, reservation, prediction, serialization round-trips.
- tests/property - randomized accounting/reservation/serialization invariants (seeded and printed).
- tests/concurrency - concurrent reservation churn over thousands of operations.
- tests/adversarial - zero capacity, oversized requests, duplicate/malformed IDs, expired deadlines.
- tests/persistence - crash-recovery round-trip and corruption rejection.
- tests/multiprocess - real framed-TCP coordinator + capacity agents; an agent is killed, the coordinator rolls its epoch, the agent restarts with a new boot identity, stale epoch/boot/admission-generation/reservation/snapshot messages are rejected, fresh authoritative admission succeeds, and accounting returns exactly to zero.
- tests/cuda - real RTX 5090 governed-capacity admission proof and prediction-feedback proof (CUDA-enabled build only).

## Validation (measured on this build)

Toolchain: MSVC 19.44 (VS 2022), CMake 4.3.2, CUDA 13.1, NVIDIA GeForce RTX 5090 (compute capability 12.0). Only measured facts are recorded.

| Area | Result |
|------|--------|
| Build (CUDA Release) | Clean; zero warnings under MSVC -W4 /WX by construction |
| CTest targets | 14; all passed in the CUDA Release build |
| Randomized accounting | 5000 reserve/release operations, accounting invariant held every step |
| Randomized reservation | 4000 reserve/commit/release operations, verify_accounting held every step; final reserved/used zero |
| Randomized serialization | 300 framed round-trips, deterministic checksums |
| Concurrency | 8 threads x 2000 reservation churn operations; coherent, zero at end |
| Descriptor round-trip fuzz | 2000 primitives, 500 descriptors |
| Multiprocess restart/fencing | coordinator rolls epoch on agent kill; stale boot, stale epoch, stale admission generation, stale reservation all rejected with explicit codes; fresh admission succeeds; release; accounting coherent |
| CUDA governed-capacity | 2 GiB governed: R1 admitted + real cudaMalloc + kernel + verify + free; R2 deferred; after release R2 admitted; accounting returns to zero |
| CUDA prediction-feedback | 3 real observations -> confidence 0.75, hybrid provenance; decision changes when governed capacity 6 GiB (admit) vs 200 MiB (defer) |
| Examples | 15 runnable examples all executed their intended scenario |
| Benchmark (completed work) | descriptor validation 61.5M ops/s; resource prediction 1.72M ops/s; admission evaluation 3.3K ops/s; atomic reservation+release 3.6M ops/s; serialization 1.22M ops/s; threaded admission 129K ops/s |
| Downstream find_package | external consumer configured, built and ran against an installed prefix; admitted |
| Test runtime | full suite ~1.3 s |

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
