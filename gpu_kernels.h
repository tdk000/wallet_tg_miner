// ===========================================================================
//  gpu_kernels.h - embedded OpenCL source for the `app` derivation.
//
//      mnemonic -> PBKDF2-HMAC-SHA512(salt="mnemonic", 2048) -> seed(64)
//               -> SLIP-0010 ed25519, path m/44'/607'/0'      -> secret(32)
//
//  Only the 32-byte secret is produced on the GPU; ed25519 public-key
//  derivation and the TON address hashing stay on the CPU.  The kernel is
//  byte-for-byte equivalent to app_derive_secret() in wallet_tg_miner.cpp
//  (see `--gpu-test`).  The 2048-iteration inner loop is specialized for
//  64-byte messages so that no private-memory scratch buffer is touched.
//
//  Note: the #pragma-unroll directives were deliberately left out.  On the
//  AMD gfx803 driver they change the throughput by <1% but make the OpenCL
//  kernel build roughly 1.6x slower, so plain loops are used instead.
// ===========================================================================
#ifndef WALLET_TG_MINER_KERNELS_H
#define WALLET_TG_MINER_KERNELS_H

static const char *GPU_APP_KERNEL_SRC = R"OCL(

__constant ulong K512[80] = {
    0x428a2f98d728ae22UL,0x7137449123ef65cdUL,0xb5c0fbcfec4d3b2fUL,0xe9b5dba58189dbbcUL,
    0x3956c25bf348b538UL,0x59f111f1b605d019UL,0x923f82a4af194f9bUL,0xab1c5ed5da6d8118UL,
    0xd807aa98a3030242UL,0x12835b0145706fbeUL,0x243185be4ee4b28cUL,0x550c7dc3d5ffb4e2UL,
    0x72be5d74f27b896fUL,0x80deb1fe3b1696b1UL,0x9bdc06a725c71235UL,0xc19bf174cf692694UL,
    0xe49b69c19ef14ad2UL,0xefbe4786384f25e3UL,0x0fc19dc68b8cd5b5UL,0x240ca1cc77ac9c65UL,
    0x2de92c6f592b0275UL,0x4a7484aa6ea6e483UL,0x5cb0a9dcbd41fbd4UL,0x76f988da831153b5UL,
    0x983e5152ee66dfabUL,0xa831c66d2db43210UL,0xb00327c898fb213fUL,0xbf597fc7beef0ee4UL,
    0xc6e00bf33da88fc2UL,0xd5a79147930aa725UL,0x06ca6351e003826fUL,0x142929670a0e6e70UL,
    0x27b70a8546d22ffcUL,0x2e1b21385c26c926UL,0x4d2c6dfc5ac42aedUL,0x53380d139d95b3dfUL,
    0x650a73548baf63deUL,0x766a0abb3c77b2a8UL,0x81c2c92e47edaee6UL,0x92722c851482353bUL,
    0xa2bfe8a14cf10364UL,0xa81a664bbc423001UL,0xc24b8b70d0f89791UL,0xc76c51a30654be30UL,
    0xd192e819d6ef5218UL,0xd69906245565a910UL,0xf40e35855771202aUL,0x106aa07032bbd1b8UL,
    0x19a4c116b8d2d0c8UL,0x1e376c085141ab53UL,0x2748774cdf8eeb99UL,0x34b0bcb5e19b48a8UL,
    0x391c0cb3c5c95a63UL,0x4ed8aa4ae3418acbUL,0x5b9cca4f7763e373UL,0x682e6ff3d6b2b8a3UL,
    0x748f82ee5defb2fcUL,0x78a5636f43172f60UL,0x84c87814a1f0ab72UL,0x8cc702081a6439ecUL,
    0x90befffa23631e28UL,0xa4506cebde82bde9UL,0xbef9a3f7b2c67915UL,0xc67178f2e372532bUL,
    0xca273eceea26619cUL,0xd186b8c721c0c207UL,0xeada7dd6cde0eb1eUL,0xf57d4f7fee6ed178UL,
    0x06f067aa72176fbaUL,0x0a637dc5a2c898a6UL,0x113f9804bef90daeUL,0x1b710b35131c471bUL,
    0x28db77f523047d84UL,0x32caab7b40c72493UL,0x3c9ebe0a15c9bebcUL,0x431d67c49c100d4cUL,
    0x4cc5d4becb3e42b6UL,0x597f299cfc657e2aUL,0x5fcb6fab3ad6faecUL,0x6c44198c4a475817UL
};

#define ROTR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))

inline ulong load_be64(__private const uchar *p) {
    return ((ulong)p[0] << 56) | ((ulong)p[1] << 48) | ((ulong)p[2] << 40) | ((ulong)p[3] << 32)
         | ((ulong)p[4] << 24) | ((ulong)p[5] << 16) | ((ulong)p[6] << 8)  | ((ulong)p[7]);
}

inline void store_be64(__private uchar *p, ulong v) {
    p[0] = (uchar)(v >> 56); p[1] = (uchar)(v >> 48); p[2] = (uchar)(v >> 40); p[3] = (uchar)(v >> 32);
    p[4] = (uchar)(v >> 24); p[5] = (uchar)(v >> 16); p[6] = (uchar)(v >> 8);  p[7] = (uchar)(v);
}

inline void set_iv(__private ulong h[8]) {
    h[0] = 0x6a09e667f3bcc908UL; h[1] = 0xbb67ae8584caa73bUL;
    h[2] = 0x3c6ef372fe94f82bUL; h[3] = 0xa54ff53a5f1d36f1UL;
    h[4] = 0x510e527fade682d1UL; h[5] = 0x9b05688c2b3e6c1fUL;
    h[6] = 0x1f83d9abfb41bd6bUL; h[7] = 0x5be0cd19137e2179UL;
}

// 80 rounds over a rolling 16-word message schedule.
inline void sha512_rounds(__private ulong h[8], __private ulong w[16]) {
    ulong a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 80; i++) {
        ulong wi = w[i & 15];
        if (i >= 16) {
            ulong s0 = ROTR64(w[(i + 1) & 15], 1) ^ ROTR64(w[(i + 1) & 15], 8) ^ (w[(i + 1) & 15] >> 7);
            ulong s1 = ROTR64(w[(i + 14) & 15], 19) ^ ROTR64(w[(i + 14) & 15], 61) ^ (w[(i + 14) & 15] >> 6);
            wi = wi + s0 + w[(i + 9) & 15] + s1;
            w[i & 15] = wi;
        }
        ulong S1 = ROTR64(e, 14) ^ ROTR64(e, 18) ^ ROTR64(e, 41);
        ulong ch = (e & f) ^ (~e & g);
        ulong t1 = hh + S1 + ch + K512[i] + wi;
        ulong S0 = ROTR64(a, 28) ^ ROTR64(a, 34) ^ ROTR64(a, 39);
        ulong maj = (a & b) ^ (a & c) ^ (b & c);
        ulong t2 = S0 + maj;
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

// Hash a 64-byte message following a 128-byte prefix already absorbed in h.
// Padded block: [ m(64) | 0x80 | zeros | len=(128+64)*8=1536 ].
inline void sha512_64(__private const ulong hinit[8], __private const uchar *m, __private uchar *out) {
    ulong w[16];
    for (int i = 0; i < 8; i++) w[i] = load_be64(m + 8 * i);
    w[8] = 0x8000000000000000UL;
    for (int i = 9; i < 15; i++) w[i] = 0;
    w[15] = 1536UL;
    ulong h[8];
    for (int i = 0; i < 8; i++) h[i] = hinit[i];
    sha512_rounds(h, w);
    for (int i = 0; i < 8; i++) store_be64(out + 8 * i, h[i]);
}

// Generic single-block hash (message length <= 111 bytes) after a 128-byte prefix.
inline void sha512_small(__private const ulong hinit[8], __private const uchar *msg,
                         uint msglen, ulong prefixBytes, __private uchar out[64]) {
    uchar blk[128];
    for (uint i = 0; i < msglen; i++) blk[i] = msg[i];
    blk[msglen] = 0x80;
    for (uint i = msglen + 1; i < 112; i++) blk[i] = 0;
    for (int i = 112; i < 128; i++) blk[i] = 0;
    ulong bits = (prefixBytes + (ulong)msglen) * 8;
    for (int i = 0; i < 8; i++) blk[127 - i] = (uchar)(bits >> (8 * i));
    ulong w[16];
    for (int i = 0; i < 16; i++) w[i] = load_be64(blk + 8 * i);
    ulong h[8];
    for (int i = 0; i < 8; i++) h[i] = hinit[i];
    sha512_rounds(h, w);
    for (int i = 0; i < 8; i++) store_be64(out + 8 * i, h[i]);
}

// Precompute the inner/outer SHA-512 states for HMAC with a fixed key.
inline void hmac_pre(__private ulong ipad[8], __private ulong opad[8],
                     __private const uchar *key, uint keylen) {
    ulong h[8], w[16];
    for (int i = 0; i < 16; i++) {
        ulong v = 0;
        for (int j = 0; j < 8; j++) {
            uint k = (uint)(i * 8 + j);
            uchar kb = (k < keylen) ? key[k] : (uchar)0;
            v = (v << 8) | (ulong)(kb ^ 0x36);
        }
        w[i] = v;
    }
    set_iv(h); sha512_rounds(h, w);
    for (int i = 0; i < 8; i++) ipad[i] = h[i];
    for (int i = 0; i < 16; i++) {
        ulong v = 0;
        for (int j = 0; j < 8; j++) {
            uint k = (uint)(i * 8 + j);
            uchar kb = (k < keylen) ? key[k] : (uchar)0;
            v = (v << 8) | (ulong)(kb ^ 0x5c);
        }
        w[i] = v;
    }
    set_iv(h); sha512_rounds(h, w);
    for (int i = 0; i < 8; i++) opad[i] = h[i];
}

// PBKDF2-HMAC-SHA512, one output block (dkLen = 64).
inline void pbkdf2_64(__private const ulong ipad[8], __private const ulong opad[8],
                      __private const uchar *salt, uint saltlen, uint iters,
                      __private uchar out[64]) {
    uchar msg[12];
    for (uint i = 0; i < saltlen; i++) msg[i] = salt[i];
    msg[saltlen + 0] = 0; msg[saltlen + 1] = 0; msg[saltlen + 2] = 0; msg[saltlen + 3] = 1;
    uchar U[64], T[64], inner[64];
    sha512_small(ipad, msg, saltlen + 4, 128, U);   // U = H(ipad || S||INT(1))
    sha512_64(opad, U, T);                          // T = U1 = PRF(P, S||INT(1))
    for (int j = 0; j < 64; j++) U[j] = T[j];       // U <- U1 (the loop feeds U, not the inner hash)
    for (uint it = 1; it < iters; it++) {
        sha512_64(ipad, U, inner);                  // inner = H(ipad || U_it)
        sha512_64(opad, inner, U);                  // U_{it+1} = PRF(P, U_it)
        for (int j = 0; j < 64; j++) T[j] ^= U[j];
    }
    for (int j = 0; j < 64; j++) out[j] = T[j];
}

inline void hmac512(__private const uchar *key, uint keylen,
                    __private const uchar *msg, uint msglen, __private uchar out[64]) {
    ulong ipad[8], opad[8];
    hmac_pre(ipad, opad, key, keylen);
    uchar inner[64];
    if (msglen == 64) sha512_64(ipad, msg, inner);
    else              sha512_small(ipad, msg, msglen, 128, inner);
    sha512_64(opad, inner, out);
}

__kernel void app_derive(__global const uchar *mnem,
                         __global const uint  *lens,
                         const uint stride,
                         __global uchar *secrets,
                         const uint count)
{
    uint gid = get_global_id(0);
    if (gid >= count) return;

    // copy the mnemonic into private memory
    uchar pw[128];
    uint plen = lens[gid];
    if (plen > 128) plen = 128;
    __global const uchar *src = mnem + (size_t)gid * stride;
    for (uint i = 0; i < plen; i++) pw[i] = src[i];

    ulong ipad[8], opad[8];
    hmac_pre(ipad, opad, pw, plen);

    uchar salt[8] = { 'm','n','e','m','o','n','i','c' };
    uchar seed[64];
    pbkdf2_64(ipad, opad, salt, 8, 2048, seed);

    // SLIP-0010 ed25519, m/44'/607'/0'
    uchar I[64], kc[32], c[32];
    uchar es[12] = { 'e','d','2','5','5','1','9',' ','s','e','e','d' };
    hmac512(es, 12, seed, 64, I);
    for (int i = 0; i < 32; i++) { kc[i] = I[i]; c[i] = I[32 + i]; }

    uint path[3] = { 44u, 607u, 0u };
    for (int p = 0; p < 3; p++) {
        uchar data[37];
        data[0] = 0;
        for (int i = 0; i < 32; i++) data[1 + i] = kc[i];
        uint idx = path[p] | 0x80000000u;
        data[33] = (uchar)(idx >> 24); data[34] = (uchar)(idx >> 16);
        data[35] = (uchar)(idx >> 8);  data[36] = (uchar)(idx);
        hmac512(c, 32, data, 37, I);
        for (int i = 0; i < 32; i++) { kc[i] = I[i]; c[i] = I[32 + i]; }
    }

    __global uchar *o = secrets + (size_t)gid * 32;
    for (int i = 0; i < 32; i++) o[i] = kc[i];
}

)OCL";

#endif // WALLET_TG_MINER_KERNELS_H
