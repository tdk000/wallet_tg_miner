// ===========================================================================
//  gpu_opencl.cpp - OpenCL host side (loader + app-secret engine).
// ===========================================================================
#include "gpu_opencl.h"
#include "gpu_kernels.h"

#include <chrono>
#include <cstring>
#include <vector>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

// ---- runtime library (global scope: OclApi lives here) --------------------
static void *lib_open() {
#ifdef _WIN32
    return (void *)LoadLibraryA("OpenCL.dll");
#elif defined(__APPLE__)
    void *h = dlopen("/System/Library/Frameworks/OpenCL.framework/OpenCL", RTLD_NOW | RTLD_LOCAL);
    if (!h) h = dlopen("libOpenCL.dylib", RTLD_NOW | RTLD_LOCAL);
    return h;
#else
#  if defined(__ANDROID__)
    // Android ships the ICD as a vendor library that is not on the default
    // search path; try the well-known locations before the generic names.
    static const char *cands[] = {
        "/vendor/lib64/libOpenCL.so",
        "/vendor/lib/libOpenCL.so",
        "/system/vendor/lib64/libOpenCL.so",
        "/system/vendor/lib/libOpenCL.so",
        "/system/lib64/libOpenCL.so",
        "/system/lib/libOpenCL.so",
        "libOpenCL.so",
    };
    for (const char *c : cands) {
        void *h = dlopen(c, RTLD_NOW | RTLD_LOCAL);
        if (h) return h;
    }
    return nullptr;
#  else
    void *h = dlopen("libOpenCL.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!h) h = dlopen("libOpenCL.so", RTLD_NOW | RTLD_LOCAL);
    return h;
#  endif
#endif
}

static void lib_close(void *h) {
#ifdef _WIN32
    if (h) FreeLibrary((HMODULE)h);
#else
    if (h) dlclose(h);
#endif
}

static void *lib_sym(void *h, const char *name) {
#ifdef _WIN32
    return (void *)GetProcAddress((HMODULE)h, name);
#else
    return dlsym(h, name);
#endif
}

template <class T>
static bool resolve(void *lib, const char *name, T &fn) {
    fn = (T)lib_sym(lib, name);
    return fn != nullptr;
}

bool OclApi::load() {
    if (lib) return true;
    lib = lib_open();
    if (!lib) return false;

    bool ok = true;
    ok &= resolve(lib, "clGetPlatformIDs",          GetPlatformIDs);
    ok &= resolve(lib, "clGetPlatformInfo",         GetPlatformInfo);
    ok &= resolve(lib, "clGetDeviceIDs",            GetDeviceIDs);
    ok &= resolve(lib, "clGetDeviceInfo",           GetDeviceInfo);
    ok &= resolve(lib, "clCreateContext",           CreateContext);
    ok &= resolve(lib, "clCreateCommandQueue",      CreateCommandQueue);
    ok &= resolve(lib, "clCreateBuffer",            CreateBuffer);
    ok &= resolve(lib, "clCreateProgramWithSource", CreateProgramWithSource);
    ok &= resolve(lib, "clBuildProgram",            BuildProgram);
    ok &= resolve(lib, "clGetProgramBuildInfo",     GetProgramBuildInfo);
    ok &= resolve(lib, "clCreateKernel",            CreateKernel);
    ok &= resolve(lib, "clSetKernelArg",            SetKernelArg);
    ok &= resolve(lib, "clEnqueueNDRangeKernel",    EnqueueNDRangeKernel);
    ok &= resolve(lib, "clEnqueueWriteBuffer",      EnqueueWriteBuffer);
    ok &= resolve(lib, "clEnqueueReadBuffer",       EnqueueReadBuffer);
    ok &= resolve(lib, "clFinish",                  Finish);
    ok &= resolve(lib, "clReleaseMemObject",        ReleaseMemObject);
    ok &= resolve(lib, "clReleaseKernel",           ReleaseKernel);
    ok &= resolve(lib, "clReleaseProgram",          ReleaseProgram);
    ok &= resolve(lib, "clReleaseCommandQueue",     ReleaseCommandQueue);
    ok &= resolve(lib, "clReleaseContext",          ReleaseContext);
    // optional (only used for profiling / waiting)
    resolve(lib, "clWaitForEvents",          WaitForEvents);
    resolve(lib, "clReleaseEvent",           ReleaseEvent);
    resolve(lib, "clGetEventProfilingInfo",  GetEventProfilingInfo);

    if (!ok) { unload(); return false; }
    return true;
}

void OclApi::unload() {
    GetPlatformIDs = nullptr; GetPlatformInfo = nullptr;
    GetDeviceIDs = nullptr;   GetDeviceInfo = nullptr;
    CreateContext = nullptr;  CreateCommandQueue = nullptr;
    CreateBuffer = nullptr;   CreateProgramWithSource = nullptr;
    BuildProgram = nullptr;   GetProgramBuildInfo = nullptr;
    CreateKernel = nullptr;   SetKernelArg = nullptr;
    EnqueueNDRangeKernel = nullptr; EnqueueWriteBuffer = nullptr; EnqueueReadBuffer = nullptr;
    Finish = nullptr; ReleaseMemObject = nullptr; ReleaseKernel = nullptr;
    ReleaseProgram = nullptr; ReleaseCommandQueue = nullptr; ReleaseContext = nullptr;
    WaitForEvents = nullptr; ReleaseEvent = nullptr; GetEventProfilingInfo = nullptr;
    if (lib) lib_close(lib);
    lib = nullptr;
}

namespace gpu {

// ---- device discovery -----------------------------------------------------
static std::string dev_str(OclApi &api, cl_device_id d, cl_device_info p) {
    size_t n = 0;
    if (api.GetDeviceInfo(d, p, 0, nullptr, &n) != CL_SUCCESS || n == 0) return std::string();
    std::string s(n, '\0');
    if (api.GetDeviceInfo(d, p, n, &s[0], nullptr) != CL_SUCCESS) return std::string();
    while (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

static std::string plat_str(OclApi &api, cl_platform_id p, cl_platform_info q) {
    size_t n = 0;
    if (api.GetPlatformInfo(p, q, 0, nullptr, &n) != CL_SUCCESS || n == 0) return std::string();
    std::string s(n, '\0');
    if (api.GetPlatformInfo(p, q, n, &s[0], nullptr) != CL_SUCCESS) return std::string();
    while (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

static bool find_gpu(OclApi &api, cl_platform_id &plat, cl_device_id &dev, Info &info) {
    cl_uint np = 0;
    if (api.GetPlatformIDs(0, nullptr, &np) != CL_SUCCESS || np == 0) {
        info.error = "no OpenCL platform"; return false;
    }
    std::vector<cl_platform_id> plats(np);
    if (api.GetPlatformIDs(np, plats.data(), nullptr) != CL_SUCCESS) {
        info.error = "clGetPlatformIDs failed"; return false;
    }
    for (cl_uint i = 0; i < np; i++) {
        cl_uint nd = 0;
        if (api.GetDeviceIDs(plats[i], CL_DEVICE_TYPE_GPU, 0, nullptr, &nd) != CL_SUCCESS || nd == 0)
            continue;
        std::vector<cl_device_id> devs(nd);
        if (api.GetDeviceIDs(plats[i], CL_DEVICE_TYPE_GPU, nd, devs.data(), nullptr) != CL_SUCCESS)
            continue;
        plat = plats[i];
        dev  = devs[0];
        return true;
    }
    info.error = "no OpenCL GPU device found";
    return false;
}

static void fill_info(OclApi &api, cl_platform_id plat, cl_device_id dev, Info &info) {
    info.platform = plat_str(api, plat, CL_PLATFORM_NAME);
    info.device   = dev_str(api, dev, CL_DEVICE_NAME);
    info.vendor   = dev_str(api, dev, CL_DEVICE_VENDOR);
    info.driver   = dev_str(api, dev, CL_DRIVER_VERSION);
    info.version  = dev_str(api, dev, CL_DEVICE_VERSION);
    cl_uint  cu = 0; api.GetDeviceInfo(dev, CL_DEVICE_MAX_COMPUTE_UNITS,   sizeof(cu), &cu, nullptr);
    cl_ulong gm = 0; api.GetDeviceInfo(dev, CL_DEVICE_GLOBAL_MEM_SIZE,     sizeof(gm), &gm, nullptr);
    size_t   wg = 0; api.GetDeviceInfo(dev, CL_DEVICE_MAX_WORK_GROUP_SIZE, sizeof(wg), &wg, nullptr);
    info.computeUnits = cu;
    info.globalMem    = gm;
    info.maxWorkGroup = wg;
    info.ok = true;
}

Info probe() {
    Info info;
    OclApi api;
    if (!api.load()) { info.error = "OpenCL runtime not available"; return info; }
    cl_platform_id plat = nullptr; cl_device_id dev = nullptr;
    if (find_gpu(api, plat, dev, info)) fill_info(api, plat, dev, info);
    api.unload();
    return info;
}

// ---- AppDeriver -----------------------------------------------------------
AppDeriver::~AppDeriver() { shutdown(); }

bool AppDeriver::init(std::string *err) {
    if (kern_) return true;
    if (!api_.load()) { if (err) *err = "OpenCL runtime not available"; return false; }

    cl_platform_id plat = nullptr; cl_device_id dev = nullptr;
    if (!find_gpu(api_, plat, dev, info_)) {
        if (err) *err = info_.error.empty() ? "no OpenCL GPU" : info_.error;
        shutdown();
        return false;
    }
    fill_info(api_, plat, dev, info_);

    cl_int rc = CL_SUCCESS;
    cl_context_properties props[] = { CL_CONTEXT_PLATFORM, (cl_context_properties)plat, 0 };
    ctx_ = api_.CreateContext(props, 1, &dev, nullptr, nullptr, &rc);
    if (!ctx_ || rc != CL_SUCCESS) {
        if (err) *err = "clCreateContext failed";
        shutdown(); return false;
    }
    q_ = api_.CreateCommandQueue(ctx_, dev, 0, &rc);
    if (!q_ || rc != CL_SUCCESS) {
        if (err) *err = "clCreateCommandQueue failed";
        shutdown(); return false;
    }

    const char *src = GPU_APP_KERNEL_SRC;
    size_t slen = std::strlen(src);
    prog_ = api_.CreateProgramWithSource(ctx_, 1, &src, &slen, &rc);
    if (!prog_ || rc != CL_SUCCESS) {
        if (err) *err = "clCreateProgramWithSource failed";
        shutdown(); return false;
    }
    rc = api_.BuildProgram(prog_, 1, &dev, "", nullptr, nullptr);
    if (rc != CL_SUCCESS) {
        size_t lsz = 0;
        api_.GetProgramBuildInfo(prog_, dev, CL_PROGRAM_BUILD_LOG, 0, nullptr, &lsz);
        std::string log(lsz ? lsz : 1, '\0');
        api_.GetProgramBuildInfo(prog_, dev, CL_PROGRAM_BUILD_LOG, lsz, &log[0], nullptr);
        if (err) *err = "OpenCL kernel build failed:\n" + log;
        shutdown(); return false;
    }
    kern_ = api_.CreateKernel(prog_, "app_derive", &rc);
    if (!kern_ || rc != CL_SUCCESS) {
        if (err) *err = "clCreateKernel failed";
        shutdown(); return false;
    }
    return true;
}

void AppDeriver::shutdown() {
    releaseSlots();
    if (api_.ReleaseMemObject) {
        if (bufMnem_) { api_.ReleaseMemObject(bufMnem_); bufMnem_ = nullptr; }
        if (bufLens_) { api_.ReleaseMemObject(bufLens_); bufLens_ = nullptr; }
        if (bufOut_)  { api_.ReleaseMemObject(bufOut_);  bufOut_  = nullptr; }
    }
    capMnem_ = capLens_ = capOut_ = 0;
    if (api_.ReleaseKernel && kern_) { api_.ReleaseKernel(kern_); kern_ = nullptr; }
    if (api_.ReleaseProgram && prog_) { api_.ReleaseProgram(prog_); prog_ = nullptr; }
    if (api_.ReleaseCommandQueue && q_) { api_.ReleaseCommandQueue(q_); q_ = nullptr; }
    if (api_.ReleaseContext && ctx_) { api_.ReleaseContext(ctx_); ctx_ = nullptr; }
    if (api_.lib) api_.unload();
}

bool AppDeriver::derive(const std::vector<std::string> &mn,
                        std::vector<uint8_t> &secrets, std::string *err) {
    if (!kern_) { if (err) *err = "GPU not initialised"; return false; }

    const size_t count = mn.size();
    secrets.assign(count * 32, 0);
    if (count == 0) return true;

    size_t maxlen = 0;
    for (const std::string &s : mn) if (s.size() > maxlen) maxlen = s.size();
    const uint32_t stride = (uint32_t)(maxlen ? maxlen : 1);

    std::vector<uint8_t> pool(count * stride);
    std::vector<cl_uint> lens(count);
    for (size_t i = 0; i < count; i++) {
        std::memcpy(&pool[i * stride], mn[i].data(), mn[i].size());
        lens[i] = (cl_uint)mn[i].size();
    }

    cl_int rc = CL_SUCCESS;
    const size_t needMnem = pool.size();
    const size_t needLens = count * sizeof(cl_uint);
    const size_t needOut  = count * 32;

    if (needMnem > capMnem_) {
        if (bufMnem_) api_.ReleaseMemObject(bufMnem_);
        bufMnem_ = api_.CreateBuffer(ctx_, CL_MEM_READ_ONLY, needMnem, nullptr, &rc);
        if (!bufMnem_ || rc != CL_SUCCESS) { if (err) *err = "clCreateBuffer(mnem) failed"; return false; }
        capMnem_ = needMnem;
    }
    if (needLens > capLens_) {
        if (bufLens_) api_.ReleaseMemObject(bufLens_);
        bufLens_ = api_.CreateBuffer(ctx_, CL_MEM_READ_ONLY, needLens, nullptr, &rc);
        if (!bufLens_ || rc != CL_SUCCESS) { if (err) *err = "clCreateBuffer(lens) failed"; return false; }
        capLens_ = needLens;
    }
    if (needOut > capOut_) {
        if (bufOut_) api_.ReleaseMemObject(bufOut_);
        bufOut_ = api_.CreateBuffer(ctx_, CL_MEM_WRITE_ONLY, needOut, nullptr, &rc);
        if (!bufOut_ || rc != CL_SUCCESS) { if (err) *err = "clCreateBuffer(out) failed"; return false; }
        capOut_ = needOut;
    }

    api_.EnqueueWriteBuffer(q_, bufMnem_, CL_TRUE, 0, needMnem, pool.data(), 0, nullptr, nullptr);
    api_.EnqueueWriteBuffer(q_, bufLens_, CL_TRUE, 0, needLens, lens.data(), 0, nullptr, nullptr);

    cl_uint cnt = (cl_uint)count;
    api_.SetKernelArg(kern_, 0, sizeof(cl_mem),  &bufMnem_);
    api_.SetKernelArg(kern_, 1, sizeof(cl_mem),  &bufLens_);
    api_.SetKernelArg(kern_, 2, sizeof(cl_uint), &stride);
    api_.SetKernelArg(kern_, 3, sizeof(cl_mem),  &bufOut_);
    api_.SetKernelArg(kern_, 4, sizeof(cl_uint), &cnt);

    size_t local = 64;
    if (info_.maxWorkGroup && local > info_.maxWorkGroup) local = info_.maxWorkGroup;
    if (local == 0) local = 1;
    size_t global = ((count + local - 1) / local) * local;

    auto t0 = std::chrono::steady_clock::now();
    rc = api_.EnqueueNDRangeKernel(q_, kern_, 1, nullptr, &global, &local, 0, nullptr, nullptr);
    if (rc != CL_SUCCESS) { if (err) *err = "clEnqueueNDRangeKernel failed"; return false; }
    rc = api_.Finish(q_);
    auto t1 = std::chrono::steady_clock::now();
    if (rc != CL_SUCCESS) { if (err) *err = "clFinish failed"; return false; }
    lastSeconds_ = std::chrono::duration<double>(t1 - t0).count();

    rc = api_.EnqueueReadBuffer(q_, bufOut_, CL_TRUE, 0, needOut, secrets.data(), 0, nullptr, nullptr);
    if (rc != CL_SUCCESS) { if (err) *err = "clEnqueueReadBuffer failed"; return false; }
    return true;
}

// ---- pipelined mode ------------------------------------------------------
//
// The synchronous derive() above leaves the GPU idle while the caller derives
// ed25519 keys for the batch it just received (on a 2-core machine that is
// 2.0 s of CPU work against 0.75 s of GPU work).  submit()/wait() let the
// caller keep up to kDepth batches queued: wait() blocks only until the OLDEST
// kernel finishes, and everything queued behind it keeps running.
//
// All writes are non-blocking, so the host staging buffers must stay alive
// until the slot's read has completed - they live in the Slot itself.

void AppDeriver::releaseSlots() {
    if (api_.ReleaseMemObject) {
        for (Slot &s : slots_) {
            if (s.in)   { api_.ReleaseMemObject(s.in);   s.in = nullptr; }
            if (s.lens) { api_.ReleaseMemObject(s.lens); s.lens = nullptr; }
            if (s.out)  { api_.ReleaseMemObject(s.out);  s.out = nullptr; }
            s.capIn = s.capLens = s.capOut = 0;
        }
    }
    queue_.clear();
}

bool AppDeriver::ensureSlot(Slot &s, size_t needMnem, size_t needLens, size_t needOut, std::string *err) {
    cl_int rc = CL_SUCCESS;
    if (needMnem > s.capIn) {
        if (s.in) api_.ReleaseMemObject(s.in);
        s.in = api_.CreateBuffer(ctx_, CL_MEM_READ_ONLY, needMnem, nullptr, &rc);
        if (!s.in || rc != CL_SUCCESS) { s.capIn = 0; if (err) *err = "clCreateBuffer(mnem) failed"; return false; }
        s.capIn = needMnem;
    }
    if (needLens > s.capLens) {
        if (s.lens) api_.ReleaseMemObject(s.lens);
        s.lens = api_.CreateBuffer(ctx_, CL_MEM_READ_ONLY, needLens, nullptr, &rc);
        if (!s.lens || rc != CL_SUCCESS) { s.capLens = 0; if (err) *err = "clCreateBuffer(lens) failed"; return false; }
        s.capLens = needLens;
    }
    if (needOut > s.capOut) {
        if (s.out) api_.ReleaseMemObject(s.out);
        s.out = api_.CreateBuffer(ctx_, CL_MEM_WRITE_ONLY, needOut, nullptr, &rc);
        if (!s.out || rc != CL_SUCCESS) { s.capOut = 0; if (err) *err = "clCreateBuffer(out) failed"; return false; }
        s.capOut = needOut;
    }
    return true;
}

bool AppDeriver::submit(std::vector<std::string> &&mnemonics, std::string *err) {
    if (!kern_) { if (err) *err = "GPU not initialised"; return false; }
    if ((int)queue_.size() >= kDepth) { if (err) *err = "GPU pipeline is full"; return false; }

    const size_t count = mnemonics.size();
    if (count == 0) return true;

    // Find a slot that is not in flight.
    size_t si = (size_t)-1;
    for (size_t i = 0; i < slots_.size() && si == (size_t)-1; i++) {
        bool used = false;
        for (size_t q : queue_) if (q == i) { used = true; break; }
        if (!used) si = i;
    }
    if (si == (size_t)-1) { if (err) *err = "GPU pipeline is full"; return false; }
    Slot &s = slots_[si];

    size_t maxlen = 0;
    for (const std::string &x : mnemonics) if (x.size() > maxlen) maxlen = x.size();
    const uint32_t stride = (uint32_t)(maxlen ? maxlen : 1);

    s.hostPool.assign(count * stride, 0);
    s.hostLens.resize(count);
    for (size_t i = 0; i < count; i++) {
        std::memcpy(&s.hostPool[i * stride], mnemonics[i].data(), mnemonics[i].size());
        s.hostLens[i] = (cl_uint)mnemonics[i].size();
    }

    const size_t needMnem = s.hostPool.size();
    const size_t needLens = count * sizeof(cl_uint);
    const size_t needOut  = count * 32;
    if (!ensureSlot(s, needMnem, needLens, needOut, err)) return false;

    // Non-blocking writes: the data is copied by the runtime while the kernel
    // of a previous batch is still running.
    api_.EnqueueWriteBuffer(q_, s.in,   CL_FALSE, 0, needMnem, s.hostPool.data(), 0, nullptr, nullptr);
    api_.EnqueueWriteBuffer(q_, s.lens, CL_FALSE, 0, needLens, s.hostLens.data(), 0, nullptr, nullptr);

    cl_uint cnt = (cl_uint)count;
    api_.SetKernelArg(kern_, 0, sizeof(cl_mem),  &s.in);
    api_.SetKernelArg(kern_, 1, sizeof(cl_mem),  &s.lens);
    api_.SetKernelArg(kern_, 2, sizeof(cl_uint), &stride);
    api_.SetKernelArg(kern_, 3, sizeof(cl_mem),  &s.out);
    api_.SetKernelArg(kern_, 4, sizeof(cl_uint), &cnt);

    size_t local = 64;
    if (info_.maxWorkGroup && local > info_.maxWorkGroup) local = info_.maxWorkGroup;
    if (local == 0) local = 1;
    size_t global = ((count + local - 1) / local) * local;

    cl_int rc = api_.EnqueueNDRangeKernel(q_, kern_, 1, nullptr, &global, &local, 0, nullptr, nullptr);
    if (rc != CL_SUCCESS) { if (err) *err = "clEnqueueNDRangeKernel failed"; return false; }

    s.count = count;
    s.mnemonics = std::move(mnemonics);
    queue_.push_back(si);
    return true;
}

bool AppDeriver::wait(std::vector<std::string> &mnemonics, std::vector<uint8_t> &secrets, std::string *err) {
    if (queue_.empty()) { if (err) *err = "nothing in flight"; return false; }
    const size_t si = queue_.front();
    queue_.pop_front();
    Slot &s = slots_[si];

    secrets.assign(s.count * 32, 0);
    auto t0 = std::chrono::steady_clock::now();
    // Blocking read of the OLDEST batch.  The queue is in-order, so this waits
    // for this slot's kernel only - later batches keep the GPU busy.
    cl_int rc = api_.EnqueueReadBuffer(q_, s.out, CL_TRUE, 0, s.count * 32, secrets.data(), 0, nullptr, nullptr);
    auto t1 = std::chrono::steady_clock::now();
    lastSeconds_ = std::chrono::duration<double>(t1 - t0).count();
    if (rc != CL_SUCCESS) { if (err) *err = "clEnqueueReadBuffer failed"; return false; }

    mnemonics = std::move(s.mnemonics);
    s.mnemonics.clear();
    s.count = 0;
    return true;
}

} // namespace gpu
