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

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
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

    // How many batches may be queued by submit() before it refuses work.
    static constexpr int kDepth = 3;

    bool init(std::string *err);
    void shutdown();
    bool ok() const { return kern_ != nullptr; }

    // Blocking: writes the batch, runs the kernel, waits, reads the result.
    // secrets is resized to mnemonics.size() * 32.
    bool derive(const std::vector<std::string> &mnemonics,
                std::vector<uint8_t> &secrets, std::string *err);

    // Pipelined mode.  submit() only enqueues work and returns; while the GPU
    // crunches the batch, the caller derives keys for a previous one.  Up to
    // kDepth batches may be in flight.  Takes ownership of the mnemonics.
    bool submit(std::vector<std::string> &&mnemonics, std::string *err);
    // Waits for the OLDEST queued batch and hands its mnemonics and secrets back.
    bool wait(std::vector<std::string> &mnemonics,
              std::vector<uint8_t> &secrets, std::string *err);
    int  inFlight() const { return (int)queue_.size(); }

    double lastKernelSeconds() const { return lastSeconds_; }
    const Info &info() const { return info_; }

private:
    bool ensureBuffers(size_t poolBytes, size_t count);

    // One pipeline slot: device buffers + host staging that must outlive the
    // non-blocking writes.
    struct Slot {
        cl_mem in = nullptr, lens = nullptr, out = nullptr;
        size_t capIn = 0, capLens = 0, capOut = 0;
        std::vector<uint8_t>     hostPool;
        std::vector<uint32_t>    hostLens;
        std::vector<std::string> mnemonics;
        size_t count = 0;
    };
    bool ensureSlot(Slot &s, size_t needMnem, size_t needLens, size_t needOut, std::string *err);
    void releaseSlots();

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
    std::array<Slot, kDepth> slots_{};
    std::deque<size_t>       queue_;
};

} // namespace gpu

#endif // WALLET_TG_MINER_GPU_OPENCL_H
