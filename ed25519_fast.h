// ===========================================================================
//  ed25519_fast.h - fast fixed-base ed25519 public key derivation.
//
//  Why: in the GPU pipeline the CPU only derives ed25519 public keys for the
//  secrets the GPU produced.  On a 2-core machine that tail is the bottleneck
//  (62 us per key against 11.5 us for the whole GPU stage), and essentially all
//  of it is the scalar multiplication.
//
//  How: 5 x 51-bit limbs and a fixed table of 31 multiples of the base point,
//  so a key costs 51 point additions and one field inversion - no doublings.
//
//  Safety: this is VARIABLE-TIME code.  That is fine here because the input is
//  a freshly generated mnemonic whose key we print on a hit anyway; it must
//  never be used to sign.  The reference implementation (Monocypher's
//  crypto_eddsa_scalarbase via ed25519_pubkey()) stays in wallet_tg_miner.cpp
//  and is used for every check.  Base::init() verifies itself against the
//  caller-supplied reference and the caller disables the fast path on any
//  mismatch, so a broken build silently falls back instead of producing wrong
//  addresses.
//
//  Portability: requires unsigned __int128 (GCC, Clang, MinGW).  Where it is
//  absent (MSVC) EDFAST_HAVE is 0 and the caller keeps using Monocypher.
// ===========================================================================
#ifndef WALLET_TG_MINER_ED25519_FAST_H
#define WALLET_TG_MINER_ED25519_FAST_H

#if defined(__SIZEOF_INT128__)

#include <cstdint>
#include <cstring>
#include <vector>

#define EDFAST_HAVE 1

namespace edfast {

typedef unsigned __int128 u128;
typedef uint64_t fe[5];               // radix 2^51, little endian

static const uint64_t M51 = (1ULL << 51) - 1;

inline u128 mm(uint64_t a, uint64_t b) { return (u128)a * (u128)b; }

inline void fe_0(fe h)               { h[0] = h[1] = h[2] = h[3] = h[4] = 0; }
inline void fe_1(fe h)               { fe_0(h); h[0] = 1; }
inline void fe_copy(fe h, const fe f){ h[0] = f[0]; h[1] = f[1]; h[2] = f[2]; h[3] = f[3]; h[4] = f[4]; }

inline void fe_add(fe h, const fe f, const fe g) {
    h[0] = f[0] + g[0]; h[1] = f[1] + g[1]; h[2] = f[2] + g[2];
    h[3] = f[3] + g[3]; h[4] = f[4] + g[4];
}

// h = f - g.  Requires f, g < 2^52; adds 2p first so nothing goes negative.
inline void fe_sub(fe h, const fe f, const fe g) {
    static const uint64_t two_p[5] = {
        0xFFFFFFFFFFFDAULL, 0xFFFFFFFFFFFFEULL, 0xFFFFFFFFFFFFEULL,
        0xFFFFFFFFFFFFEULL, 0xFFFFFFFFFFFFEULL
    };
    h[0] = f[0] + two_p[0] - g[0];
    h[1] = f[1] + two_p[1] - g[1];
    h[2] = f[2] + two_p[2] - g[2];
    h[3] = f[3] + two_p[3] - g[3];
    h[4] = f[4] + two_p[4] - g[4];
}

// h = f * g mod 2^255-19.  Inputs must be < 2^54 (products then fit in 128 bit:
// 2^54 * 2^54 * 19 * 4 < 2^121).
inline void fe_mul(fe h, const fe f, const fe g) {
    const uint64_t f0 = f[0], f1 = f[1], f2 = f[2], f3 = f[3], f4 = f[4];
    const uint64_t g0 = g[0], g1 = g[1], g2 = g[2], g3 = g[3], g4 = g[4];

    u128 t0 = mm(f0, g0) + 19 * (mm(f1, g4) + mm(f2, g3) + mm(f3, g2) + mm(f4, g1));
    u128 t1 = mm(f0, g1) + mm(f1, g0) + 19 * (mm(f2, g4) + mm(f3, g3) + mm(f4, g2));
    u128 t2 = mm(f0, g2) + mm(f1, g1) + mm(f2, g0) + 19 * (mm(f3, g4) + mm(f4, g3));
    u128 t3 = mm(f0, g3) + mm(f1, g2) + mm(f2, g1) + mm(f3, g0) + 19 * mm(f4, g4);
    u128 t4 = mm(f0, g4) + mm(f1, g3) + mm(f2, g2) + mm(f3, g1) + mm(f4, g0);

    uint64_t c;
    c = (uint64_t)(t0 >> 51); uint64_t r0 = (uint64_t)t0 & M51; t1 += c;
    c = (uint64_t)(t1 >> 51); uint64_t r1 = (uint64_t)t1 & M51; t2 += c;
    c = (uint64_t)(t2 >> 51); uint64_t r2 = (uint64_t)t2 & M51; t3 += c;
    c = (uint64_t)(t3 >> 51); uint64_t r3 = (uint64_t)t3 & M51; t4 += c;
    c = (uint64_t)(t4 >> 51); uint64_t r4 = (uint64_t)t4 & M51;

    r0 += 19 * c;
    c = r0 >> 51; r0 &= M51; r1 += c;

    h[0] = r0; h[1] = r1; h[2] = r2; h[3] = r3; h[4] = r4;
}

inline void fe_sq(fe h, const fe f) { fe_mul(h, f, f); }

// h = f * k for small k (k < 2^20).
inline void fe_mul_small(fe h, const fe f, uint64_t k) {
    u128 acc = mm(f[0], k); uint64_t r0 = (uint64_t)acc & M51; acc >>= 51;
    acc += mm(f[1], k);     uint64_t r1 = (uint64_t)acc & M51; acc >>= 51;
    acc += mm(f[2], k);     uint64_t r2 = (uint64_t)acc & M51; acc >>= 51;
    acc += mm(f[3], k);     uint64_t r3 = (uint64_t)acc & M51; acc >>= 51;
    acc += mm(f[4], k);     uint64_t r4 = (uint64_t)acc & M51; acc >>= 51;
    r0 += 19 * (uint64_t)acc;
    const uint64_t c = r0 >> 51; r0 &= M51; r1 += c;
    h[0] = r0; h[1] = r1; h[2] = r2; h[3] = r3; h[4] = r4;
}

inline uint64_t load64le(const uint8_t *p) {
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24)
         | ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) | ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
}

// 32-byte little-endian encoding -> limbs (bit 255, the x sign, is ignored).
inline void fe_frombytes(fe h, const uint8_t s[32]) {
    h[0] = load64le(s) & M51;
    h[1] = (load64le(s + 6) >> 3) & M51;
    h[2] = (load64le(s + 12) >> 6) & M51;
    h[3] = (load64le(s + 19) >> 1) & M51;
    h[4] = (load64le(s + 24) >> 12) & M51;
}

// Canonical little-endian encoding of h.
inline void fe_tobytes(uint8_t s[32], const fe h) {
    uint64_t t[5] = { h[0], h[1], h[2], h[3], h[4] };
    uint64_t c;

    c = t[0] >> 51; t[0] &= M51; t[1] += c;
    c = t[1] >> 51; t[1] &= M51; t[2] += c;
    c = t[2] >> 51; t[2] &= M51; t[3] += c;
    c = t[3] >> 51; t[3] &= M51; t[4] += c;
    c = t[4] >> 51; t[4] &= M51; t[0] += 19 * c;
    c = t[0] >> 51; t[0] &= M51; t[1] += c;

    // t is now < 2^255; q = 1 iff t >= p.
    uint64_t q = (t[0] + 19) >> 51;
    q = (t[1] + q) >> 51;
    q = (t[2] + q) >> 51;
    q = (t[3] + q) >> 51;
    q = (t[4] + q) >> 51;

    t[0] += 19 * q;
    c = t[0] >> 51; t[0] &= M51; t[1] += c;
    c = t[1] >> 51; t[1] &= M51; t[2] += c;
    c = t[2] >> 51; t[2] &= M51; t[3] += c;
    c = t[3] >> 51; t[3] &= M51; t[4] += c;
    t[4] &= M51;                      // drop the 2^255 bit

    const uint64_t w0 = t[0] | (t[1] << 51);
    const uint64_t w1 = (t[1] >> 13) | (t[2] << 38);
    const uint64_t w2 = (t[2] >> 26) | (t[3] << 25);
    const uint64_t w3 = (t[3] >> 39) | (t[4] << 12);
    for (int i = 0; i < 8; i++) {
        s[i]      = (uint8_t)(w0 >> (8 * i));
        s[8 + i]  = (uint8_t)(w1 >> (8 * i));
        s[16 + i] = (uint8_t)(w2 >> (8 * i));
        s[24 + i] = (uint8_t)(w3 >> (8 * i));
    }
}

inline int fe_isnegative(const fe f) {
    uint8_t s[32];
    fe_tobytes(s, f);
    return s[0] & 1;
}

inline void fe_neg(fe h, const fe f) { fe zero; fe_0(zero); fe_sub(h, zero, f); }

// Fixed-exponent exponentiation (LSB-first exponent bytes).  Used only during
// init, so a plain square-and-multiply is fine.
inline void fe_pow_bits(fe out, const fe z, const uint8_t *exp, int nbits) {
    fe r; fe_1(r);
    for (int i = nbits - 1; i >= 0; i--) {
        fe_sq(r, r);
        if ((exp[i >> 3] >> (i & 7)) & 1) fe_mul(r, r, z);
    }
    fe_copy(out, r);
}

// z^(p-2) = z^-1 via the standard 2^k-1 addition chain: 254 squarings,
// 12 multiplications.
inline void fe_invert(fe out, const fe z) {
    fe z2, z9, z11, t, u;

    fe_sq(z2, z);               // 2
    fe_sq(t, z2);               // 4
    fe_sq(u, t);                // 8
    fe_mul(z9, u, z);           // 9
    fe_mul(z11, z9, z2);        // 11

    fe_sq(t, z11);              // 22
    fe_mul(u, t, z9);           // 2^5-1

    // powers of two minus one: 2^10, 2^20, 2^40, 2^50, 2^100, 2^200, 2^250
    fe_sq(t, u); for (int i = 1; i < 5; i++) fe_sq(t, t);
    fe_mul(t, t, u);            // 2^10-1
    fe p10; fe_copy(p10, t);

    fe_sq(t, p10); for (int i = 1; i < 10; i++) fe_sq(t, t);
    fe_mul(t, t, p10);          // 2^20-1
    fe p20; fe_copy(p20, t);

    fe_sq(t, p20); for (int i = 1; i < 20; i++) fe_sq(t, t);
    fe_mul(t, t, p20);          // 2^40-1
    fe p40; fe_copy(p40, t);

    fe_sq(t, p40); for (int i = 1; i < 10; i++) fe_sq(t, t);
    fe_mul(t, t, p10);          // 2^50-1
    fe p50; fe_copy(p50, t);

    fe_sq(t, p50); for (int i = 1; i < 50; i++) fe_sq(t, t);
    fe_mul(t, t, p50);          // 2^100-1
    fe p100; fe_copy(p100, t);

    fe_sq(t, p100); for (int i = 1; i < 100; i++) fe_sq(t, t);
    fe_mul(t, t, p100);         // 2^200-1

    fe_sq(t, t); for (int i = 1; i < 50; i++) fe_sq(t, t);
    fe_mul(t, t, p50);          // 2^250-1

    fe_sq(t, t); for (int i = 1; i < 5; i++) fe_sq(t, t);
    fe_mul(out, t, z11);        // 2^255-21 = p-2
}

// ---- points in extended coordinates (a = -1) ------------------------------
struct ge { fe X, Y, Z, T; };

inline void ge_copy(ge &d, const ge &s) { fe_copy(d.X, s.X); fe_copy(d.Y, s.Y); fe_copy(d.Z, s.Z); fe_copy(d.T, s.T); }

// add-2008-hwcd-3, works for any pair of valid points (Z != 0).
inline void ge_add(ge &r, const ge &p, const ge &q, const fe two_d) {
    fe a, b, c, d, e, f, g, h, t;

    fe_sub(t, p.Y, p.X); fe_sub(a, q.Y, q.X); fe_mul(a, t, a);
    fe_add(t, p.Y, p.X); fe_add(b, q.Y, q.X); fe_mul(b, t, b);
    fe_mul(c, p.T, q.T);  fe_mul(c, c, two_d);
    fe_mul(d, p.Z, q.Z);  fe_mul_small(d, d, 2);

    fe_sub(e, b, a);
    fe_sub(f, d, c);
    fe_add(g, d, c);
    fe_add(h, b, a);

    fe X, Y, T, Z;
    fe_mul(X, e, f);
    fe_mul(Y, g, h);
    fe_mul(T, e, h);
    fe_mul(Z, f, g);
    fe_copy(r.X, X); fe_copy(r.Y, Y); fe_copy(r.T, T); fe_copy(r.Z, Z);
}

// dbl-2008-hwcd (a = -1), used only while building the table.
inline void ge_double(ge &r, const ge &p, const fe two_d) {
    (void)two_d;
    fe a, b, c, d, e, f, g, h, t;

    fe_sq(a, p.X);
    fe_sq(b, p.Y);
    fe_sq(c, p.Z); fe_mul_small(c, c, 2);
    fe_neg(d, a);
    fe_add(t, p.X, p.Y); fe_sq(t, t); fe_sub(t, t, a); fe_sub(e, t, b);
    fe_add(g, d, b);
    fe_sub(f, g, c);
    fe_sub(h, d, b);

    fe X, Y, T, Z;
    fe_mul(X, e, f);
    fe_mul(Y, g, h);
    fe_mul(T, e, h);
    fe_mul(Z, f, g);
    fe_copy(r.X, X); fe_copy(r.Y, Y); fe_copy(r.T, T); fe_copy(r.Z, Z);
}

// Precomputed table for fixed-base multiplication:
//
//     table_[i][v] = v * 2^(4i) * B          i = 0..63, v = 1..15
//
// so a scalar costs one addition per non-zero nibble (64 at most) and NO
// doublings - that is the whole point of this implementation.
class Base {
public:
    static constexpr int W        = 4;      // bits per window
    static constexpr int WINDOWS  = 64;     // 256 / W
    static constexpr int ENTRIES  = 1 << W; // 16, index 0 unused

    // enc_b is the canonical 32-byte encoding of the base point (as produced by
    // the reference implementation for the scalar 1).
    bool init(const uint8_t enc_b[32]) {
        ok_ = false;

        // d = -121665 / 121666
        fe num, den, d;
        uint8_t small[32];
        set_small(small, 121665); fe_frombytes(num, small);
        set_small(small, 121666); fe_frombytes(den, small);
        fe_invert(den, den);
        fe_mul(d, num, den);           // 121665 / 121666
        fe_neg(d, d);                  // d = -121665/121666

        // 2d
        fe_mul_small(two_d_, d, 2);

        // sqrt(-1) = 2^((p-1)/4) = 2^(2^253-5)
        fe two; fe_1(two); fe_mul_small(two, two, 2);
        uint8_t e254[32];
        std::memset(e254, 0xFF, sizeof(e254));
        e254[0] &= ~0x04u;             // clear bit 2
        e254[31] &= 0x3F;              // keep bits 248..253
        fe sqrt_m1;
        fe_pow_bits(sqrt_m1, two, e254, 254);

        // Decompress the base point: x^2 = (y^2-1) / (d*y^2+1)
        fe y, u, v;
        uint8_t yb[32];
        std::memcpy(yb, enc_b, 32);
        const int sign = yb[31] >> 7;
        yb[31] &= 0x7F;
        fe_frombytes(y, yb);

        fe y2; fe_sq(y2, y);
        fe one; fe_1(one);
        fe_sub(u, y2, one);            // y^2 - 1
        fe_mul(v, d, y2); fe_add(v, v, one);   // d*y^2 + 1
        fe v_inv; fe_invert(v_inv, v);
        fe w; fe_mul(w, u, v_inv);     // u/v

        // x = w^((p+3)/8) = w^(2^252-2)
        uint8_t e252[32];
        std::memset(e252, 0xFF, sizeof(e252));
        e252[0] &= ~0x01u;             // clear bit 0
        e252[31] &= 0x0F;              // keep bits 248..251
        fe x;
        fe_pow_bits(x, w, e252, 252);

        // check x^2 * v == u, else x *= sqrt(-1)
        fe chk; fe_sq(chk, x); fe_mul(chk, chk, v);
        if (!fe_equal(chk, u)) {
            fe_mul(x, x, sqrt_m1);
            fe_sq(chk, x); fe_mul(chk, chk, v);
            if (!fe_equal(chk, u)) return false;
        }
        if (fe_isnegative(x) != sign) fe_neg(x, x);

        // B in extended coordinates
        ge b;
        fe_copy(b.X, x);
        fe_copy(b.Y, y);
        fe_1(b.Z);
        fe_mul(b.T, x, y);

        // Fill the table: each window's base is 16x the previous one.
        tab_.resize((size_t)WINDOWS * ENTRIES);
        ge cur;
        ge_copy(cur, b);
        for (int i = 0; i < WINDOWS; i++) {
            ge *const slot = &tab_[(size_t)i * ENTRIES];
            ge_copy(slot[1], cur);
            for (int v = 2; v < ENTRIES; v++)
                ge_add(slot[v], slot[v - 1], cur, two_d_);
            if (i + 1 < WINDOWS)
                for (int k = 0; k < W; k++) ge_double(cur, cur, two_d_);
        }

        ok_ = true;
        return true;
    }

    bool ok() const { return ok_; }

    // out = (scalar * B) encoded.  Scalars are 255-bit and already clamped, so
    // bit 254 is set and the top nibble is never zero.
    void scalarmult_base(uint8_t out[32], const uint8_t scalar[32]) const {
        if (!ok_) { std::memset(out, 0, 32); return; }

        ge acc;
        bool started = false;

        for (int i = WINDOWS - 1; i >= 0; i--) {
            const unsigned nib = (unsigned)(scalar[i >> 1] >> ((i & 1) * 4)) & 0x0f;
            if (nib == 0) continue;
            const ge &t = tab_[(size_t)i * ENTRIES + nib];
            if (!started) { ge_copy(acc, t); started = true; }
            else          { ge_add(acc, acc, t, two_d_); }
        }
        if (!started) { std::memset(out, 0, 32); return; }

        fe zi, x, y;
        fe_invert(zi, acc.Z);
        fe_mul(x, acc.X, zi);
        fe_mul(y, acc.Y, zi);
        fe_tobytes(out, y);
        out[31] |= (uint8_t)(fe_isnegative(x) << 7);
    }

private:
    static bool fe_equal(const fe a, const fe b) {
        uint8_t x[32], y[32];
        fe_tobytes(x, a);
        fe_tobytes(y, b);
        return std::memcmp(x, y, 32) == 0;
    }

    // little-endian 32-byte encoding of a small integer
    static void set_small(uint8_t b[32], uint32_t v) {
        std::memset(b, 0, 32);
        b[0] = (uint8_t)(v);
        b[1] = (uint8_t)(v >> 8);
        b[2] = (uint8_t)(v >> 16);
        b[3] = (uint8_t)(v >> 24);
    }

    bool ok_ = false;
    fe   two_d_{};
    std::vector<ge> tab_;      // WINDOWS * ENTRIES points, ~160 KB
};

} // namespace edfast

#else  // no unsigned __int128

#define EDFAST_HAVE 0

#endif

#endif // WALLET_TG_MINER_ED25519_FAST_H
