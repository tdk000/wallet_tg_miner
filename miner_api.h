// ===========================================================================
//  miner_api.h - embeddable C API for wallet_tg_miner.
//
//  The desktop build keeps its own main()/CLI; the Android build compiles the
//  same core with -DWTM_LIB and talks to it through this small C surface.
//  No C++ types cross the boundary.
// ===========================================================================
#ifndef WALLET_TG_MINER_API_H
#define WALLET_TG_MINER_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *mode;       /* "phrase" (default) | "subwallet" */
    const char *scheme;     /* "app" (default) | "ton"           */
    const char *suffix;     /* matched at the end of the address  */
    const char *prefix;     /* matched right after the tag (idx 2)*/
    int   matchAt;          /* explicit base64 index, -1 = auto   */
    int   ci;               /* case-insensitive matching          */
    int   testnet;
    int   words;            /* 12 or 24                           */
    int   threads;          /* 0 = auto (hardware_concurrency)    */
    int   gpu;              /* allow OpenCL when applicable       */
    unsigned subwalletId;   /* used only when haveSubwallet       */
    int   haveSubwallet;
    const char *mnemonic;   /* optional fixed mnemonic            */
} wtm_params;

typedef struct {
    int      found;         /* 1 = match, 0 = stopped/exhausted, -1 = error */
    char     error[256];
    char     address[64];   /* friendly base64url address   */
    char     raw[80];       /* 0:<hex> raw form             */
    char     mnemonic[512];
    char     pubkey[80];    /* hex                          */
    uint32_t subwalletId;
    uint64_t attempts;
    double   elapsed;
    int      usesGpu;
    int      importable;    /* 1 = a standard client will show this address */
} wtm_result;

typedef struct {
    int                status;   /* 1 = found/ok, 0 = not found, -1 = error */
    unsigned long long index;    /* stream index (found / demo)             */
    char               mnemonic[512];
    char               address[64];
    char               error[256];
} wtm_fallback_result;

/* Fill *p with safe defaults (phrase/app/12 words/no gpu). */
void wtm_defaults(wtm_params *p);

/* Run a search to completion (blocking).  Poll wtm_attempts() for progress;
   call wtm_request_stop() from another thread to abort.  Returns the same
   value as out->found. */
int wtm_run(const wtm_params *p, wtm_result *out);

/* Monotonic attempt counter for the running (or last) search. */
uint64_t wtm_attempts(void);

/* Cooperative stop request. */
void wtm_request_stop(void);

/* Built-in self-tests (1 = ok). */
int wtm_selftest(void);

/* Expected number of attempts for a pattern (probability model). */
double wtm_expected_attempts(const char *pattern, int ci);

/* Was `mnemonic` produced by the old insecure deterministic fallback
 * (12-word app phrases only)?  Scans the first `limit` phrases.
 * 1 = found (out->index), 0 = not found, -1 = error (out->error). */
int wtm_check_fallback_phrase(const char *mnemonic, unsigned long long limit,
                              wtm_fallback_result *out);

/* Developer diagnostics: reproduce the phrase/address the old buggy fallback
 * would emit at `index`.  Insecure by construction -- never use for funds. */
int wtm_insecure_demo_fallback(unsigned long long index, wtm_fallback_result *out);

#ifdef __cplusplus
}
#endif
#endif /* WALLET_TG_MINER_API_H */
