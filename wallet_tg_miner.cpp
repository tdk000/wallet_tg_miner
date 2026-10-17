// ============================================================================
//  wallet_tg_miner.cpp  —  portable beauty-address miner for Telegram Wallet (WalletTg)
// ----------------------------------------------------------------------------
//  Generates WalletTg "beautiful" addresses entirely offline.
//
//  An account address is:
//      hash = SHA256( StateInit{ code = WalletTrampoline, data = <storage> } )
//      storage = 0x00 | seqno:uint32 | subwalletId:uint32 | publicKey:bits256
//      friendly = base64url( tag:u8 | workchain:i8 | hash:bits256 | crc16:u16 )
//
//  Two search modes:
//    phrase     : vary the 12/24-word BIP39 mnemonic (subwalletId fixed).
//                 Slower per try (PBKDF2-2048 + SLIP-0010 + ed25519) but the
//                 resulting address IS reproducible from the mnemonic -- this is
//                 the mode to use when you want to import the phrase into Telegram.
//    subwallet  : keep the mnemonic/key fixed and vary subwalletId (2^32 space).
//                 Only a SHA-256 + CRC + base64 per try -> 1000x faster, but the
//                 address is NOT reproducible from the mnemonic alone: a standard
//                 client (Telegram Wallet) needs the same custom subwalletId.
//
//  Crypto: self-contained SHA-256 / SHA-512 / HMAC-SHA512 / PBKDF2 (this file),
//          Ed25519 public-key derivation via Monocypher's group primitives
//          (Monocypher is CC0 / BSD-2-Clause), BIP39 wordlist (public domain).
//
//  Build:
//      zig c++ -std=c++17 -O3 -o wallet_tg_miner wallet_tg_miner.cpp monocypher.c
//      g++     -std=c++17 -O3 -pthread -o wallet_tg_miner wallet_tg_miner.cpp monocypher.c
//
//  This tool is for the owner of the key. It never talks to the network.
// ============================================================================

#define _CRT_SECURE_NO_WARNINGS
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <mutex>
#include <algorithm>
#include <cmath>

#include "monocypher.h"
#include "bip39_english.h"
#include "gpu_opencl.h"
#include "miner_api.h"

// UI language for the interactive wizard and the legacy check/demo output.
// Default is Russian (matches the historical behaviour of the CLI).
enum Lang { LANG_RU = 0, LANG_EN = 1, LANG_CN = 2 };
static Lang g_lang = LANG_RU;
static const char *L3(const char *ru, const char *en, const char *cn) {
    switch (g_lang) { case LANG_EN: return en; case LANG_CN: return cn; default: return ru; }
}
static Lang parse_lang(const std::string &s) {
    if (s == "en" || s == "EN" || s == "english" || s == "English") return LANG_EN;
    if (s == "cn" || s == "CN" || s == "zh" || s == "ZH" || s == "chinese" || s == "Chinese" || s == "zh-cn" || s == "zh_CN") return LANG_CN;
    return LANG_RU;
}
// ===========================================================================
//  Platform entropy (used only to seed the per-thread ChaCha20 DRBG)
// ===========================================================================
#if defined(_WIN32)
   // RtlGenRandom from advapi32 (no windows.h needed -> keeps the link tiny)
   extern "C" __declspec(dllimport) unsigned char __stdcall
   SystemFunction036(void *buf, unsigned long len);
   static bool os_random(void *buf, size_t len) {
       return SystemFunction036(buf, (unsigned long)len) != 0;
   }
   // kernel32: how many processes share our console? If it's just us, the user
   // double-clicked the .exe and the window would vanish instantly.
   extern "C" __declspec(dllimport) unsigned long __stdcall
   GetConsoleProcessList(unsigned long *lpdwProcessList, unsigned long dwProcessCount);
   // kernel32 console handle/mode, used only for the optional ANSI-colour check.
   extern "C" __declspec(dllimport) void *__stdcall GetStdHandle(unsigned long nStdHandle);
   extern "C" __declspec(dllimport) int   __stdcall GetConsoleMode(void *hConsoleHandle, unsigned long *lpMode);
   extern "C" __declspec(dllimport) int   __stdcall SetConsoleMode(void *hConsoleHandle, unsigned long dwMode);
   // kernel32 console code pages.  A Windows console starts on an OEM code page
   // (866 on a Russian system) while every message in this program is UTF-8, so
   // the Russian text came out as mojibake.  Switch the console to UTF-8 for the
   // duration of the run and put the old code page back on exit.  Redirected
   // output (a pipe or a file) is left alone: it always gets the raw UTF-8 bytes.
   extern "C" __declspec(dllimport) unsigned int __stdcall GetConsoleOutputCP(void);
   extern "C" __declspec(dllimport) int          __stdcall SetConsoleOutputCP(unsigned int id);
   extern "C" __declspec(dllimport) unsigned int __stdcall GetConsoleCP(void);
   extern "C" __declspec(dllimport) int          __stdcall SetConsoleCP(unsigned int id);
   static unsigned int g_saved_out_cp = 0, g_saved_in_cp = 0;
   static void restore_console_cp() {
       if (g_saved_out_cp) SetConsoleOutputCP(g_saved_out_cp);
       if (g_saved_in_cp)  SetConsoleCP(g_saved_in_cp);
   }
   static void setup_utf8_console() {
       if (getenv("WTM_NO_UTF8_CONSOLE")) return;      // escape hatch for scripts
       std::atexit(restore_console_cp);
       unsigned long mode = 0;
       void *ho = GetStdHandle((unsigned long)-11);    // STD_OUTPUT_HANDLE
       if (ho && GetConsoleMode(ho, &mode)) {
           unsigned int cp = GetConsoleOutputCP();
           if (cp && cp != 65001u) {                   // not UTF-8 yet
               g_saved_out_cp = cp;
               SetConsoleOutputCP(65001u);
           }
       }
       void *hi = GetStdHandle((unsigned long)-10);    // STD_INPUT_HANDLE
       if (hi && GetConsoleMode(hi, &mode)) {
           unsigned int cp = GetConsoleCP();
           if (cp && cp != 65001u) {
               g_saved_in_cp = cp;
               SetConsoleCP(65001u);
           }
       }
   }
   static bool launched_by_double_click() {
       unsigned long procs[4];
       unsigned long n = GetConsoleProcessList(procs, 4);
       return n <= 1;
   }
#else
#  include <fcntl.h>
#  include <unistd.h>
#  if defined(__linux__)
#    if !defined(__ANDROID__) || __ANDROID_API__ >= 28
#      define WTM_HAVE_GETRANDOM 1
#      include <sys/random.h>
#    endif
#  endif
   static bool os_random(void *buf, size_t len) {
       uint8_t *p = (uint8_t *)buf;
       size_t got = 0;
#  if defined(WTM_HAVE_GETRANDOM)
       while (got < len) {
           ssize_t n = getrandom(p + got, len - got, 0);
           if (n <= 0) break;
           got += (size_t)n;
       }
       if (got == len) return true;
       got = 0;
#  endif
       int fd = open("/dev/urandom", O_RDONLY);
       if (fd < 0) return false;
       while (got < len) {
           ssize_t n = read(fd, p + got, len - got);
           if (n <= 0) break;
           got += (size_t)n;
       }
       close(fd);
       return got == len;
   }
   static bool launched_by_double_click() { return false; }
   static void setup_utf8_console() {}              // UTF-8 is the default here
#endif

// ===========================================================================
//  SHA-256
// ===========================================================================
namespace {

const uint32_t SHA256_K[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};
inline uint32_t rotr32(uint32_t x, int n){ return (x >> n) | (x << (32 - n)); }

struct Sha256 {
    uint32_t h[8];
    uint64_t total;      // bytes
    uint8_t  buf[64];
    size_t   buflen;
};

void sha256_init(Sha256 *s){
    s->h[0]=0x6a09e667u; s->h[1]=0xbb67ae85u; s->h[2]=0x3c6ef372u; s->h[3]=0xa54ff53au;
    s->h[4]=0x510e527fu; s->h[5]=0x9b05688cu; s->h[6]=0x1f83d9abu; s->h[7]=0x5be0cd19u;
    s->total=0; s->buflen=0;
}

void sha256_block(uint32_t h[8], const uint8_t *p){
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
               ((uint32_t)p[i*4+2] << 8) | (uint32_t)p[i*4+3];
    for (int i = 16; i < 64; i++){
        uint32_t s0 = rotr32(w[i-15],7) ^ rotr32(w[i-15],18) ^ (w[i-15] >> 3);
        uint32_t s1 = rotr32(w[i-2],17) ^ rotr32(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
    for (int i = 0; i < 64; i++){
        uint32_t S1 = rotr32(e,6) ^ rotr32(e,11) ^ rotr32(e,25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + S1 + ch + SHA256_K[i] + w[i];
        uint32_t S0 = rotr32(a,2) ^ rotr32(a,13) ^ rotr32(a,22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
}

void sha256_update(Sha256 *s, const void *data, size_t len){
    const uint8_t *p = (const uint8_t *)data;
    s->total += len;
    if (s->buflen){
        size_t need = 64 - s->buflen;
        if (len < need){ memcpy(s->buf + s->buflen, p, len); s->buflen += len; return; }
        memcpy(s->buf + s->buflen, p, need);
        sha256_block(s->h, s->buf);
        p += need; len -= need; s->buflen = 0;
    }
    while (len >= 64){ sha256_block(s->h, p); p += 64; len -= 64; }
    if (len){ memcpy(s->buf, p, len); s->buflen = len; }
}

void sha256_final(Sha256 *s, uint8_t out[32]){
    uint64_t bits = s->total * 8;
    size_t padlen = (s->buflen < 56) ? (56 - s->buflen) : (120 - s->buflen);
    uint8_t pad[128];
    memset(pad, 0, padlen + 8);
    pad[0] = 0x80;
    for (int i = 0; i < 8; i++) pad[padlen + i] = (uint8_t)(bits >> (56 - i*8));
    sha256_update(s, pad, padlen + 8);
    for (int i = 0; i < 8; i++){
        out[i*4]   = (uint8_t)(s->h[i] >> 24);
        out[i*4+1] = (uint8_t)(s->h[i] >> 16);
        out[i*4+2] = (uint8_t)(s->h[i] >> 8);
        out[i*4+3] = (uint8_t)(s->h[i]);
    }
}

void sha256(const void *data, size_t len, uint8_t out[32]){
    Sha256 s; sha256_init(&s); sha256_update(&s, data, len); sha256_final(&s, out);
}

// ===========================================================================
//  SHA-512
// ===========================================================================
const uint64_t SHA512_K[80] = {
    0x428a2f98d728ae22ULL,0x7137449123ef65cdULL,0xb5c0fbcfec4d3b2fULL,0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL,0x59f111f1b605d019ULL,0x923f82a4af194f9bULL,0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL,0x12835b0145706fbeULL,0x243185be4ee4b28cULL,0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL,0x80deb1fe3b1696b1ULL,0x9bdc06a725c71235ULL,0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL,0xefbe4786384f25e3ULL,0x0fc19dc68b8cd5b5ULL,0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL,0x4a7484aa6ea6e483ULL,0x5cb0a9dcbd41fbd4ULL,0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL,0xa831c66d2db43210ULL,0xb00327c898fb213fULL,0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL,0xd5a79147930aa725ULL,0x06ca6351e003826fULL,0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL,0x2e1b21385c26c926ULL,0x4d2c6dfc5ac42aedULL,0x53380d139d95b3dfULL,
    0x650a73548baf63deULL,0x766a0abb3c77b2a8ULL,0x81c2c92e47edaee6ULL,0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL,0xa81a664bbc423001ULL,0xc24b8b70d0f89791ULL,0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL,0xd69906245565a910ULL,0xf40e35855771202aULL,0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL,0x1e376c085141ab53ULL,0x2748774cdf8eeb99ULL,0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL,0x4ed8aa4ae3418acbULL,0x5b9cca4f7763e373ULL,0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL,0x78a5636f43172f60ULL,0x84c87814a1f0ab72ULL,0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL,0xa4506cebde82bde9ULL,0xbef9a3f7b2c67915ULL,0xc67178f2e372532bULL,
    0xca273eceea26619cULL,0xd186b8c721c0c207ULL,0xeada7dd6cde0eb1eULL,0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL,0x0a637dc5a2c898a6ULL,0x113f9804bef90daeULL,0x1b710b35131c471bULL,
    0x28db77f523047d84ULL,0x32caab7b40c72493ULL,0x3c9ebe0a15c9bebcULL,0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL,0x597f299cfc657e2aULL,0x5fcb6fab3ad6faecULL,0x6c44198c4a475817ULL
};
inline uint64_t rotr64(uint64_t x, int n){ return (x >> n) | (x << (64 - n)); }

struct Sha512 {
    uint64_t h[8];
    uint64_t total;      // bytes
    uint8_t  buf[128];
    size_t   buflen;
};

void sha512_init(Sha512 *s){
    s->h[0]=0x6a09e667f3bcc908ULL; s->h[1]=0xbb67ae8584caa73bULL;
    s->h[2]=0x3c6ef372fe94f82bULL; s->h[3]=0xa54ff53a5f1d36f1ULL;
    s->h[4]=0x510e527fade682d1ULL; s->h[5]=0x9b05688c2b3e6c1fULL;
    s->h[6]=0x1f83d9abfb41bd6bULL; s->h[7]=0x5be0cd19137e2179ULL;
    s->total=0; s->buflen=0;
}

void sha512_block(uint64_t h[8], const uint8_t *p){
    uint64_t w[80];
    for (int i = 0; i < 16; i++){
        uint64_t v = 0;
        for (int j = 0; j < 8; j++) v = (v << 8) | p[i*8+j];
        w[i] = v;
    }
    for (int i = 16; i < 80; i++){
        uint64_t s0 = rotr64(w[i-15],1) ^ rotr64(w[i-15],8) ^ (w[i-15] >> 7);
        uint64_t s1 = rotr64(w[i-2],19) ^ rotr64(w[i-2],61) ^ (w[i-2] >> 6);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint64_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
    for (int i = 0; i < 80; i++){
        uint64_t S1 = rotr64(e,14) ^ rotr64(e,18) ^ rotr64(e,41);
        uint64_t ch = (e & f) ^ (~e & g);
        uint64_t t1 = hh + S1 + ch + SHA512_K[i] + w[i];
        uint64_t S0 = rotr64(a,28) ^ rotr64(a,34) ^ rotr64(a,39);
        uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint64_t t2 = S0 + maj;
        hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
}

void sha512_update(Sha512 *s, const void *data, size_t len){
    const uint8_t *p = (const uint8_t *)data;
    s->total += len;
    if (s->buflen){
        size_t need = 128 - s->buflen;
        if (len < need){ memcpy(s->buf + s->buflen, p, len); s->buflen += len; return; }
        memcpy(s->buf + s->buflen, p, need);
        sha512_block(s->h, s->buf);
        p += need; len -= need; s->buflen = 0;
    }
    while (len >= 128){ sha512_block(s->h, p); p += 128; len -= 128; }
    if (len){ memcpy(s->buf, p, len); s->buflen = len; }
}

void sha512_final(Sha512 *s, uint8_t out[64]){
    uint64_t bits = s->total * 8;
    size_t padlen = (s->buflen < 112) ? (112 - s->buflen) : (240 - s->buflen);
    uint8_t pad[256];
    memset(pad, 0, padlen + 16);
    pad[0] = 0x80;
    for (int i = 0; i < 8; i++) pad[padlen + 8 + i] = (uint8_t)(bits >> (56 - i*8));
    sha512_update(s, pad, padlen + 16);
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++)
            out[i*8+j] = (uint8_t)(s->h[i] >> (56 - j*8));
}

void sha512(const void *data, size_t len, uint8_t out[64]){
    Sha512 s; sha512_init(&s); sha512_update(&s, data, len); sha512_final(&s, out);
}

// ===========================================================================
//  HMAC-SHA512  (with precomputed key schedule for PBKDF2)
// ===========================================================================
struct HmacKey512 { uint8_t ipad[128]; uint8_t opad[128]; };

void hmac512_key(HmacKey512 *k, const void *key, size_t keylen){
    uint8_t kb[128];
    memset(kb, 0, sizeof(kb));
    if (keylen > 128){ sha512(key, keylen, kb); }
    else memcpy(kb, key, keylen);
    for (int i = 0; i < 128; i++){
        k->ipad[i] = (uint8_t)(kb[i] ^ 0x36);
        k->opad[i] = (uint8_t)(kb[i] ^ 0x5c);
    }
    memset(kb, 0, sizeof(kb));
}

void hmac512_pre(const HmacKey512 *k, const void *msg, size_t msglen, uint8_t out[64]){
    uint8_t inner[64];
    Sha512 s;
    sha512_init(&s);
    sha512_update(&s, k->ipad, 128);
    sha512_update(&s, msg, msglen);
    sha512_final(&s, inner);
    sha512_init(&s);
    sha512_update(&s, k->opad, 128);
    sha512_update(&s, inner, 64);
    sha512_final(&s, out);
}

void hmac512(const void *key, size_t keylen, const void *msg, size_t msglen, uint8_t out[64]){
    HmacKey512 k; hmac512_key(&k, key, keylen); hmac512_pre(&k, msg, msglen, out);
}

// PBKDF2-HMAC-SHA512, dkLen == 64 -> exactly one output block.
void pbkdf2_sha512_1block(const HmacKey512 *key, const uint8_t *salt, size_t saltlen,
                          uint32_t iters, uint8_t out[64]){
    uint8_t msg[512];
    memcpy(msg, salt, saltlen);
    msg[saltlen+0] = 0; msg[saltlen+1] = 0; msg[saltlen+2] = 0; msg[saltlen+3] = 1;
    uint8_t U[64];
    hmac512_pre(key, msg, saltlen + 4, U);
    memcpy(out, U, 64);
    for (uint32_t i = 1; i < iters; i++){
        hmac512_pre(key, U, 64, U);
        for (int j = 0; j < 64; j++) out[j] ^= U[j];
    }
}

// ===========================================================================
//  Ed25519 public key from a 32-byte seed (standard: SHA-512 based)
// ===========================================================================
void ed25519_pubkey(uint8_t pk[32], const uint8_t seed[32]){
    uint8_t h[64], a[32], b[32];
    sha512(seed, 32, h);
    memcpy(a, h, 32);
    crypto_eddsa_trim_scalar(b, a);   // clamp
    crypto_eddsa_scalarbase(pk, b);   // pk = a * B
    memset(h, 0, sizeof(h));
    memset(a, 0, sizeof(a));
    memset(b, 0, sizeof(b));
}

// ===========================================================================
//  BIP39
// ===========================================================================
// word count -> entropy bytes: 12 words -> 16, 24 words -> 32
static inline int bip39_ent_bytes(int words){ return words / 3 * 4; }

// entropy (128 or 256 bits) -> mnemonic string
void bip39_from_entropy(const uint8_t *ent, int entBytes, std::string &out){
    int entBits = entBytes * 8;
    int csBits  = entBits / 32;
    uint8_t hash[32];
    sha256(ent, entBytes, hash);
    int totalBits = entBits + csBits;
    int words = totalBits / 11;
    out.clear();
    for (int w = 0; w < words; w++){
        int idx = 0;
        for (int b = 0; b < 11; b++){
            int pos = w * 11 + b, bit;
            if (pos < entBits) bit = (ent[pos >> 3] >> (7 - (pos & 7))) & 1;
            else { int c = pos - entBits; bit = (hash[c >> 3] >> (7 - (c & 7))) & 1; }
            idx = (idx << 1) | bit;
        }
        if (w) out += ' ';
        out += BIP39_ENGLISH[idx];
    }
}

// mnemonic -> entropy.  The BIP39 English list is sorted, so a plain binary
// search is enough.  Returns false (and fills `err`) on an unknown word or a
// bad SHA-256 checksum.  Only 12/24-word phrases are accepted.
static int bip39_word_index(const std::string &w){
    int lo = 0, hi = 2047;
    while (lo <= hi){
        int mid = (lo + hi) >> 1;
        int c = std::strcmp(w.c_str(), BIP39_ENGLISH[mid]);
        if (c == 0) return mid;
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return -1;
}

static bool bip39_to_entropy(const std::string &mnemonic, uint8_t ent[32],
                             int &entBytes, std::string &err){
    std::vector<std::string> w;
    std::string cur;
    for (char c : mnemonic){
        if (std::isspace((unsigned char)c)){
            if (!cur.empty()){ w.push_back(cur); cur.clear(); }
        } else cur += (char)std::tolower((unsigned char)c);
    }
    if (!cur.empty()) w.push_back(cur);

    if (w.size() != 12 && w.size() != 24){
        err = "mnemonic must have 12 or 24 words (got " + std::to_string(w.size()) + ")";
        return false;
    }
    entBytes = bip39_ent_bytes((int)w.size());
    const int entBits = entBytes * 8, csBits = entBits / 32;

    std::vector<int> bits;
    bits.reserve(w.size() * 11);
    for (const std::string &word : w){
        int idx = bip39_word_index(word);
        if (idx < 0){ err = "unknown BIP39 word: " + word; return false; }
        for (int b = 10; b >= 0; b--) bits.push_back((idx >> b) & 1);
    }

    memset(ent, 0, 32);
    for (int i = 0; i < entBits; i++)
        if (bits[i]) ent[i >> 3] |= (uint8_t)(1u << (7 - (i & 7)));

    uint8_t hash[32];
    sha256(ent, entBytes, hash);
    for (int i = 0; i < csBits; i++){
        int got = bits[entBits + i];
        int want = (hash[i >> 3] >> (7 - (i & 7))) & 1;
        if (got != want){ err = "invalid mnemonic checksum"; return false; }
    }
    return true;
}

// ===========================================================================
//  Legacy deterministic fallback stream  (reproduction of the old defect)
// ---------------------------------------------------------------------------
//  Early releases, whenever the OS entropy source was unavailable, seeded the
//  generator from the fixed constant byte pattern below instead of failing.
//  Every mnemonic was therefore a predictable function of its index.  The
//  production Rng NEVER uses this code -- it lives here only so a user can test
//  whether a phrase they already hold came from such a build.
//
//      s[i]  = (i*131 + 7) & 0xFF          (i = 0..47)
//      key   = s[0 .. 32)
//      nonce = s[32 .. 40)
//      ctr   = big-endian uint64 of s[0 .. 8)
//      stream = ChaCha20 (djb) over zero bytes
//      phrase #n = BIP39 of the 16-byte entropy block at offset 16*n
//                  (4 phrases per 64-byte ChaCha20 block)
// ===========================================================================
static void insecure_fallback_entropy(uint64_t index, uint8_t out[16]){
    uint8_t s[48];
    for (size_t i = 0; i < sizeof(s); i++) s[i] = (uint8_t)(i * 131 + 7);

    uint8_t key[32], nonce[8];
    memcpy(key, s, 32);
    memcpy(nonce, s + 32, 8);
    uint64_t ctr0 = 0;
    for (int i = 0; i < 8; i++) ctr0 = (ctr0 << 8) | s[i];

    static const uint8_t zeros[64] = {0};
    uint8_t block[64];
    crypto_chacha20_djb(block, zeros, 64, key, nonce, ctr0 + index / 4);
    memcpy(out, block + (size_t)((index % 4) * 16), 16);

    memset(s, 0, sizeof(s));
    memset(block, 0, sizeof(block));
}

// Fallback check results.
enum { FB_ERROR = -1, FB_NOT_FOUND = 0, FB_FOUND = 1 };

// Was `mnemonic` emitted by the legacy fallback at some index below `limit`?
// Returns FB_FOUND (and writes *index), FB_NOT_FOUND, or FB_ERROR (bad phrase).
static int check_fallback_phrase(const std::string &mnemonic, uint64_t limit,
                                 uint64_t *index, std::string &err){
    uint8_t target[32];
    int entBytes = 0;
    if (!bip39_to_entropy(mnemonic, target, entBytes, err)) return FB_ERROR;
    if (entBytes != 16){
        err = "only 12-word (128-bit) phrases are supported by this check";
        return FB_ERROR;
    }

    for (uint64_t i = 0; i < limit; i++){
        uint8_t e[16];
        insecure_fallback_entropy(i, e);
        if (memcmp(e, target, 16) == 0){
            if (index) *index = i;
            memset(target, 0, sizeof(target));
            return FB_FOUND;
        }
    }
    memset(target, 0, sizeof(target));
    return FB_NOT_FOUND;
}

// ===========================================================================
//  Slip-0010 / app derivation (=== the app's "FU")
//      seed = PBKDF2-HMAC-SHA512(mnemonic, salt="mnemonic", 2048, 64)
//      key  = SLIP-0010 ed25519  m/44'/607'/0'
// ===========================================================================
void app_derive_secret(const std::string &mnemonic, uint8_t secret[32]){
    HmacKey512 hk;
    hmac512_key(&hk, mnemonic.data(), mnemonic.size());          // password = mnemonic
    uint8_t seed[64];
    pbkdf2_sha512_1block(&hk, (const uint8_t *)"mnemonic", 8, 2048, seed); // salt = "mnemonic"

    uint8_t I[64], k[32], c[32];
    hmac512("ed25519 seed", 12, seed, 64, I);
    memcpy(k, I, 32); memcpy(c, I + 32, 32);

    const uint32_t path[3] = { 44, 607, 0 };
    for (int i = 0; i < 3; i++){
        uint8_t data[37];
        data[0] = 0;
        memcpy(data + 1, k, 32);
        uint32_t idx = path[i] | 0x80000000u;
        data[33] = (uint8_t)(idx >> 24); data[34] = (uint8_t)(idx >> 16);
        data[35] = (uint8_t)(idx >> 8);  data[36] = (uint8_t)(idx);
        hmac512(c, 32, data, 37, I);
        memcpy(k, I, 32); memcpy(c, I + 32, 32);
    }
    memcpy(secret, k, 32);
    memset(seed, 0, sizeof(seed));
    memset(I, 0, sizeof(I));
}

// ===========================================================================
//  TON-native derivation (=== @ton/crypto "mnemonicToPrivateKey")
//      entropy = HMAC-SHA512(key = mnemonic, msg = "")
//      seed    = PBKDF2-HMAC-SHA512(password = entropy, salt = "TON default seed",
//                                   100000, 64)
//      key     = seed[0..31]
//  A mnemonic is "valid" for TON when
//      PBKDF2(entropy, "TON seed version", 390)[0] == 0
// ===========================================================================
void ton_entropy(const std::string &mnemonic, uint8_t entropy[64]){
    static const uint8_t empty = 0;
    hmac512(mnemonic.data(), mnemonic.size(), &empty, 0, entropy);
}

bool ton_is_basic_seed(const uint8_t entropy[64]){
    HmacKey512 hk; hmac512_key(&hk, entropy, 64);
    uint8_t out[64];
    pbkdf2_sha512_1block(&hk, (const uint8_t *)"TON seed version", 16, 390, out);
    return out[0] == 0;
}

void ton_derive_secret(const std::string &mnemonic, uint8_t secret[32]){
    uint8_t entropy[64];
    ton_entropy(mnemonic, entropy);
    HmacKey512 hk; hmac512_key(&hk, entropy, 64);
    uint8_t seed[64];
    pbkdf2_sha512_1block(&hk, (const uint8_t *)"TON default seed", 16, 100000, seed);
    memcpy(secret, seed, 32);
    memset(entropy, 0, sizeof(entropy));
    memset(seed, 0, sizeof(seed));
}

// ===========================================================================
//  TON account address for a WalletTg state
// ===========================================================================
static uint8_t g_codeHash[32];
static uint8_t g_dataDescPrefix[2] = { 0x00, 0x52 }; // refs=0, bits=(41+41)

void wallet_init_code_hash(){
    // WalletTrampoline.fif  (192 bits, 0 refs)
    static const uint8_t CODE[24] = {
        0xFF,0x00,0x20,0x98,0x21,0xD7,0x49,0x83,0x08,0xB9,0xF2,0x40,
        0xDF,0x80,0x85,0xF8,0x33,0xD0,0xED,0x1E,0x20,0xED,0x53,0xD9
    };
    uint8_t repr[2 + 24];
    repr[0] = 0x00;   // refs descriptor
    repr[1] = 0x30;   // bits descriptor = ceil(192/8)+floor(192/8) = 48
    memcpy(repr + 2, CODE, 24);
    sha256(repr, sizeof(repr), g_codeHash);
}

// build the 36-byte friendly address buffer
void wallet_address(const uint8_t pubkey[32], uint32_t subwalletId, bool testnet,
                    uint8_t out36[36]){
    uint8_t data[41];
    data[0] = 0x00;                       // storage tag / version
    data[1] = data[2] = data[3] = data[4] = 0x00; // seqno = 0 (deploy state)
    data[5] = (uint8_t)(subwalletId >> 24); data[6] = (uint8_t)(subwalletId >> 16);
    data[7] = (uint8_t)(subwalletId >> 8);  data[8] = (uint8_t)(subwalletId);
    memcpy(data + 9, pubkey, 32);

    // data cell hash
    uint8_t drep[2 + 41];
    drep[0] = g_dataDescPrefix[0]; drep[1] = g_dataDescPrefix[1];
    memcpy(drep + 2, data, 41);
    uint8_t dataHash[32];
    sha256(drep, sizeof(drep), dataHash);

    // state init cell hash:  [0x02,0x01,0x34, codeDepth(2), dataDepth(2), codeHash, dataHash]
    uint8_t si[2 + 1 + 4 + 64];
    si[0] = 0x02; si[1] = 0x01; si[2] = 0x34;
    si[3] = si[4] = si[5] = si[6] = 0x00;
    memcpy(si + 7,  g_codeHash, 32);
    memcpy(si + 39, dataHash, 32);
    uint8_t addrHash[32];
    sha256(si, sizeof(si), addrHash);

    uint8_t tag = testnet ? 0xD1 : 0x51;   // non-bounceable
    out36[0] = tag;
    out36[1] = 0x00;                       // workchain 0
    memcpy(out36 + 2, addrHash, 32);

    // CRC16 (matches @ton/core: message + two trailing zero bytes)
    uint32_t reg = 0;
    for (size_t i = 0; i < 36; i++){
        uint8_t byte = (i < 34) ? out36[i] : 0x00;
        for (int mask = 0x80; mask; mask >>= 1){
            reg <<= 1;
            if (byte & mask) reg |= 1;
            if (reg > 0xffff){ reg &= 0xffff; reg ^= 0x1021; }
        }
    }
    out36[34] = (uint8_t)(reg >> 8);
    out36[35] = (uint8_t)(reg & 0xff);
}

// ===========================================================================
//  base64url helpers + pattern matching (only the needed chars are computed)
// ===========================================================================
static const char B64URL[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

inline char addr_char(const uint8_t a[36], int idx){
    int g = idx >> 2, r = idx & 3;
    uint8_t b0 = a[3*g], b1 = a[3*g+1], b2 = a[3*g+2];
    unsigned v;
    switch (r){
        case 0: v = b0 >> 2; break;
        case 1: v = ((b0 & 3) << 4) | (b1 >> 4); break;
        case 2: v = ((b1 & 15) << 2) | (b2 >> 6); break;
        default:v = b2 & 63; break;
    }
    return B64URL[v];
}

inline bool addr_matches(const uint8_t a[36], const std::string &pat,
                         int start, bool ci){
    int n = (int)pat.size();
    for (int i = 0; i < n; i++){
        char c = addr_char(a, start + i);
        char t = pat[i];
        if (ci){ if (std::tolower((unsigned char)c) != std::tolower((unsigned char)t)) return false; }
        else   { if (c != t) return false; }
    }
    return true;
}

void addr_to_string(const uint8_t a[36], std::string &out){
    out.resize(48);
    for (int i = 0; i < 48; i++) out[i] = addr_char(a, i);
}

void to_hex(const uint8_t *p, size_t n, std::string &out){
    static const char *hx = "0123456789abcdef";
    out.resize(n * 2);
    for (size_t i = 0; i < n; i++){ out[i*2] = hx[p[i] >> 4]; out[i*2+1] = hx[p[i] & 15]; }
}

void raw_address_string(const uint8_t a[36], std::string &out){
    std::string h; to_hex(a + 2, 32, h);
    out = "0:" + h;
}

// ===========================================================================
//  Deterministic RNG (ChaCha20 keystream), seeded from the OS
// ===========================================================================
struct Rng {
    uint8_t key[32], nonce[8];
    uint64_t ctr;
    uint8_t buf[64];
    size_t pos;
    // Fails closed: returns false (leaving the generator unseeded) when the OS
    // entropy source is unavailable.  Deterministic data is never substituted.
    bool seed(){
        uint8_t s[48];
        if (!os_random(s, sizeof(s))) return false;
        memcpy(key, s, 32);
        memcpy(nonce, s + 32, 8);
        ctr = 0;
        for (int i = 0; i < 8; i++) ctr = (ctr << 8) | s[i];
        pos = 64;
        memset(s, 0, sizeof(s));
        return true;
    }
    void fill(uint8_t *out, size_t n){
        static const uint8_t zeros[64] = {0};
        while (n){
            if (pos == 64){
                ctr = crypto_chacha20_djb(buf, zeros, 64, key, nonce, ctr);
                pos = 0;
            }
            size_t take = 64 - pos; if (take > n) take = n;
            memcpy(out, buf + pos, take);
            pos += take; out += take; n -= take;
        }
    }
};

// ===========================================================================
//  Entropy policy
// ===========================================================================
//  If the OS entropy source is unavailable we must NOT fall back to a
//  deterministic generator (that would hand out predictable mnemonics).  On
//  the CLI we report the fault on stderr and exit with a non-zero status; the
//  embedded API instead propagates the error to the caller (see wtm_run).
static const char *const ENTROPY_ERROR = "OS entropy source unavailable";

static void fatal_entropy(){
    fprintf(stderr, "fatal: %s\n", ENTROPY_ERROR);
    exit(1);
}

// ===========================================================================
//  Options
// ===========================================================================
struct Options {
    std::string mode = "phrase";     // phrase | subwallet
    std::string scheme = "app";      // app (compat) | ton (native)
    std::string suffix, prefix;
    int  matchStart = -1;            // explicit start index (optional)
    bool atGiven = false;
    bool ci = false;
    int  words = 12;                 // 12 or 24
    int  threads = 0;
    bool testnet = false;
    uint32_t subwalletId = 0x7FFF7F11u;
    bool haveSubwallet = false;
    std::string mnemonic;            // fixed mnemonic (optional)
    uint64_t maxTries = 0;           // 0 = unlimited
    bool quiet = false;
    bool selftest = false;
    bool bench = false;
    bool gpu = false;                // use OpenCL if available (phrase mode)
    bool gpuInfo = false;
    bool gpuTest = false;
    std::string pattern;

    // legacy weak-phrase tools (see "Legacy deterministic fallback stream")
    bool        checkFallback = false;
    std::string checkPhrase;
    uint64_t    checkLimit = 100000;
    bool        demoFallback = false;
    uint64_t    demoIndex = 0;
};

struct Found {
    std::atomic<bool> ok{false};
    std::mutex mtx;
    std::string mnemonic;
    uint32_t subwalletId = 0;
    uint8_t pubkey[32];
    uint8_t addr[36];
    uint64_t attempts = 0;
};

static std::atomic<uint64_t> g_attempts{0};
static std::atomic<bool>     g_stop{false};

// ===========================================================================
//  Self-test
// ===========================================================================
static int hexcmp(const uint8_t *p, size_t n, const char *hex){
    std::string h; to_hex(p, n, h);
    return h == hex ? 0 : 1;
}
static bool selftest(){
    int fail = 0;
    uint8_t d[64];

    sha256("abc", 3, d);
    if (hexcmp(d,32,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")){
        printf("[FAIL] sha256(\"abc\")\n"); fail++;
    } else printf("[ok]   sha256(\"abc\")\n");

    sha512("abc", 3, d);
    if (hexcmp(d,64,"ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
                       "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f")){
        printf("[FAIL] sha512(\"abc\")\n"); fail++;
    } else printf("[ok]   sha512(\"abc\")\n");

    wallet_init_code_hash();
    if (hexcmp(g_codeHash,32,"9149ae51c1e4689710cebf7830297b16acfbadb363a920a537893e7ffeeca768")){
        printf("[FAIL] WalletTrampoline code hash\n"); fail++;
    } else printf("[ok]   WalletTrampoline code hash\n");

    hmac512("key", 3, "The quick brown fox jumps over the lazy dog", 43, d);
    if (hexcmp(d,64,"b42af09057bac1e2d41708e48a902e09b5ff7f12ab428a4fe86653c73dd248fb"
                       "82f948a549f7b791a5b41915ee4d1ec3935357e4e2317250d0372afa2ebeeb3a")){
        printf("[FAIL] hmac-sha512\n"); fail++;
    } else printf("[ok]   hmac-sha512\n");

    {
        HmacKey512 hk; hmac512_key(&hk, "password", 8);
        pbkdf2_sha512_1block(&hk, (const uint8_t *)"salt", 4, 1, d);
        if (hexcmp(d,64,"867f70cf1ade02cff3752599a3a53dc4af34c7a669815ae5d513554e1c8cf252"
                           "c02d470a285a0501bad999bfe943c08f050235d7d68b1da55e63f73b60a57fce")){
            printf("[FAIL] pbkdf2-hmac-sha512\n"); fail++;
        } else printf("[ok]   pbkdf2-hmac-sha512\n");
    }

    // synthetic pubkey -> address (cross-checked against @ton/core
    // contractAddress() with the same WalletTrampoline code cell)
    {
        const char *pubhex = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
        uint8_t pub[32];
        for (int i = 0; i < 32; i++){
            auto hv = [](char c){ return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; };
            pub[i] = (uint8_t)((hv(pubhex[i*2]) << 4) | hv(pubhex[i*2+1]));
        }
        uint8_t a[36]; wallet_address(pub, 0x7FFF7F11u, false, a);
        std::string s; addr_to_string(a, s);
        if (s != "UQAbzItZqG4Emy2gouoSMn5PXT--1pT6KuO_JlzeTOKAh_E7"){
            printf("[FAIL] address vector: %s\n", s.c_str()); fail++;
        } else printf("[ok]   address vector\n");
    }

    // known mnemonic -> pubkey + address (app derivation)
    {
        const std::string m =
            "abandon abandon abandon abandon abandon abandon "
            "abandon abandon abandon abandon abandon about";
        uint8_t sec[32]; app_derive_secret(m, sec);
        if (hexcmp(sec,32,"b477ef5ed17fb8a2b8faddd7a9835a227243a82c70b190c7af4896155aa7df9f")){
            printf("[FAIL] slip10 secret\n"); fail++;
        } else printf("[ok]   slip10 secret\n");
        uint8_t pub[32]; ed25519_pubkey(pub, sec);
        if (hexcmp(pub,32,"7952e94118f34607c75e23258dd9220d66ccac5a3ee074125c25068e8107bfbf")){
            printf("[FAIL] ed25519 pubkey\n"); fail++;
        } else printf("[ok]   ed25519 pubkey\n");
        uint8_t a[36]; wallet_address(pub, 0x7FFF7F11u, false, a);
        std::string s; addr_to_string(a, s);
        if (s != "UQD4d2pzr-W4jzh1wOd52HrsH7rRagzkSUPC3_jCgMxmKz_L"){
            printf("[FAIL] mnemonic address: %s\n", s.c_str()); fail++;
        } else printf("[ok]   mnemonic address\n");
    }

    // BIP39 generation vectors (entropy 0 -> canonical phrases)
    {
        uint8_t z[32]; memset(z, 0, sizeof(z));
        std::string m12, m24;
        bip39_from_entropy(z, 16, m12);
        bip39_from_entropy(z, 32, m24);
        const char *exp12 =
            "abandon abandon abandon abandon abandon abandon "
            "abandon abandon abandon abandon abandon about";
        const char *exp24 =
            "abandon abandon abandon abandon abandon abandon abandon abandon "
            "abandon abandon abandon abandon abandon abandon abandon abandon "
            "abandon abandon abandon abandon abandon abandon abandon art";
        if (m12 != exp12){ printf("[FAIL] bip39 12 words: %s\n", m12.c_str()); fail++; }
        else printf("[ok]   bip39 12 words\n");
        if (m24 != exp24){ printf("[FAIL] bip39 24 words: %s\n", m24.c_str()); fail++; }
        else printf("[ok]   bip39 24 words\n");
    }

    // TON-native derivation vector (from @ton/crypto mnemonicNew + mnemonicToPrivateKey)
    {
        const std::string m =
            "drum taste oyster attract soda salon luxury social fork frog crouch learn "
            "vessel rescue verify easily paddle insect appear slow noble adapt lyrics head";
        uint8_t entropy[64];
        ton_entropy(m, entropy);
        if (!ton_is_basic_seed(entropy)){ printf("[FAIL] ton isBasicSeed\n"); fail++; }
        else printf("[ok]   ton isBasicSeed\n");
        uint8_t sec[32]; ton_derive_secret(m, sec);
        if (hexcmp(sec,32,"b6e302f84bbb64c04d7b50470ef4264803462cd369fd9dc74349f4a02c0bb9af")){
            printf("[FAIL] ton secret\n"); fail++;
        } else printf("[ok]   ton secret\n");
        uint8_t pub[32]; ed25519_pubkey(pub, sec);
        if (hexcmp(pub,32,"752e39c11db80956eead808156347d120bbffb655b520ad2748da10c61cd242a")){
            printf("[FAIL] ton pubkey\n"); fail++;
        } else printf("[ok]   ton pubkey\n");
        uint8_t a[36]; wallet_address(pub, 0x7FFF7F11u, false, a);
        std::string s; addr_to_string(a, s);
        if (s != "UQDWukXCchiV2P81LG_vOCKhBFmLVY56nuvLUlMTWXrgQH4S"){
            printf("[FAIL] ton address: %s\n", s.c_str()); fail++;
        } else printf("[ok]   ton address\n");
    }

    // legacy deterministic fallback: phrase #0 and its address
    {
        uint8_t ent[16];
        insecure_fallback_entropy(0, ent);
        std::string m; bip39_from_entropy(ent, 16, m);
        const char *exp =
            "knife term question marble enroll supreme scene bus walnut slide review exclude";
        if (m != exp){ printf("[FAIL] fallback phrase #0: %s\n", m.c_str()); fail++; }
        else printf("[ok]   fallback phrase #0\n");

        uint8_t sec[32]; app_derive_secret(m, sec);
        uint8_t pub[32]; ed25519_pubkey(pub, sec);
        uint8_t a[36]; wallet_address(pub, 0x7FFF7F11u, false, a);
        std::string s; addr_to_string(a, s);
        if (s != "UQB2dHHyDGA9mVPSe8-QgFQsuOKF7zwEFAwsCQrw-Qx8dUy9"){
            printf("[FAIL] fallback phrase #0 address: %s\n", s.c_str()); fail++;
        } else printf("[ok]   fallback phrase #0 address\n");
    }

    // fallback check: hit / miss / invalid checksum
    {
        uint64_t idx = 0; std::string err;
        int rc = check_fallback_phrase(
            "mail gadget olympic destroy blood ahead hold enemy behave inmate turn erosion",
            7000, &idx, err);
        if (rc != FB_FOUND || idx != 6706){
            printf("[FAIL] fallback check hit: rc=%d idx=%llu %s\n",
                   rc, (unsigned long long)idx, err.c_str()); fail++;
        } else printf("[ok]   fallback check hit (#6706)\n");

        rc = check_fallback_phrase(
            "abandon abandon abandon abandon abandon abandon "
            "abandon abandon abandon abandon abandon about", 50, &idx, err);
        if (rc != FB_NOT_FOUND){ printf("[FAIL] fallback check miss: rc=%d\n", rc); fail++; }
        else printf("[ok]   fallback check miss\n");

        rc = check_fallback_phrase(
            "abandon abandon abandon abandon abandon abandon "
            "abandon abandon abandon abandon abandon abandon", 50, &idx, err);
        if (rc != FB_ERROR){ printf("[FAIL] fallback check checksum: rc=%d\n", rc); fail++; }
        else printf("[ok]   fallback check checksum error\n");
    }

    printf(fail ? "\n%d self-test(s) FAILED\n" : "\nall self-tests passed\n", fail);
    return fail == 0;
}

// ===========================================================================
//  Mining workers
// ===========================================================================
// Prepare mutable address bytes with the fixed fields for subwallet mode.
static void report_found(const Options &o, Found &f, const std::string &mnemonic,
                         uint32_t subwalletId, const uint8_t pub[32], const uint8_t addr[36]){
    std::lock_guard<std::mutex> lk(f.mtx);
    if (f.ok.load()) return;
    f.mnemonic = mnemonic;
    f.subwalletId = subwalletId;
    memcpy(f.pubkey, pub, 32);
    memcpy(f.addr, addr, 36);
    f.attempts = g_attempts.load();
    f.ok = true;
    g_stop = true;
}

static void worker_subwallet(const Options &o, Found &f, Rng &rng, int tid, int nthreads,
                             const uint8_t pub[32], std::string mnemonic){
    (void)rng; (void)tid;
    uint8_t addr[36];
    uint64_t total = (uint64_t)1 << 32;
    for (uint64_t s = (uint64_t)tid; s < total; s += (uint64_t)nthreads){
        if (g_stop.load()) return;
        if (o.maxTries && g_attempts.load(std::memory_order_relaxed) >= o.maxTries){ g_stop = true; return; }
        wallet_address(pub, (uint32_t)s, o.testnet, addr);
        if (addr_matches(addr, o.pattern, o.matchStart, o.ci)){
            report_found(o, f, mnemonic, (uint32_t)s, pub, addr);
            return;
        }
        g_attempts.fetch_add(1, std::memory_order_relaxed);
    }
}

static void worker_phrase(const Options &o, Found &f, Rng &rng, int tid, int nthreads){
    (void)tid; (void)nthreads;
    const bool ton = (o.scheme == "ton");
    int entBytes = bip39_ent_bytes(o.words);   // 12 words -> 16, 24 words -> 32
    while (!g_stop.load()){
        if (o.maxTries && g_attempts.load(std::memory_order_relaxed) >= o.maxTries){ g_stop = true; return; }
        std::string m;
        if (ton){
            // any random words from the list, retried until the TON validity
            // test passes (isBasicSeed, ~1/256)
            uint8_t entropy[64];
            for (;;){
                m.clear();
                uint8_t b[4];
                for (int i = 0; i < o.words; i++){
                    rng.fill(b, 4);
                    uint32_t v = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                                 ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
                    if (i) m += ' ';
                    m += BIP39_ENGLISH[v % 2048u];
                }
                ton_entropy(m, entropy);
                if (ton_is_basic_seed(entropy)) break;
            }
        } else {
            uint8_t ent[32];
            rng.fill(ent, entBytes);
            bip39_from_entropy(ent, entBytes, m);
        }
        uint8_t sec[32];
        if (ton) ton_derive_secret(m, sec); else app_derive_secret(m, sec);
        uint8_t pub[32]; ed25519_pubkey(pub, sec);
        uint8_t addr[36];
        wallet_address(pub, o.subwalletId, o.testnet, addr);
        memset(sec, 0, sizeof(sec));
        if (addr_matches(addr, o.pattern, o.matchStart, o.ci)){
            report_found(o, f, m, o.subwalletId, pub, addr);
            return;
        }
        g_attempts.fetch_add(1, std::memory_order_relaxed);
    }
}

// Result of the GPU phrase worker.  "Unavailable" means the GPU could not be
// used and the caller should fall back to the CPU worker; "EntropyError" means
// the OS entropy source failed and the whole search must abort.
enum class GpuRun { Ran, Unavailable, EntropyError };

// ---------------------------------------------------------------------------
//  GPU phrase worker.
//
//  The GPU derives the 32-byte `app` secret (PBKDF2 + SLIP-0010 -- the part
//  that dominates the CPU cost); the CPU pool then derives the ed25519 public
//  key and the TON address for every secret in the batch.
// ---------------------------------------------------------------------------
static GpuRun gpu_phrase_search(const Options &o, Found &f, int nthreads){
    gpu::AppDeriver der;
    std::string err;
    if (!der.init(&err)){ fprintf(stderr, "[gpu] %s\n", err.c_str()); return GpuRun::Unavailable; }

    if (!o.quiet){
        const gpu::Info &gi = der.info();
        printf("gpu        : %s (%s), %u CU, %s\n",
               gi.device.c_str(), gi.vendor.c_str(), gi.computeUnits, gi.version.c_str());
    }

    const size_t B = (size_t)1 << 16;
    const int entBytes = bip39_ent_bytes(o.words);
    const int nt = nthreads < 1 ? 1 : nthreads;

    Rng rng;
    if (!rng.seed()) return GpuRun::EntropyError;
    std::vector<std::string> mn; mn.reserve(B);
    std::vector<uint8_t> sec;

    while (!g_stop.load()){
        if (o.maxTries && g_attempts.load(std::memory_order_relaxed) >= o.maxTries){ g_stop = true; break; }

        mn.clear();
        for (size_t i = 0; i < B && !g_stop.load(); i++){
            uint8_t ent[32];
            rng.fill(ent, entBytes);
            std::string m;
            bip39_from_entropy(ent, entBytes, m);
            mn.push_back(std::move(m));
        }
        if (mn.empty()) break;

        if (!der.derive(mn, sec, &err)){
            fprintf(stderr, "[gpu] %s\n", err.c_str());
            return GpuRun::Unavailable;
        }

        const size_t n = mn.size();
        std::atomic<size_t> next{0};
        auto job = [&](){
            uint8_t pub[32], addr[36];
            for (;;){
                size_t i = next.fetch_add(1, std::memory_order_relaxed);
                if (i >= n || g_stop.load()) break;
                const uint8_t *s = &sec[i * 32];
                ed25519_pubkey(pub, s);
                wallet_address(pub, o.subwalletId, o.testnet, addr);
                if (addr_matches(addr, o.pattern, o.matchStart, o.ci)){
                    report_found(o, f, mn[i], o.subwalletId, pub, addr);
                    return;
                }
                g_attempts.fetch_add(1, std::memory_order_relaxed);
            }
        };
        if (nt == 1){
            job();
        } else {
            std::vector<std::thread> ts;
            for (int t = 0; t < nt; t++) ts.emplace_back(job);
            for (auto &t : ts) t.join();
        }
        if (f.ok.load()) g_stop = true;
    }
    return GpuRun::Ran;
}

// Compare GPU and CPU derivations on a handful of mnemonics.
static int gpu_selftest(){
    gpu::Info info = gpu::probe();
    if (!info.ok){ printf("OpenCL: not available (%s)\n", info.error.c_str()); return 0; }
    printf("OpenCL platform : %s\n", info.platform.c_str());
    printf("OpenCL device   : %s (%s)\n", info.device.c_str(), info.vendor.c_str());
    printf("driver/version  : %s / %s\n", info.driver.c_str(), info.version.c_str());
    printf("compute units   : %u   global memory: %.0f MB\n\n",
           info.computeUnits, info.globalMem / 1048576.0);

    gpu::AppDeriver der; std::string err;
    if (!der.init(&err)){ printf("[FAIL] gpu init: %s\n", err.c_str()); return 1; }

    Rng r;
    if (!r.seed() || !r.seed()) fatal_entropy();
    std::vector<std::string> mn;
    mn.push_back("abandon abandon abandon abandon abandon abandon "
                 "abandon abandon abandon abandon abandon about");
    for (int i = 0; i < 15; i++){
        uint8_t ent[16]; r.fill(ent, 16);
        std::string m; bip39_from_entropy(ent, 16, m);
        mn.push_back(std::move(m));
    }

    std::vector<uint8_t> sec;
    auto t0 = std::chrono::steady_clock::now();
    if (!der.derive(mn, sec, &err)){ printf("[FAIL] gpu derive: %s\n", err.c_str()); return 1; }
    auto t1 = std::chrono::steady_clock::now();

    int fail = 0;
    for (size_t i = 0; i < mn.size(); i++){
        uint8_t ref[32]; app_derive_secret(mn[i], ref);
        if (memcmp(ref, &sec[i * 32], 32) != 0){
            std::string a, b; to_hex(ref, 32, a); to_hex(&sec[i * 32], 32, b);
            printf("[FAIL] gpu != cpu on mnemonic #%zu\n  cpu=%s\n  gpu=%s\n", i, a.c_str(), b.c_str());
            fail++;
        }
    }
    double ms = std::chrono::duration<double>(t1 - t0).count() * 1000.0;
    if (!fail) printf("[ok]   GPU matches CPU on %zu mnemonics (%.1f ms)\n", mn.size(), ms);
    return fail ? 1 : 0;
}

static double expected_attempts(const std::string &pat, bool ci){
    double e = 1.0;
    for (char c : pat){
        if (ci && std::isalpha((unsigned char)c)) e *= 32.0;
        else e *= 64.0;
    }
    return e;
}

static void progress_loop(const Options &o, std::chrono::steady_clock::time_point t0){
    double exp_tries = expected_attempts(o.pattern, o.ci);
    uint64_t last = 0;
    auto lastT = std::chrono::steady_clock::now();
    while (!g_stop.load()){
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        if (g_stop.load()) break;
        uint64_t cur = g_attempts.load();
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - lastT).count();
        double rate = dt > 0 ? (double)(cur - last) / dt : 0;
        last = cur; lastT = now;
        double elapsed = std::chrono::duration<double>(now - t0).count();
        double p = 1.0 - exp(-(double)cur / exp_tries);
        double eta = rate > 0 ? (exp_tries - (double)cur) / rate : 0;
        if (eta < 0) eta = 0;
        char etabuf[32];
        if (rate <= 0) snprintf(etabuf, sizeof(etabuf), "??");
        else snprintf(etabuf, sizeof(etabuf), "%.0fs", eta);
        printf("\r  %12llu tries  %9.0f/s  %7.0fs  p~%5.1f%%  ETA %s      ",
               (unsigned long long)cur, rate, elapsed, p * 100.0, etabuf);
        fflush(stdout);
    }
    printf("\r%-90s\r", "");
}

// ===========================================================================
//  Main
// ===========================================================================
static void benchmark(){
    auto clk = []{ return std::chrono::steady_clock::now(); };
    uint8_t d[64], pk[32], seed[32] = {0};
    auto rate = [](int n, std::chrono::steady_clock::time_point a,
                   std::chrono::steady_clock::time_point b){
        return n / std::chrono::duration<double>(b - a).count();
    };

    int N = 400000; auto a = clk();
    for (int i = 0; i < N; i++){ d[0] = (uint8_t)i; sha256(d, 32, d); }
    auto b = clk();
    printf("sha256          : %12.0f/s\n", rate(N, a, b));

    N = 400000; a = clk();
    for (int i = 0; i < N; i++){ d[0] = (uint8_t)i; sha512(d, 32, d); }
    b = clk();
    printf("sha512          : %12.0f/s\n", rate(N, a, b));

    N = 20000; a = clk();
    for (int i = 0; i < N; i++){ seed[0] = (uint8_t)i; ed25519_pubkey(pk, seed); }
    b = clk();
    printf("ed25519 pubkey  : %12.0f/s\n", rate(N, a, b));

    N = 2000; a = clk();
    for (int i = 0; i < N; i++){
        std::string m; uint8_t ent[16] = {1,2,3,4,5,6,7,8,9,10,11,(uint8_t)i,13,14,15,16};
        bip39_from_entropy(ent, 16, m);
        uint8_t sec[32]; app_derive_secret(m, sec);
        ed25519_pubkey(pk, sec);
        uint8_t addr[36]; wallet_address(pk, 0x7FFF7F11u, false, addr);
    }
    b = clk();
    printf("phrase attempt  : %12.0f/s\n", rate(N, a, b));

    N = 2000000; a = clk();
    for (int i = 0; i < N; i++){ uint8_t addr[36]; wallet_address(pk, (uint32_t)i, false, addr); }
    b = clk();
    printf("subwallet try   : %12.0f/s\n", rate(N, a, b));

    // optional: OpenCL throughput for PBKDF2 + SLIP-0010
    {
        gpu::AppDeriver der; std::string err;
        if (der.init(&err)){
            std::vector<std::string> mn; mn.reserve(4096);
            Rng r;
            if (!r.seed() || !r.seed()) fatal_entropy();
            for (int i = 0; i < 4096; i++){
                uint8_t ent[16]; r.fill(ent, 16); std::string m;
                bip39_from_entropy(ent, 16, m); mn.push_back(std::move(m));
            }
            std::vector<uint8_t> sec;
            der.derive(mn, sec, &err);            // warm-up
            const int R = 8;
            auto g0 = clk();
            for (int i = 0; i < R; i++) der.derive(mn, sec, &err);
            auto g1 = clk();
            printf("gpu pbkdf2+slip : %12.0f/s   (%s)\n",
                   rate((int)mn.size() * R, g0, g1), der.info().device.c_str());
        } else {
            printf("gpu pbkdf2+slip :      n/a   (%s)\n", err.c_str());
        }
    }
}

// ===========================================================================
//  Search driver, shared by the CLI (run_main) and the embeddable API
//  (wtm_run).  prepare_fixed_key() builds the key material that stays
//  constant in subwallet mode; execute_search() runs the worker pool.
// ===========================================================================
static bool prepare_fixed_key(const Options &o, std::string &fixedMnemonic, uint8_t fixedPub[32]){
    fixedMnemonic = o.mnemonic;
    if (o.mode != "subwallet") return true;

    if (fixedMnemonic.empty()){
        Rng r;
        if (!r.seed()) return false;
        if (o.scheme == "ton"){
            uint8_t entropy[64];
            for (;;){
                fixedMnemonic.clear();
                uint8_t b[4];
                for (int i = 0; i < o.words; i++){
                    r.fill(b, 4);
                    uint32_t v = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
                                 ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
                    if (i) fixedMnemonic += ' ';
                    fixedMnemonic += BIP39_ENGLISH[v % 2048u];
                }
                ton_entropy(fixedMnemonic, entropy);
                if (ton_is_basic_seed(entropy)) break;
            }
        } else {
            int entBytes = bip39_ent_bytes(o.words);
            uint8_t ent[32]; r.fill(ent, entBytes);
            bip39_from_entropy(ent, entBytes, fixedMnemonic);
        }
    }
    uint8_t sec[32];
    if (o.scheme == "ton") ton_derive_secret(fixedMnemonic, sec);
    else                   app_derive_secret(fixedMnemonic, sec);
    ed25519_pubkey(fixedPub, sec);
    memset(sec, 0, sizeof(sec));
    return true;
}

static bool execute_search(const Options &o, int nthreads, const std::string &fixedMnemonic,
                           const uint8_t fixedPub[32], Found &found,
                           double &elapsed, bool &ranOnGpu){
    g_attempts = 0;
    g_stop = false;
    auto t0 = std::chrono::steady_clock::now();

    // ---- progress thread ---------------------------------------------------
    std::thread prog;
    if (!o.quiet) prog = std::thread(progress_loop, std::cref(o), t0);

    // Stop the progress thread and report failure on an unrecoverable error.
    auto bail = [&]() -> bool {
        g_stop = true;
        if (prog.joinable()) prog.join();
        return false;
    };

    // ---- GPU path (optional) ----------------------------------------------
    ranOnGpu = false;
    if (o.mode == "phrase" && o.gpu && o.scheme == "app" && o.words == 12 && fixedMnemonic.empty()){
        GpuRun g = gpu_phrase_search(o, found, nthreads);
        if (g == GpuRun::EntropyError) return bail();
        ranOnGpu = (g == GpuRun::Ran);
    }

    // ---- CPU workers -------------------------------------------------------
    // Seed every per-thread DRBG up front: fail closed if the OS has no entropy.
    if (!ranOnGpu){
        std::vector<Rng> rngs(nthreads);
        for (int t = 0; t < nthreads; t++){
            if (!rngs[t].seed()) return bail();
            if (!rngs[t].seed()) return bail(); // discard first keystream block
        }
        std::vector<std::thread> workers;
        for (int t = 0; t < nthreads; t++){
            if (o.mode == "subwallet")
                workers.emplace_back(worker_subwallet, std::cref(o), std::ref(found), std::ref(rngs[t]),
                                     t, nthreads, fixedPub, fixedMnemonic);
            else
                workers.emplace_back(worker_phrase, std::cref(o), std::ref(found), std::ref(rngs[t]),
                                     t, nthreads);
        }
        for (auto &w : workers) w.join();
    }
    g_stop = true;
    if (prog.joinable()) prog.join();
    elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return true;
}

// Reproduce the mnemonic + address the legacy fallback would emit at `index`.
// Diagnostics only: such a phrase is predictable and must never hold funds.
static void demo_insecure_fallback(uint64_t index, std::string &mnemonic, std::string &address){
    uint8_t ent[16];
    insecure_fallback_entropy(index, ent);
    bip39_from_entropy(ent, 16, mnemonic);

    uint8_t sec[32];  app_derive_secret(mnemonic, sec);
    uint8_t pub[32];  ed25519_pubkey(pub, sec);
    uint8_t addr[36]; wallet_address(pub, 0x7FFF7F11u, false, addr);
    addr_to_string(addr, address);

    memset(ent, 0, sizeof(ent));
    memset(sec, 0, sizeof(sec));
}

// ===========================================================================
//  Embeddable C API (see miner_api.h).  Implemented after the anonymous
//  namespace, so it can reach the internal types above.
// ===========================================================================
static bool importable_for(const Options &o, const Found &found){
    const uint32_t netDefault = o.testnet ? 0x7FFF7F7Du : 0x7FFF7F11u;
    return found.subwalletId == netDefault && o.scheme == "app";
}

} // namespace

extern "C" void wtm_defaults(wtm_params *p){
    if (!p) return;
    memset(p, 0, sizeof(*p));
    p->mode = "phrase";
    p->scheme = "app";
    p->matchAt = -1;
    p->words = 12;
    p->threads = 0;
    p->gpu = 0;
}

extern "C" uint64_t wtm_attempts(void){
    return g_attempts.load(std::memory_order_relaxed);
}

extern "C" void wtm_request_stop(void){
    g_stop = true;
}

extern "C" int wtm_selftest(void){
    wallet_init_code_hash();
    return selftest() ? 1 : 0;
}

extern "C" int wtm_check_fallback_phrase(const char *mnemonic, unsigned long long limit,
                                         wtm_fallback_result *out){
    if (out) memset(out, 0, sizeof(*out));
    if (!out) return FB_ERROR;
    out->status = FB_ERROR;
    if (!mnemonic){
        snprintf(out->error, sizeof(out->error), "no mnemonic");
        return FB_ERROR;
    }
    if (limit == 0) limit = 1;

    std::string phrase = mnemonic;
    uint64_t idx = 0; std::string err;
    int rc = check_fallback_phrase(phrase, (uint64_t)limit, &idx, err);
    if (!phrase.empty()) memset(&phrase[0], 0, phrase.size());  // do not keep the secret

    out->status = rc;
    if (rc == FB_FOUND) out->index = (unsigned long long)idx;
    if (rc == FB_ERROR) snprintf(out->error, sizeof(out->error), "%s", err.c_str());
    return rc;
}

extern "C" int wtm_insecure_demo_fallback(unsigned long long index, wtm_fallback_result *out){
    if (out) memset(out, 0, sizeof(*out));
    if (!out) return FB_ERROR;
    wallet_init_code_hash();

    std::string m, a;
    demo_insecure_fallback((uint64_t)index, m, a);
    snprintf(out->mnemonic, sizeof(out->mnemonic), "%s", m.c_str());
    snprintf(out->address,  sizeof(out->address),  "%s", a.c_str());
    out->index  = index;
    out->status = FB_FOUND;
    return FB_FOUND;
}

extern "C" double wtm_expected_attempts(const char *pattern, int ci){
    return expected_attempts(pattern ? std::string(pattern) : std::string(), ci != 0);
}

extern "C" int wtm_run(const wtm_params *p, wtm_result *out){
    if (out) memset(out, 0, sizeof(*out));
    if (!p || !out) return -1;

    auto fail = [&](const char *m){
        snprintf(out->error, sizeof(out->error), "%s", m);
        out->found = -1;
        return -1;
    };

    Options o;
    o.mode   = p->mode   ? p->mode   : "phrase";
    o.scheme = p->scheme ? p->scheme : "app";
    o.suffix = p->suffix ? p->suffix : "";
    o.prefix = p->prefix ? p->prefix : "";
    o.ci     = p->ci != 0;
    o.testnet= p->testnet != 0;
    o.words  = p->words > 0 ? p->words : 12;
    o.threads= p->threads;
    o.gpu    = p->gpu != 0;
    if (p->haveSubwallet){ o.subwalletId = p->subwalletId; o.haveSubwallet = true; }
    if (p->mnemonic && *p->mnemonic) o.mnemonic = p->mnemonic;
    o.quiet = true;                     // embedded: never touch the console

    if (o.mode != "phrase" && o.mode != "subwallet") return fail("mode must be phrase or subwallet");
    if (o.scheme != "app" && o.scheme != "ton")      return fail("scheme must be app or ton");
    if (o.words != 12 && o.words != 24)              return fail("words must be 12 or 24");
    if (o.suffix.empty() && o.prefix.empty())        return fail("specify a suffix or a prefix");

    if (!o.suffix.empty()){
        o.pattern = o.suffix;
        o.matchStart = (p->matchAt >= 0) ? p->matchAt : (48 - (int)o.suffix.size());
    } else {
        o.pattern = o.prefix;
        o.matchStart = (p->matchAt >= 0) ? p->matchAt : 2;
    }
    if (o.matchStart < 0) o.matchStart = 0;
    if (o.matchStart + (int)o.pattern.size() > 48)
        return fail("pattern does not fit in a 48-character address");
    for (char c : o.pattern){
        if (!(std::isalnum((unsigned char)c) || c == '-' || c == '_'))
            return fail("pattern must be base64url (A-Za-z0-9-_)");
    }
    if (o.testnet && !o.haveSubwallet) o.subwalletId = 0x7FFF7F7Du;

    wallet_init_code_hash();

    int nthreads = o.threads > 0 ? o.threads : (int)std::thread::hardware_concurrency();
    if (nthreads <= 0) nthreads = 1;

    std::string fixedMnemonic;
    uint8_t fixedPub[32];
    if (!prepare_fixed_key(o, fixedMnemonic, fixedPub)) return fail(ENTROPY_ERROR);

    Found found;
    double elapsed = 0.0;
    bool ranOnGpu = false;
    if (!execute_search(o, nthreads, fixedMnemonic, fixedPub, found, elapsed, ranOnGpu))
        return fail(ENTROPY_ERROR);

    out->elapsed = elapsed;
    out->usesGpu = ranOnGpu ? 1 : 0;

    if (!found.ok.load()){
        out->found = 0;
        out->attempts = g_attempts.load();
        return 0;
    }

    std::string addr, raw, pubh;
    addr_to_string(found.addr, addr);
    raw_address_string(found.addr, raw);
    to_hex(found.pubkey, 32, pubh);

    out->found = 1;
    snprintf(out->address, sizeof(out->address), "%s", addr.c_str());
    snprintf(out->raw, sizeof(out->raw), "%s", raw.c_str());
    snprintf(out->pubkey, sizeof(out->pubkey), "%s", pubh.c_str());
    snprintf(out->mnemonic, sizeof(out->mnemonic), "%s",
             (o.mode == "phrase" || !fixedMnemonic.empty()) ? found.mnemonic.c_str() : "");
    out->subwalletId = found.subwalletId;
    out->attempts = found.attempts;
    out->importable = importable_for(o, found) ? 1 : 0;
    return 1;
}

// Read one line from stdin (for `--check-fallback-phrase -`).  Returns false on
// EOF / empty input so a bare pipe does not silently pass an empty phrase.
static bool read_secret_stdin(std::string &out){
    char buf[2048];
    if (!fgets(buf, sizeof(buf), stdin)) return false;
    std::string s(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                          s.back() == ' '  || s.back() == '\t')) s.pop_back();
    size_t i = 0; while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    out = s.substr(i);
    return !out.empty();
}

// May we emit ANSI colour?  On Windows try to switch the console into
// virtual-terminal mode; elsewhere a plain tty check is enough.
static bool ansi_ok(){
#if defined(_WIN32)
    void *h = GetStdHandle((unsigned long)-11);         // STD_OUTPUT_HANDLE
    if (!h || h == (void *)-1) return false;
    unsigned long m = 0;
    if (!GetConsoleMode(h, &m)) return false;
    if (m & 0x0004u) return true;                       // ENABLE_VIRTUAL_TERMINAL_PROCESSING
    return SetConsoleMode(h, m | 0x0004u) != 0;
#else
    return isatty(1) != 0;
#endif
}

// --check-fallback-phrase "<12 words>" [--limit N]
//   0 = not found, 2 = found, 1 = error.
static int cmd_check_fallback(const Options &o){
    std::string phrase = o.checkPhrase;
    if (phrase == "-"){
        if (!read_secret_stdin(phrase)){
            fprintf(stderr, "%s\n", L3("ошибка: нет мнемоники в stdin",
                                       "error: no mnemonic on stdin",
                                       "错误：stdin 中没有助记词"));
            return 1;
        }
    }

    uint64_t index = 0; std::string err;
    int rc = check_fallback_phrase(phrase, o.checkLimit, &index, err);
    if (!phrase.empty()) memset(&phrase[0], 0, phrase.size());   // do not keep the secret

    if (rc == FB_ERROR){
        fprintf(stderr, "%s: %s\n", L3("ошибка", "error", "错误"), err.c_str());
        return 1;
    }

    printf("%s", L3("wallet_tg_miner - проверка фразы по потоку старой версии",
                    "wallet_tg_miner - checking phrase against the old version stream",
                    "wallet_tg_miner - 检查短语是否来自旧版数据流"));
    printf(" (<= %llu %s)\n\n", (unsigned long long)o.checkLimit,
           L3("фраз", "phrases", "短语"));

    if (rc == FB_FOUND){
        printf(L3("!!! СОВПАДЕНИЕ: это фраза #%llu потока старого дефекта.\n",
                  "!!! MATCH: this is phrase #%llu of the old defect stream.\n",
                  "!!! 匹配：这是旧版缺陷数据流的第 %llu 条短语。\n"),
               (unsigned long long)index);
        printf("%s\n", L3("!!! Фраза предсказуема — переведите средства на новый кошелёк",
                           "!!! The phrase is predictable - move funds to a new wallet",
                           "!!! 该短语可被预测 — 请将资金转移到新钱包"));
        printf("%s\n", L3("!!! и больше не пользуйтесь этой фразой.",
                           "!!! and never use this phrase again.",
                           "!!! 并不要再使用该短语。"));
        return 2;
    }

    printf(L3("совпадений не найдено в первых %llu фразах потока.\n",
              "no matches found in the first %llu phrases of the stream.\n",
              "在数据流前 %llu 条短语中未找到匹配。\n"),
           (unsigned long long)o.checkLimit);
    printf("\n%s\n", L3("ВАЖНО: отсутствие совпадения — НЕ гарантия.",
                        "IMPORTANT: no match is NOT a guarantee.",
                        "重要：未找到匹配并不等于安全。"));
    printf(L3("  * поток длиннее %llu фраз, поэтому совпадение может быть дальше;\n",
              "  * the stream is longer than %llu phrases, so a match may appear later;\n",
              "  * 数据流长度超过 %llu 条短语，匹配可能出现在更后面；\n"),
           (unsigned long long)o.checkLimit);
    printf("  %s\n", L3("* проверка покрывает только 12-словную схему app",
                          "* the check covers only the 12-word app scheme",
                          "* 检查仅覆盖 12 词 app 方案"));
    printf("    %s\n", L3("(24-словная схема rotation/anchor+signing не проверяется);",
                            "(the 24-word rotation/anchor+signing scheme is not checked);",
                            "（24 词 rotation/anchor+signing 方案未被检查）；"));
    printf("  %s\n", L3("* в режиме subwallet опасна только фраза #0 (ключ берётся один",
                          "* in subwallet mode only phrase #0 is dangerous (the key is",
                          "* 在 subwallet 模式下只有短语 #0 危险（密钥只取一次，"));
    printf("    %s\n", L3("    раз, его первые 16 байт = фраза #0).",
                            "    taken once, its first 16 bytes = phrase #0).",
                            "    其前 16 字节 = 短语 #0）。"));
    return 0;
}

// --insecure-demo-fallback [--index K]  (hidden; only shown by --help-dev)
static int cmd_demo_fallback(const Options &o){
    std::string m, a;
    demo_insecure_fallback(o.demoIndex, m, a);

    const char *red = "\033[1;31m", *rst = "\033[0m";
    const bool colour = ansi_ok();

    if (colour) printf("%s", red);
    printf("\n");
    printf("  ################################################################\n");
    printf("  #                                                              #\n");
    printf("  #   %s\n", L3("ТЕСТ: фраза предсказуема, средства НЕ отправлять",
                            "TEST: phrase is predictable, DO NOT send funds",
                            "测试：短语可预测，切勿转款"));
    printf("  #                                                              #\n");
    printf("  ################################################################\n");
    if (colour) printf("%s", rst);

    printf("\n%s #%llu\n", L3("поток старой версии (детерминированный fallback), фраза",
                              "old version stream (deterministic fallback), phrase",
                              "旧版数据流（确定性回退），短语"),
           (unsigned long long)o.demoIndex);
    printf("  mnemonic : %s\n", m.c_str());
    printf("  address  : %s\n", a.c_str());
    printf("\n%s\n", L3("Этот режим только воспроизводит старую ошибку и существует для",
                      "This mode only reproduces the old bug and exists for",
                      "此模式仅复现旧版错误，仅用于"));
    printf("%s\n", L3("диагностики. Такие фразы НИКОГДА не должны хранить реальные средства.",
                     "diagnostics. Such phrases must NEVER hold real funds.",
                     "诊断。此类短语永远不应存放真实资金。"));
    if (!m.empty()) memset(&m[0], 0, m.size());
    return 0;
}

static void usage(const char *prog, bool dev = false){
    printf(
"wallet_tg_miner - offline beauty address miner for Telegram Wallet (WalletTg)\n\n"
"Usage: %s (--suffix STR | --prefix STR) [options]\n\n"
"Match:\n"
"  --suffix STR       require the address to end with STR (default mode)\n"
"  --prefix STR       require STR right after the \"UQ\"/\"EQ\" tag (index 2)\n"
"  --at N             match STR at base64 index N instead\n"
"  --ci               case-insensitive matching\n"
"\n"
"Search mode:\n"
"  --mode phrase      vary the mnemonic (default). Slower; the address follows the\n"
"                     words, so importing the phrase into Telegram gives the SAME address.\n"
"  --mode subwallet   vary subwalletId only (fast, ~2^32 tries per key). The address\n"
"                     is NOT reproducible by Telegram without the same subwalletId.\n"
"\n"
"Derivation scheme:\n"
"  --scheme app       app scheme (default): PBKDF2(salt=\"mnemonic\") + SLIP-0010\n"
"                      m/44'/607'/0' + ed25519. Matches Telegram Wallet on import.\n"
"  --scheme ton       TON-native scheme (@ton/crypto mnemonicToPrivateKey):\n"
"                      24-word phrase importable into standard TON wallets\n"
"\n"
"Other:\n"
"  --words 12|24      mnemonic length for phrase mode (default 12)\n"
"  --threads N        worker threads (default: CPU count)\n"
"  --mnemonic \"...\"   use a fixed mnemonic (subwallet mode)\n"
"  --subwallet HEX    fixed subwallet id (default 0x7FFF7F11 mainnet)\n"
"  --testnet          testnet (subwallet 0x7FFF7F7D, tag 0xD1)\n"
"  --max-tries N      stop after N attempts\n"
"  --quiet            less output\n"
"  --bench            measure hash/derivation throughput and exit\n"
"  --selftest         run built-in self-tests and exit\n"
"\n"
"GPU (OpenCL, optional; no SDK required):\n"
"  --gpu              run PBKDF2/SLIP-0010 on the GPU (phrase mode, scheme app,\n"
"                     12 words). The CPU still derives ed25519 + the address.\n"
"                     Falls back to the CPU automatically when OpenCL is absent.\n"
"  --gpu-info         print the detected OpenCL device and exit\n"
"  --gpu-test         check that GPU and CPU derivations agree, then exit\n"
"  --wizard           interactive prompt: search / old-phrase check / demo\n"
"                     (used automatically on double-click)\n"
"  --lang ru|en|cn   UI language (default ru; or env WTM_LANG)\n"
"  --help             this text\n"
"  --help-dev         developer help (extra, insecure diagnostic modes)\n"
"\n"
"Old-version phrase check:\n"
"  --check-fallback-phrase \"<12 words>\" [--limit N]\n"
"                     check whether a mnemonic was produced by the old\n"
"                     deterministic fallback (default limit 100000; \"-\" reads\n"
"                     the phrase from stdin so it stays out of the shell history).\n"
"                     exit codes: 0 = not found, 2 = found, 1 = error\n",
        prog);

    if (dev) printf(
"\n"
"Developer (insecure diagnostics; never use with real funds):\n"
"  --insecure-demo-fallback [--index K]\n"
"                     print the phrase #K (default 0) that the OLD buggy\n"
"                     fallback would produce, together with its address.\n"
"  --index K          index for --insecure-demo-fallback (default 0)\n"
"\n");
}

static int run_main(int argc, char **argv){
    Options o;
    std::vector<std::string> args(argv + 1, argv + argc);

    auto need = [&](size_t &i) -> std::string {
        if (i + 1 >= args.size()){ fprintf(stderr, "missing value for %s\n", args[i].c_str()); exit(2); }
        return args[++i];
    };

    for (size_t i = 0; i < args.size(); i++){
        const std::string &a = args[i];
        if      (a == "--help" || a == "-h"){ usage(argv[0]); return 0; }
        else if (a == "--selftest")          o.selftest = true;
        else if (a == "--bench")             o.bench = true;
        else if (a == "--suffix")            o.suffix = need(i);
        else if (a == "--prefix")            o.prefix = need(i);
        else if (a == "--at")                { o.matchStart = atoi(need(i).c_str()); o.atGiven = true; }
        else if (a == "--ci")                o.ci = true;
        else if (a == "--mode")              o.mode = need(i);
        else if (a == "--scheme")            o.scheme = need(i);
        else if (a == "--words")             o.words = atoi(need(i).c_str());
        else if (a == "--threads" || a == "--jobs") o.threads = atoi(need(i).c_str());
        else if (a == "--mnemonic")          o.mnemonic = need(i);
        else if (a == "--subwallet")         { o.subwalletId = (uint32_t)strtoul(need(i).c_str(), nullptr, 0); o.haveSubwallet = true; }
        else if (a == "--testnet")           o.testnet = true;
        else if (a == "--max-tries")         o.maxTries = strtoull(need(i).c_str(), nullptr, 0);
        else if (a == "--quiet")             o.quiet = true;
        else if (a == "--gpu")               o.gpu = true;
        else if (a == "--no-gpu")            o.gpu = false;
        else if (a == "--gpu-info")          o.gpuInfo = true;
        else if (a == "--gpu-test")          o.gpuTest = true;
        else if (a == "--help-dev")          { usage(argv[0], true); return 0; }
        else if (a == "--check-fallback-phrase")  { o.checkFallback = true; o.checkPhrase = need(i); }
        else if (a == "--insecure-demo-fallback") { o.demoFallback = true; }
        else if (a == "--limit")             o.checkLimit = strtoull(need(i).c_str(), nullptr, 0);
        else if (a == "--index")             o.demoIndex  = strtoull(need(i).c_str(), nullptr, 0);
        else if (a == "--lang")              g_lang = parse_lang(need(i));
        else { fprintf(stderr, "unknown option: %s\n", a.c_str()); usage(argv[0]); return 2; }
    }

    wallet_init_code_hash();
    if (o.selftest) return selftest() ? 0 : 1;
    if (o.gpuTest)  return gpu_selftest();
    if (o.gpuInfo){
        gpu::Info gi = gpu::probe();
        if (!gi.ok){ printf("OpenCL: not available (%s)\n", gi.error.c_str()); return 1; }
        printf("platform       : %s\n", gi.platform.c_str());
        printf("device         : %s\n", gi.device.c_str());
        printf("vendor         : %s\n", gi.vendor.c_str());
        printf("driver         : %s\n", gi.driver.c_str());
        printf("device version : %s\n", gi.version.c_str());
        printf("compute units  : %u\n", gi.computeUnits);
        printf("global memory  : %.0f MB\n", gi.globalMem / 1048576.0);
        printf("max work-group : %zu\n", gi.maxWorkGroup);
        return 0;
    }
    if (o.bench){ benchmark(); return 0; }

    // legacy weak-phrase tools (independent of the search options above)
    if (o.checkFallback) return cmd_check_fallback(o);
    if (o.demoFallback)  return cmd_demo_fallback(o);

    if (o.suffix.empty() && o.prefix.empty()){
        fprintf(stderr, "error: specify --suffix STR or --prefix STR\n\n");
        usage(argv[0]); return 2;
    }
    if (o.mode != "phrase" && o.mode != "subwallet"){
        fprintf(stderr, "error: --mode must be 'phrase' or 'subwallet'\n"); return 2;
    }
    if (o.scheme != "app" && o.scheme != "ton"){
        fprintf(stderr, "error: --scheme must be 'app' or 'ton' (native)\n"); return 2;
    }
    if (o.words != 12 && o.words != 24){
        fprintf(stderr, "error: --words must be 12 or 24\n"); return 2;
    }

    // resolve the matched fragment
    if (!o.suffix.empty()){ o.pattern = o.suffix; if (!o.atGiven) o.matchStart = 48 - (int)o.suffix.size(); }
    else                  { o.pattern = o.prefix; if (!o.atGiven) o.matchStart = 2; }
    if (o.matchStart < 0) o.matchStart = 0;
    if (o.matchStart + (int)o.pattern.size() > 48){
        fprintf(stderr, "error: pattern does not fit in a 48-char address\n"); return 2;
    }
    for (char c : o.pattern){
        if (!(std::isalnum((unsigned char)c) || c == '-' || c == '_')){
            fprintf(stderr, "error: '%c' is not a base64url character (A-Za-z0-9-_)\n", c);
            return 2;
        }
    }

    if (o.testnet && !o.haveSubwallet) o.subwalletId = 0x7FFF7F7Du;

    int nthreads = o.threads > 0 ? o.threads : (int)std::thread::hardware_concurrency();
    if (nthreads <= 0) nthreads = 1;

    printf("wallet_tg_miner  |  mode=%s  scheme=%s  match=%s  start=%d  ci=%d  threads=%d%s\n",
           o.mode.c_str(), o.scheme.c_str(), o.pattern.c_str(), o.matchStart, (int)o.ci, nthreads,
           o.testnet ? "  [testnet]" : "");
    {
        double e = expected_attempts(o.pattern, o.ci);
        printf("expected ~%.3g attempts for this pattern\n\n", e);
    }

    Found found;

    // ---- prepare the fixed key material -----------------------------------
    uint8_t fixedPub[32];
    std::string fixedMnemonic;
    if (!prepare_fixed_key(o, fixedMnemonic, fixedPub)) fatal_entropy();

    if (o.mode == "subwallet"){
        printf("fixed mnemonic : %s\n", fixedMnemonic.c_str());
        { std::string h; to_hex(fixedPub, 32, h); printf("fixed pubkey   : %s\n", h.c_str()); }
        printf("searching subwalletId in [0, 2^32) ...\n\n");
    } else {
        printf("searching %s mnemonics (subwalletId = 0x%08X) ...\n\n",
               o.scheme == "ton" ? "TON" : "BIP39/app", o.subwalletId);
    }

    double elapsed = 0.0;
    bool ranOnGpu = false;
    if (!execute_search(o, nthreads, fixedMnemonic, fixedPub, found, elapsed, ranOnGpu)) fatal_entropy();

    if (!found.ok.load()){
        printf("no match found after %llu tries (%.1fs)\n",
               (unsigned long long)g_attempts.load(), elapsed);
        return 1;
    }

    std::string addr, raw;
    addr_to_string(found.addr, addr);
    raw_address_string(found.addr, raw);
    std::string pubh; to_hex(found.pubkey, 32, pubh);

    printf("\n=== FOUND ===\n");
    printf("  address     : %s\n", addr.c_str());
    printf("  raw         : %s\n", raw.c_str());
    printf("  subwalletId : 0x%08X (%u)\n", found.subwalletId, found.subwalletId);
    printf("  public key  : %s\n", pubh.c_str());
    if (o.mode == "phrase" || !fixedMnemonic.empty())
        printf("  mnemonic    : %s\n", found.mnemonic.c_str());
    printf("  attempts    : %llu\n", (unsigned long long)found.attempts);
    printf("  elapsed     : %.1fs\n", elapsed);
    printf("\n");
    // ---- importability check ------------------------------------------------
    const uint32_t netDefault = o.testnet ? 0x7FFF7F7Du : 0x7FFF7F11u;
    const bool standardSubwallet = (found.subwalletId == netDefault);

    printf("--- IMPORT CHECK (will the wallet show this address?) ---\n");
    if (o.testnet)
        printf("  network  : TESTNET (only a testnet wallet would match)\n");

    if (standardSubwallet && o.scheme == "app"){
        printf("  RESULT   : YES - import this phrase into Telegram Wallet (@wallet) and it\n");
        printf("             will show THIS address:\n");
        printf("               * standard subwalletId 0x%08X\n", found.subwalletId);
        printf("               * app derivation (PBKDF2 + SLIP-0010 m/44'/607'/0')\n");
    } else if (standardSubwallet){
        printf("  RESULT   : YES for TON-native wallets (Tonkeeper etc.) - standard\n");
        printf("             subwalletId 0x%08X + @ton/crypto mnemonicToPrivateKey.\n", found.subwalletId);
        printf("             In Telegram Wallet: check the address BEFORE funding.\n");
    } else {
        printf("  RESULT   : NO  - subwalletId 0x%08X is CUSTOM, so a standard client\n", found.subwalletId);
        printf("             (Telegram Wallet) would show a DIFFERENT address for this phrase.\n");
        printf("             Tip: re-run in phrase mode (--mode phrase) - there the words\n");
        printf("                  control the address and Telegram reproduces it exactly.\n");
    }
    printf("  Reminder : verify on-chain (address -> public key) before funding.\n");
    return 0;
}

// ===========================================================================
//  Entry point: when the .exe is started by double-click (no arguments),
//  run a tiny interactive wizard so the user does not need the command line.
//
//  Omitted in library builds (-DWTM_LIB, e.g. the Android JNI library), which
//  have no console.
// ===========================================================================
#ifndef WTM_LIB
static std::string read_line(const char *prompt){
    printf("%s", prompt); fflush(stdout);
    char buf[512];
    if (!fgets(buf, sizeof(buf), stdin)) return std::string();
    std::string s(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                          s.back() == ' '  || s.back() == '\t')) s.pop_back();
    size_t i = 0; while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    return s.substr(i);
}

// Same feature set as the Android app: search, old-version phrase check and
// the developer demo.  The check/demo sessions are the same code paths as the
// --check-fallback-phrase / --insecure-demo-fallback command line options.
static int interactive_wizard(){
    // 1) language first
    printf("\nLanguage / Язык / 语言:\n");
    printf("  1) Русский\n");
    printf("  2) English\n");
    printf("  3) 中文\n");
    std::string lc = read_line("Choice / Выбор / 选择 [1]: ");
    if (lc == "2") g_lang = LANG_EN;
    else if (lc == "3") g_lang = LANG_CN;
    else g_lang = LANG_RU;

    printf("\n=== %s ===\n\n", L3("wallet_tg_miner - красивый адрес для WalletTg",
                                    "wallet_tg_miner - beauty address for WalletTg",
                                    "wallet_tg_miner - WalletTg 美观地址"));
    printf("%s  UQ................................zz\n\n",
           L3("Адреса выглядят так:", "Addresses look like:", "地址示例："));

    printf("%s\n", L3("Что сделать:", "What to do:", "要做什么："));
    printf("  1) %s\n", L3("найти красивый адрес (по умолчанию)",
                            "find a beauty address (default)",
                            "查找美观地址（默认）"));
    printf("  2) %s\n", L3("проверить фразу старой версии",
                            "check a phrase from the old version",
                            "检查旧版短语"));
    printf("     %s\n", L3("(не сгенерирована ли она старым небезопасным способом)",
                            "(was it generated by the old insecure method?)",
                            "（是否由旧版不安全方式生成？）"));
    printf("  3) %s\n", L3("[режим разработчика] показать демо-фразу старого дефекта",
                            "[developer mode] show a demo phrase of the old defect",
                            "【开发者模式】显示旧版缺陷的演示短语"));
    std::string what = read_line(L3("Выбор [1]: ", "Choice [1]: ", "选择 [1]: "));

    std::vector<std::string> a = { "wallet_tg_miner" };

    if (what == "2"){
        printf("\n%s\n", L3("Введите 12 слов мнемоники через пробел.",
                            "Enter 12 mnemonic words separated by spaces.",
                            "请输入 12 个助记词，用空格分隔。"));
        printf("%s\n", L3("Фраза проверяется только на этом компьютере, никуда не отправляется",
                           "The phrase is checked only on this computer, nothing is sent",
                           "短语仅在本机检查，不会发送到任何地方"));
        printf("%s\n", L3("и не сохраняется. Лимит — сколько первых фраз потока проверить.",
                           "and is not stored. Limit = how many first phrases of the stream to check.",
                           "且不会保存。限额 = 检查数据流前多少条短语。"));
        std::string phrase = read_line(L3("Мнемоника: ", "Mnemonic: ", "助记词："));
        if (phrase.empty()){ printf("%s\n", L3("Ничего не введено - выход.", "Nothing entered - exiting.", "未输入内容 - 退出。")); return 1; }
        std::string lim = read_line(L3("Лимит проверки [100000]: ", "Check limit [100000]: ", "检查限额 [100000]："));
        a.push_back("--check-fallback-phrase"); a.push_back(phrase);
        if (!lim.empty()){ a.push_back("--limit"); a.push_back(lim); }
    } else if (what == "3"){
        printf("\n%s\n", L3("ДЕМО СТАРОГО ДЕФЕКТА. Эти фразы предсказуемы -",
                            "DEMO OF THE OLD DEFECT. These phrases are predictable -",
                            "旧版缺陷演示。这些短语可被预测 -"));
        printf("%s\n", L3("НИКОГДА не переводите на них средства.",
                           "NEVER send funds to them.",
                           "切勿向其转账。"));
        std::string idx = read_line(L3("Номер фразы [0]: ", "Phrase index [0]: ", "短语编号 [0]："));
        a.push_back("--insecure-demo-fallback");
        if (!idx.empty()){ a.push_back("--index"); a.push_back(idx); }
    } else {
        std::string suf = read_line(L3("Желаемое окончание адреса (например TG): ",
                                       "Desired ending of the address (e.g. TG): ",
                                       "期望的地址结尾（如 TG）："));
        if (suf.empty()){ printf("%s\n", L3("Ничего не введено - выход.", "Nothing entered - exiting.", "未输入内容 - 退出。")); return 1; }

        printf("\n%s\n", L3("Режим:", "Mode:", "模式："));
        printf("  1) %s\n", L3("рабочий (рекомендуется) - режим фразы: слова сами задают адрес,",
                                "usable (recommended) - phrase mode: the words themselves set the address,",
                                "可用（推荐）- 短语模式：单词本身决定地址，"));
        printf("     %s\n", L3("поэтому в Telegram после импорта будет ТОТ ЖЕ адрес",
                                "so Telegram shows the SAME address after import",
                                "因此导入后 Telegram 显示相同地址"));
        printf("  2) %s\n", L3("быстрый - режим subwallet: секунды, но Telegram покажет",
                                "fast - subwallet mode: seconds, but Telegram shows",
                                "快速 - 子钱包模式：几秒，但 Telegram 显示"));
        printf("     %s\n", L3("ДРУГОЙ адрес (нужен тот же самый subwalletId)",
                                "a DIFFERENT address (needs the same subwalletId)",
                                "不同的地址（需要相同的 subwalletId）"));
        std::string mode = read_line(L3("Выбор [1]: ", "Choice [1]: ", "选择 [1]："));
        const char *m = (mode == "2") ? "subwallet" : "phrase";

        // GPU only helps in phrase mode (app scheme, 12 words). Probe first, then
        // let the user pick CPU or GPU with the actual device name shown.
        if (std::string(m) == "phrase") {
            gpu::Info gi = gpu::probe();
            if (gi.ok) {
                printf("\n%s\n", L3("Обнаружен GPU:", "GPU detected:", "检测到 GPU："));
                printf("  %s  (%s)\n", gi.device.c_str(), gi.platform.c_str());
                printf("  %s\n", L3("OpenCL-ядро компилируется ~10 с при старте.",
                                    "The OpenCL kernel compiles in ~10 s at startup.",
                                    "启动时 OpenCL 内核编译约需 10 秒。"));
                printf("  1) %s\n", L3("CPU (по умолчанию)", "CPU (default)", "CPU（默认）"));
                printf("  2) %s\n", L3("GPU — ускорение ~30x (только phrase/app/12 слов)",
                                      "GPU — ~30x speedup (phrase/app/12 words only)",
                                      "GPU — 约 30 倍加速（仅 phrase/app/12 词）"));
                std::string g = read_line(L3("Выбор [1]: ", "Choice [1]: ", "选择 [1]："));
                if (g == "2") a.push_back("--gpu");
            } else {
                printf("\n%s\n", L3("GPU не обнаружен — будет использован CPU.",
                                    "No GPU detected — CPU will be used.",
                                    "未检测到 GPU — 将使用 CPU。"));
                if (!gi.error.empty()) printf("  (%s)\n", gi.error.c_str());
            }
        } else {
            printf("\n%s\n", L3("В режиме subwallet GPU не используется (и так очень быстро).",
                                "GPU is not used in subwallet mode (already very fast).",
                                "subwallet 模式不使用 GPU（已经非常快）。"));
        }

        a.push_back("--mode"); a.push_back(m);
        a.push_back("--suffix"); a.push_back(suf);
    }

    printf("\n%s\n\n", L3("Запуск...", "Starting...", "启动..."));

    std::vector<char *> av;
    for (auto &x : a) av.push_back(const_cast<char *>(x.c_str()));
    return run_main((int)av.size(), av.data());
}

int main(int argc, char **argv){
    setup_utf8_console();               // Russian text needs a UTF-8 console
    if (const char *e = std::getenv("WTM_LANG")) g_lang = parse_lang(e);
    const bool interactive = launched_by_double_click();

    int rc;
    if ((argc == 2 && std::string(argv[1]) == "--wizard") || (interactive && argc == 1))
        rc = interactive_wizard();
    else
        rc = run_main(argc, argv);

    if (interactive){
        printf("\nPress Enter to exit...");
        fflush(stdout);
        (void)getchar();
    }
    return rc;
}
#endif // WTM_LIB
