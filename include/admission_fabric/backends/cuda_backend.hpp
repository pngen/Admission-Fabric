#pragma once

// Admission Fabric - real CUDA capacity/admission backend.
//
// Wraps an actual NVIDIA CUDA device (validated on the RTX 5090 with CUDA 13.1,
// compute capability 12.0) to prove accelerator-side resource accounting and
// admission behavior with real allocation pressure. It uses real device
// discovery, cudaMemGetInfo, cudaMalloc, H2D, kernel execution, sync, D2H,
// verification and cudaFree. Governed capacity is kept below physical VRAM so
// the proof never approaches physical OOM.

#include <cstddef>
#include <cstdint>
#include <string>
#include "admission_fabric/error.hpp"

namespace admission_fabric {

class CudaBackend {
public:
    static Status device_count(int& count);

    Status init(int device = 0);
    Status properties(std::string& name, int& compute_major, int& compute_minor, std::size_t& total_vram);
    Status mem_info(std::size_t& free_bytes, std::size_t& total_bytes);

    // Governed (managed) capacity for admission decisions, typically a fraction
    // of physical VRAM. Held here so the proof can drive admission against it.
    Status set_governed_capacity(std::size_t bytes);
    std::size_t governed_capacity() const { return governed_capacity_; }
    std::size_t physical_total() const { return physical_total_; }

    Status allocate(void*& ptr, std::size_t bytes);
    Status free(void* ptr);
    Status memcpy_h2d(void* dst, const void* src, std::size_t bytes);
    Status memcpy_d2h(void* dst, const void* src, std::size_t bytes);
    Status sync();

    // Run a real elementwise add kernel over n floats and verify the result.
    Status run_and_verify_add(std::size_t n_floats);

    int device() const { return device_; }

private:
    int device_{0};
    std::size_t physical_total_{0};
    std::size_t governed_capacity_{0};
};

} // namespace admission_fabric
