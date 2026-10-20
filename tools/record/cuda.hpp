#pragma once
// The CUDA driver calls NVENC's path needs, loaded from libcuda at run time
// as gpu-screen-recorder does (no build dependency on CUDA). The types are
// the driver API's own, as NVIDIA's cuda.h documents them; defining
// CUDA_VERSION lets FFmpeg's hwcontext_cuda.h use them instead of cuda.h.

#include <cstddef>
#include <cstdint>

#define CUDA_VERSION 12000

using CUresult = int;
using CUdevice = int;
using CUdeviceptr = unsigned long long;
typedef struct CUctx_st* CUcontext;
typedef struct CUstream_st* CUstream;
typedef struct CUarray_st* CUarray;
typedef struct CUgraphicsResource_st* CUgraphicsResource;

namespace atrium::record::cuda {

constexpr unsigned kCtxSchedAuto = 0;
constexpr unsigned kRegisterReadOnly = 1;   // CU_GRAPHICS_REGISTER_FLAGS_READ_ONLY
constexpr int kMemoryDevice = 2;            // CU_MEMORYTYPE_DEVICE
constexpr int kMemoryArray = 3;             // CU_MEMORYTYPE_ARRAY

struct Memcpy2D {
    size_t srcXInBytes, srcY;
    int srcMemoryType;
    const void* srcHost;
    CUdeviceptr srcDevice;
    CUarray srcArray;
    size_t srcPitch;
    size_t dstXInBytes, dstY;
    int dstMemoryType;
    void* dstHost;
    CUdeviceptr dstDevice;
    CUarray dstArray;
    size_t dstPitch;
    size_t WidthInBytes, Height;
};

struct Api {
    CUresult (*cuInit)(unsigned);
    CUresult (*cuDeviceGetByPCIBusId)(CUdevice*, const char*);
    CUresult (*cuDeviceGet)(CUdevice*, int);
    CUresult (*cuCtxCreate_v2)(CUcontext*, unsigned, CUdevice);
    CUresult (*cuCtxDestroy_v2)(CUcontext);
    CUresult (*cuCtxPushCurrent_v2)(CUcontext);
    CUresult (*cuCtxPopCurrent_v2)(CUcontext*);
    CUresult (*cuGetErrorString)(CUresult, const char**);
    CUresult (*cuGraphicsGLRegisterImage)(CUgraphicsResource*, unsigned, unsigned, unsigned);
    CUresult (*cuGraphicsUnregisterResource)(CUgraphicsResource);
    CUresult (*cuGraphicsMapResources)(unsigned, CUgraphicsResource*, CUstream);
    CUresult (*cuGraphicsUnmapResources)(unsigned, CUgraphicsResource*, CUstream);
    CUresult (*cuGraphicsSubResourceGetMappedArray)(CUarray*, CUgraphicsResource, unsigned, unsigned);
    CUresult (*cuMemcpy2DAsync_v2)(const Memcpy2D*, CUstream);
    CUresult (*cuStreamSynchronize)(CUstream);
};

// libcuda's functions; null when it's not there.
const Api* load();
// A message for a result.
const char* error(CUresult r);

} // namespace atrium::record::cuda
