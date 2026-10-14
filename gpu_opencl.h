// ===========================================================================
//  gpu_opencl.h - OpenCL acceleration for the `app` derivation.
//
//  The GPU computes PBKDF2-HMAC-SHA512(2048) + SLIP-0010 (the expensive part);
//  the CPU still derives the ed25519 public key and the TON address.  If no
//  usable OpenCL device is present the class reports failure and the miner
//  keeps running on the CPU.
// ===========================================================================
#ifndef WALLET_TG_MINER_GPU_OPENCL_H
#define WALLET_TG_MINER_GPU_OPENCL_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ocl.h"

namespace gpu {

struct Info {
    bool        ok = false;
    std::string platform;
    std::string device;
    std::string vendor;
    std::string driver;
    std::string version;
    uint32_t    computeUnits = 0;
    uint64_t    globalMem    = 0;
    size_t      maxWorkGroup = 0;
    std::string error;
};

// Enumerate the system for a usable OpenCL GPU (opens and closes the runtime).
Info probe();

// Derives the 32-byte `app` secret for a batch of mnemonics.
class AppDeriver {
public:
    AppDeriver() = default;
    ~AppDeriver();
    AppDeriver(const AppDeriver &) = delete;
    AppDeriver &operator=(const AppDeriver &) = delete;

    bool init(std::string *err);
    void shutdown();
    bool ok() const { return kern_ != nullptr; }

    // secrets is resized to mnemonics.size() * 32.
    bool derive(const std::vector<std::string> &mnemonics,
                std::vector<uint8_t> &secrets, std::string *err);

    double lastKernelSeconds() const { return lastSeconds_; }
    const Info &info() const { return info_; }

private:
    bool ensureBuffers(size_t poolBytes, size_t count);

    OclApi       api_;
    Info         info_;
    cl_context   ctx_  = nullptr;
    cl_command_queue q_ = nullptr;
    cl_program   prog_ = nullptr;
    cl_kernel    kern_ = nullptr;
    cl_mem       bufMnem_ = nullptr;
    cl_mem       bufLens_ = nullptr;
    cl_mem       bufOut_  = nullptr;
    size_t       capMnem_ = 0;
    size_t       capLens_ = 0;
    size_t       capOut_  = 0;
    double       lastSeconds_ = 0.0;
};

} // namespace gpu

#endif // WALLET_TG_MINER_GPU_OPENCL_H
