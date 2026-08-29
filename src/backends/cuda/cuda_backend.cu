// Admission Fabric - CUDA backend (real RTX 5090 validation).
#include "admission_fabric/backends/cuda_backend.hpp"

#include <cstdio>
#include <vector>

#include <cuda_runtime.h>

namespace admission_fabric {

static Status cu_error(cudaError_t e, const char* what) {
    if (e == cudaSuccess) return Status::success();
    return Status::failure(ErrorCode::CudaError, std::string(what) + ": " + cudaGetErrorString(e));
}

__global__ void af_saxpy_kernel(float* out, const float* in, int n, float a) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) out[i] = a * in[i] + 1.0f;
}

Status CudaBackend::device_count(int& count) {
    int c = 0;
    Status s = cu_error(cudaGetDeviceCount(&c), "cudaGetDeviceCount");
    if (!s.ok()) return s;
    count = c;
    return Status::success();
}

Status CudaBackend::init(int device) {
    device_ = device;
    Status s = cu_error(cudaSetDevice(device), "cudaSetDevice");
    if (!s.ok()) return s;
    cudaDeviceProp prop;
    Status sp = cu_error(cudaGetDeviceProperties(&prop, device), "cudaGetDeviceProperties");
    if (!sp.ok()) return sp;
    physical_total_ = static_cast<std::size_t>(prop.totalGlobalMem);
    return Status::success();
}

Status CudaBackend::properties(std::string& name, int& major, int& minor, std::size_t& total) {
    cudaDeviceProp prop;
    Status s = cu_error(cudaGetDeviceProperties(&prop, device_), "cudaGetDeviceProperties");
    if (!s.ok()) return s;
    name = prop.name;
    major = prop.major;
    minor = prop.minor;
    total = static_cast<std::size_t>(prop.totalGlobalMem);
    return Status::success();
}

Status CudaBackend::mem_info(std::size_t& free_bytes, std::size_t& total_bytes) {
    std::size_t f = 0, t = 0;
    Status s = cu_error(cudaMemGetInfo(&f, &t), "cudaMemGetInfo");
    if (!s.ok()) return s;
    free_bytes = f;
    total_bytes = t;
    return Status::success();
}

Status CudaBackend::set_governed_capacity(std::size_t bytes) {
    if (bytes == 0 || bytes > physical_total_) return Status::failure(ErrorCode::CudaDeviceMismatch, "governed capacity out of range");
    governed_capacity_ = bytes;
    return Status::success();
}

Status CudaBackend::allocate(void*& ptr, std::size_t bytes) {
    void* p = nullptr;
    Status s = cu_error(cudaMalloc(&p, bytes), "cudaMalloc");
    if (!s.ok()) return s;
    ptr = p;
    return Status::success();
}

Status CudaBackend::free(void* ptr) {
    if (ptr == nullptr) return Status::success();
    return cu_error(cudaFree(ptr), "cudaFree");
}

Status CudaBackend::memcpy_h2d(void* dst, const void* src, std::size_t bytes) {
    return cu_error(cudaMemcpy(dst, src, bytes, cudaMemcpyHostToDevice), "cudaMemcpy H2D");
}
Status CudaBackend::memcpy_d2h(void* dst, const void* src, std::size_t bytes) {
    return cu_error(cudaMemcpy(dst, src, bytes, cudaMemcpyDeviceToHost), "cudaMemcpy D2H");
}
Status CudaBackend::sync() { return cu_error(cudaDeviceSynchronize(), "cudaDeviceSynchronize"); }

Status CudaBackend::run_and_verify_add(std::size_t n_floats) {
    if (n_floats == 0) return Status::failure(ErrorCode::CudaError, "zero element kernel");
    std::vector<float> host_in(n_floats);
    for (std::size_t i = 0; i < n_floats; ++i) host_in[i] = static_cast<float>(i);
    float* dev_in = nullptr;
    float* dev_out = nullptr;
    Status a1 = allocate(reinterpret_cast<void*&>(dev_in), n_floats * sizeof(float));
    if (!a1.ok()) return a1;
    Status a2 = allocate(reinterpret_cast<void*&>(dev_out), n_floats * sizeof(float));
    if (!a2.ok()) { free(dev_in); return a2; }
    Status c1 = memcpy_h2d(dev_in, host_in.data(), n_floats * sizeof(float));
    if (!c1.ok()) { free(dev_in); free(dev_out); return c1; }
    int n = static_cast<int>(n_floats);
    af_saxpy_kernel<<<(n + 255) / 256, 256>>>(dev_out, dev_in, n, 2.0f);
    Status sy = sync();
    if (!sy.ok()) { free(dev_in); free(dev_out); return sy; }
    std::vector<float> host_out(n_floats);
    Status c2 = memcpy_d2h(host_out.data(), dev_out, n_floats * sizeof(float));
    if (!c2.ok()) { free(dev_in); free(dev_out); return c2; }
    for (std::size_t i = 0; i < n_floats; ++i) {
        if (host_out[i] != 2.0f * host_in[i] + 1.0f) {
            free(dev_in); free(dev_out);
            return Status::failure(ErrorCode::CudaKernelFailure, "kernel output mismatch");
        }
    }
    Status f1 = free(dev_in);
    Status f2 = free(dev_out);
    return f1.ok() ? f2 : f1;
}

} // namespace admission_fabric
