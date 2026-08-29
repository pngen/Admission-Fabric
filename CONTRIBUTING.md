# Contributing

Thank you for contributing to Admission Fabric. This is a systems runtime that makes an authoritative admission decision, so correctness, boundedness and explainability matter more than convenience.

## Ground rules

- Keep the boundary narrow: Admission Fabric decides *whether* work may enter expensive accelerator infrastructure. Do not blur it into a scheduler, quota manager, resource broker, rate limiter, autoscaler or observability tool.
- Decisions must be typed (admit/defer/reject), explainable and carried by strong identity plus authority.
- Every potentially unbounded structure must be bounded.
- Capacity arithmetic must be overflow-checked; never let a silent wrap produce a valid-looking admission.
- No raw struct serialization, no pointer payloads, no executable network input.

## Building

Requires C++20 plus CMake 3.22 or newer. On Windows use an MSVC toolchain; for CUDA validation use CUDA 13.1 with an RTX 5090 (compute capability 120, with -DADMISSION_FABRIC_ENABLE_CUDA=ON).

## Testing

Add a seeded test harness case in tests/ using the small harness in tests/test_util.hpp. Randomized and property tests must print their seed. Concurrency must be fixed at the source; never use a timeout as a mutation escape hatch. The suite must pass in both Release and Debug with zero warnings under the MSVC warning gate.

## Style

- This repository compiles with MSVC -W4 /WX for C++ (CUDA flags are kept off nvcc).
- Fix warnings at the source; do not add broad suppression.
- Use the strong ID wrappers, not raw integers or loose strings, for identities.
- Use the Result/Status error model rather than exceptions for control flow.
- Keep CUDA optional at build time; never require the CUDA toolkit to build the core library.

## Commit

- Keep commits focused and atomic.
- The repository is committed under the identity of the project owner (pngen <pngen93@gmail.com>).
- Do not add unintended co-author trailers.
