// ===========================================================================
//  ocl.h - minimal OpenCL declarations for wallet_tg_miner.
//
//  The project deliberately avoids the OpenCL SDK: only the small subset of
//  the API we actually use is declared here, and every entry point is
//  resolved at run time from OpenCL.dll / libOpenCL.so (see gpu_opencl.cpp).
//  That keeps the binary buildable with no extra headers, no import library
//  and no link dependency -- if there is no OpenCL runtime the miner simply
//  falls back to the CPU.
// ===========================================================================
#ifndef WALLET_TG_MINER_OCL_H
#define WALLET_TG_MINER_OCL_H

#include <cstddef>
#include <cstdint>

// ---- scalar types ---------------------------------------------------------
typedef int32_t   cl_int;
typedef uint32_t  cl_uint;
typedef uint64_t  cl_ulong;
typedef int64_t   cl_long;
typedef uint32_t  cl_bool;
typedef uint64_t  cl_bitfield;
typedef cl_bitfield cl_device_type;
typedef cl_bitfield cl_mem_flags;
typedef cl_bitfield cl_command_queue_properties;
typedef cl_uint   cl_platform_info;
typedef cl_uint   cl_device_info;
typedef cl_uint   cl_program_build_info;
typedef intptr_t  cl_context_properties;
typedef size_t    cl_event_info;

// ---- opaque handles -------------------------------------------------------
typedef struct _cl_platform_id   *cl_platform_id;
typedef struct _cl_device_id     *cl_device_id;
typedef struct _cl_context       *cl_context;
typedef struct _cl_command_queue *cl_command_queue;
typedef struct _cl_mem           *cl_mem;
typedef struct _cl_program       *cl_program;
typedef struct _cl_kernel        *cl_kernel;
typedef struct _cl_event         *cl_event;

// ---- error codes ----------------------------------------------------------
#define CL_SUCCESS            0
#define CL_DEVICE_NOT_FOUND   (-1)
#define CL_DEVICE_NOT_AVAILABLE (-2)
#define CL_INVALID_VALUE      (-30)
#define CL_INVALID_DEVICE     (-33)
#define CL_BUILD_PROGRAM_FAILURE (-11)

// ---- enumerations ---------------------------------------------------------
#define CL_PLATFORM_NAME                    0x0902
#define CL_PLATFORM_VENDOR                  0x0903
#define CL_PLATFORM_VERSION                 0x0901

#define CL_DEVICE_TYPE                      0x1000
#define CL_DEVICE_TYPE_DEFAULT              (1 << 0)
#define CL_DEVICE_TYPE_CPU                  (1 << 1)
#define CL_DEVICE_TYPE_GPU                  (1 << 2)
#define CL_DEVICE_TYPE_ACCELERATOR          (1 << 3)
#define CL_DEVICE_TYPE_ALL                  0xFFFFFFFF

#define CL_DEVICE_NAME                      0x102B
#define CL_DEVICE_VENDOR                    0x102C
#define CL_DRIVER_VERSION                   0x102D
#define CL_DEVICE_VERSION                   0x102F
#define CL_DEVICE_MAX_COMPUTE_UNITS         0x1002
#define CL_DEVICE_MAX_CLOCK_FREQUENCY       0x100C
#define CL_DEVICE_GLOBAL_MEM_SIZE           0x101F
#define CL_DEVICE_LOCAL_MEM_SIZE            0x1023
#define CL_DEVICE_MAX_WORK_GROUP_SIZE       0x1004
#define CL_DEVICE_MAX_WORK_ITEM_SIZES       0x1005

#define CL_CONTEXT_PLATFORM                 0x1084

#define CL_MEM_READ_WRITE                   (1 << 0)
#define CL_MEM_WRITE_ONLY                   (1 << 1)
#define CL_MEM_READ_ONLY                    (1 << 2)
#define CL_MEM_USE_HOST_PTR                 (1 << 3)
#define CL_MEM_ALLOC_HOST_PTR               (1 << 4)
#define CL_MEM_COPY_HOST_PTR                (1 << 5)

#define CL_TRUE                             1
#define CL_FALSE                            0

#define CL_PROGRAM_BUILD_LOG                0x1183
#define CL_QUEUE_PROFILING_ENABLE           (1 << 1)

#define CL_PROFILING_COMMAND_START          0x1286
#define CL_PROFILING_COMMAND_END            0x1287

typedef void (*cl_notify_fn)(const char *, const void *, size_t, void *);

// ---- function pointer types ----------------------------------------------
typedef cl_int  (*PFN_clGetPlatformIDs)(cl_uint, cl_platform_id *, cl_uint *);
typedef cl_int  (*PFN_clGetPlatformInfo)(cl_platform_id, cl_platform_info, size_t, void *, size_t *);
typedef cl_int  (*PFN_clGetDeviceIDs)(cl_platform_id, cl_device_type, cl_uint, cl_device_id *, cl_uint *);
typedef cl_int  (*PFN_clGetDeviceInfo)(cl_device_id, cl_device_info, size_t, void *, size_t *);
typedef cl_context (*PFN_clCreateContext)(const cl_context_properties *, cl_uint, const cl_device_id *,
                                          cl_notify_fn, void *, cl_int *);
typedef cl_command_queue (*PFN_clCreateCommandQueue)(cl_context, cl_device_id,
                                                     cl_command_queue_properties, cl_int *);
typedef cl_mem  (*PFN_clCreateBuffer)(cl_context, cl_mem_flags, size_t, void *, cl_int *);
typedef cl_program (*PFN_clCreateProgramWithSource)(cl_context, cl_uint, const char **,
                                                    const size_t *, cl_int *);
typedef cl_int  (*PFN_clBuildProgram)(cl_program, cl_uint, const cl_device_id *, const char *,
                                      cl_notify_fn, void *);
typedef cl_int  (*PFN_clGetProgramBuildInfo)(cl_program, cl_device_id, cl_program_build_info,
                                             size_t, void *, size_t *);
typedef cl_kernel (*PFN_clCreateKernel)(cl_program, const char *, cl_int *);
typedef cl_int  (*PFN_clSetKernelArg)(cl_kernel, cl_uint, size_t, const void *);
typedef cl_int  (*PFN_clEnqueueNDRangeKernel)(cl_command_queue, cl_kernel, cl_uint, const size_t *,
                                              const size_t *, const size_t *, cl_uint,
                                              const cl_event *, cl_event *);
typedef cl_int  (*PFN_clEnqueueWriteBuffer)(cl_command_queue, cl_mem, cl_bool, size_t, size_t,
                                            const void *, cl_uint, const cl_event *, cl_event *);
typedef cl_int  (*PFN_clEnqueueReadBuffer)(cl_command_queue, cl_mem, cl_bool, size_t, size_t,
                                           void *, cl_uint, const cl_event *, cl_event *);
typedef cl_int  (*PFN_clFinish)(cl_command_queue);
typedef cl_int  (*PFN_clReleaseMemObject)(cl_mem);
typedef cl_int  (*PFN_clReleaseKernel)(cl_kernel);
typedef cl_int  (*PFN_clReleaseProgram)(cl_program);
typedef cl_int  (*PFN_clReleaseCommandQueue)(cl_command_queue);
typedef cl_int  (*PFN_clReleaseContext)(cl_context);
typedef cl_int  (*PFN_clWaitForEvents)(cl_uint, const cl_event *);
typedef cl_int  (*PFN_clReleaseEvent)(cl_event);
typedef cl_int  (*PFN_clGetEventProfilingInfo)(cl_event, cl_uint, size_t, void *, size_t *);

// ---- resolved API ---------------------------------------------------------
struct OclApi {
    void *lib = nullptr;                 // HMODULE / dlopen handle

    PFN_clGetPlatformIDs          GetPlatformIDs          = nullptr;
    PFN_clGetPlatformInfo         GetPlatformInfo         = nullptr;
    PFN_clGetDeviceIDs            GetDeviceIDs            = nullptr;
    PFN_clGetDeviceInfo           GetDeviceInfo           = nullptr;
    PFN_clCreateContext           CreateContext           = nullptr;
    PFN_clCreateCommandQueue      CreateCommandQueue      = nullptr;
    PFN_clCreateBuffer            CreateBuffer            = nullptr;
    PFN_clCreateProgramWithSource CreateProgramWithSource = nullptr;
    PFN_clBuildProgram            BuildProgram            = nullptr;
    PFN_clGetProgramBuildInfo     GetProgramBuildInfo     = nullptr;
    PFN_clCreateKernel            CreateKernel            = nullptr;
    PFN_clSetKernelArg            SetKernelArg            = nullptr;
    PFN_clEnqueueNDRangeKernel    EnqueueNDRangeKernel    = nullptr;
    PFN_clEnqueueWriteBuffer      EnqueueWriteBuffer      = nullptr;
    PFN_clEnqueueReadBuffer       EnqueueReadBuffer       = nullptr;
    PFN_clFinish                  Finish                  = nullptr;
    PFN_clReleaseMemObject        ReleaseMemObject        = nullptr;
    PFN_clReleaseKernel           ReleaseKernel           = nullptr;
    PFN_clReleaseProgram          ReleaseProgram          = nullptr;
    PFN_clReleaseCommandQueue     ReleaseCommandQueue     = nullptr;
    PFN_clReleaseContext          ReleaseContext          = nullptr;
    PFN_clWaitForEvents           WaitForEvents           = nullptr;
    PFN_clReleaseEvent            ReleaseEvent            = nullptr;
    PFN_clGetEventProfilingInfo   GetEventProfilingInfo   = nullptr;

    // Resolve the whole API from the system OpenCL library.
    bool load();
    void unload();
};

#endif // WALLET_TG_MINER_OCL_H
