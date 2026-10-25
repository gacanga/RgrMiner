/**
 * RgrMiner - RIGOR (ORU) proof of work "OpuHash" / rgrpow01 - shared host / device core
 *
 *   pow = SHA3-256( "RGRPOW01" || header[80] )          (FIPS 202, padding byte 0x06)
 *   (rigorchain core/src/rigor_kernel/pow_digest.cpp, SPEC-008)
 *
 * The 88-byte preimage fits in ONE rate block of 136 bytes, so one nonce = ONE Keccak-f[1600]
 * permutation.  Lanes are little-endian 64-bit words of the padded block:
 *   lane 0       "RGRPOW01"
 *   lanes 1..9   header bytes 0..71   (version, prev hash, merkle root, time)
 *   lane 10      header bytes 72..79  = nBits (low 32 bits) | nNonce (high 32 bits)
 *   lane 11      0x06 (SHA3 domain + first padding bit), lane 16 = 0x80 << 56, others 0.
 * The digest is read as a little-endian uint256 compared with the target: its 64 most significant
 * bits are lane 3 of the output state.
 *
 * Keccak-f[1600] lanes are held as uint2 { x = low 32 bits, y = high 32 bits } so that Pascal works
 * on 32-bit registers (funnel shifts for the rotations, LOP3 for xor / chi).  Helpers taken from
 * the sha3-256t core of FjarMiner (same author, GPL-3.0).
 *
 * Per nonce on the GPU:
 *   - round 0: theta / rho / pi of the 14 nonce-independent lanes are precomputed on the host
 *     (job.b); only the 11 lanes reached by the nonce (lane 10 itself, columns 1 and 4 through
 *     theta) are xored with the rotated nonce, then chi + iota;
 *   - rounds 1..22: full rounds;
 *   - round 23: only lane 3 is computed and compared with the 64 high bits of the target.
 * The host recomputes the full digest of every candidate with the reference code below.
 *
 * This header compiles with nvcc (device + host) and with a plain C++ compiler: the CPU emulation
 * test runs the exact GPU path on the CPU.
 */
#pragma once

#include <stdint.h>
#include <string.h>

#if defined(__CUDACC__)
#define S3_HD __host__ __device__ __forceinline__
#define S3_PRAGMA(x) _Pragma(#x)
#else
struct uint2 { uint32_t x, y; };
static inline uint2 make_uint2(uint32_t x, uint32_t y) { uint2 r; r.x = x; r.y = y; return r; }
#define S3_HD static inline
#define S3_PRAGMA(x)
#endif

#define RGR_PAD 0x06u                 /* SHA3 domain separation + first padding bit (FIPS 202) */
#define RGR_RATE 136                  /* SHA3-256 rate in bytes */
#define RGR_DOMAIN "RGRPOW01"         /* 8-byte PoW domain tag */

/* ------------------------------------------------------------------ 32-bit helpers */

S3_HD uint64_t s3_u64(const uint2 a) { return ((uint64_t)a.y << 32) | a.x; }
S3_HD uint2 s3_u2(const uint64_t v) { return make_uint2((uint32_t)v, (uint32_t)(v >> 32)); }

S3_HD uint2 s3_xor(const uint2 a, const uint2 b) { return make_uint2(a.x ^ b.x, a.y ^ b.y); }
S3_HD uint2 s3_xor3(const uint2 a, const uint2 b, const uint2 c)
{
	return make_uint2(a.x ^ b.x ^ c.x, a.y ^ b.y ^ c.y);
}
S3_HD uint2 s3_xor5(const uint2 a, const uint2 b, const uint2 c, const uint2 d, const uint2 e)
{
	return make_uint2(a.x ^ b.x ^ c.x ^ d.x ^ e.x, a.y ^ b.y ^ c.y ^ d.y ^ e.y);
}
/* Keccak chi: a ^ (~b & c) */
S3_HD uint2 s3_chi(const uint2 a, const uint2 b, const uint2 c)
{
	return make_uint2(a.x ^ (~b.x & c.x), a.y ^ (~b.y & c.y));
}

/* 64-bit rotate left by a compile-time amount N (0..63); R = 0 funnel shifts, 1 shift + add */
template <int N, int R>
S3_HD uint2 s3_rol(const uint2 a)
{
	if (N == 0) return a;
	if (N == 32) return make_uint2(a.y, a.x);
#if defined(__CUDA_ARCH__)
	if (R == 1) {
		const uint32_t m = (uint32_t)(N & 31);
		if (N < 32)
			return make_uint2((a.x << m) + (a.y >> (32 - m)), (a.y << m) + (a.x >> (32 - m)));
		return make_uint2((a.y << m) + (a.x >> (32 - m)), (a.x << m) + (a.y >> (32 - m)));
	}
	if (N < 32)
		return make_uint2(__funnelshift_l(a.y, a.x, N & 31), __funnelshift_l(a.x, a.y, N & 31));
	return make_uint2(__funnelshift_l(a.x, a.y, (N - 32) & 31), __funnelshift_l(a.y, a.x, (N - 32) & 31));
#else
	const uint64_t v = s3_u64(a);
	return s3_u2((v << (N & 63)) | (v >> ((64 - N) & 63)));
#endif
}

/* ------------------------------------------------------------------ round constants */

static const uint64_t h_rgr_rc[24] = {
	0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808AULL, 0x8000000080008000ULL,
	0x000000000000808BULL, 0x0000000080000001ULL, 0x8000000080008081ULL, 0x8000000000008009ULL,
	0x000000000000008AULL, 0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000AULL,
	0x000000008000808BULL, 0x800000000000008BULL, 0x8000000000008089ULL, 0x8000000000008003ULL,
	0x8000000000008002ULL, 0x8000000000000080ULL, 0x000000000000800AULL, 0x800000008000000AULL,
	0x8000000080008081ULL, 0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL
};

#if defined(__CUDACC__)
__constant__ uint2 c_rgr_rc[24] = {
	{ 0x00000001, 0x00000000 }, { 0x00008082, 0x00000000 }, { 0x0000808A, 0x80000000 }, { 0x80008000, 0x80000000 },
	{ 0x0000808B, 0x00000000 }, { 0x80000001, 0x00000000 }, { 0x80008081, 0x80000000 }, { 0x00008009, 0x80000000 },
	{ 0x0000008A, 0x00000000 }, { 0x00000088, 0x00000000 }, { 0x80008009, 0x00000000 }, { 0x8000000A, 0x00000000 },
	{ 0x8000808B, 0x00000000 }, { 0x0000008B, 0x80000000 }, { 0x00008089, 0x80000000 }, { 0x00008003, 0x80000000 },
	{ 0x00008002, 0x80000000 }, { 0x00000080, 0x80000000 }, { 0x0000800A, 0x00000000 }, { 0x8000000A, 0x80000000 },
	{ 0x80008081, 0x80000000 }, { 0x00008080, 0x80000000 }, { 0x80000001, 0x00000000 }, { 0x80008008, 0x80000000 }
};
#endif

S3_HD uint2 s3_rc(const int i)
{
#if defined(__CUDA_ARCH__)
	return c_rgr_rc[i];
#else
	return s3_u2(h_rgr_rc[i]);
#endif
}

/* ------------------------------------------------------------------ Keccak-f[1600] rounds */

/* chi of the 5 rows + iota */
S3_HD void s3_chi_iota(uint2 s[25], const uint2 rc)
{
	uint2 v, w;
	S3_PRAGMA(unroll)
	for (int j = 0; j < 25; j += 5) {
		v = s[j]; w = s[j + 1];
		s[j    ] = s3_chi(v, w, s[j + 2]);
		s[j + 1] = s3_chi(w, s[j + 2], s[j + 3]);
		s[j + 2] = s3_chi(s[j + 2], s[j + 3], s[j + 4]);
		s[j + 3] = s3_chi(s[j + 3], s[j + 4], v);
		s[j + 4] = s3_chi(s[j + 4], v, w);
	}
	s[0] = s3_xor(s[0], rc);
}

/* one full round (theta, rho, pi, chi, iota), in place */
template <int R>
S3_HD void s3_round(uint2 s[25], const uint2 rc)
{
	uint2 t[5], u[5], v;

	t[0] = s3_xor5(s[0], s[5], s[10], s[15], s[20]);
	t[1] = s3_xor5(s[1], s[6], s[11], s[16], s[21]);
	t[2] = s3_xor5(s[2], s[7], s[12], s[17], s[22]);
	t[3] = s3_xor5(s[3], s[8], s[13], s[18], s[23]);
	t[4] = s3_xor5(s[4], s[9], s[14], s[19], s[24]);
	u[0] = s3_rol<1, R>(t[0]);
	u[1] = s3_rol<1, R>(t[1]);
	u[2] = s3_rol<1, R>(t[2]);
	u[3] = s3_rol<1, R>(t[3]);
	u[4] = s3_rol<1, R>(t[4]);
	s[ 4] = s3_xor3(s[ 4], t[3], u[0]); s[ 9] = s3_xor3(s[ 9], t[3], u[0]); s[14] = s3_xor3(s[14], t[3], u[0]);
	s[19] = s3_xor3(s[19], t[3], u[0]); s[24] = s3_xor3(s[24], t[3], u[0]);
	s[ 0] = s3_xor3(s[ 0], t[4], u[1]); s[ 5] = s3_xor3(s[ 5], t[4], u[1]); s[10] = s3_xor3(s[10], t[4], u[1]);
	s[15] = s3_xor3(s[15], t[4], u[1]); s[20] = s3_xor3(s[20], t[4], u[1]);
	s[ 1] = s3_xor3(s[ 1], t[0], u[2]); s[ 6] = s3_xor3(s[ 6], t[0], u[2]); s[11] = s3_xor3(s[11], t[0], u[2]);
	s[16] = s3_xor3(s[16], t[0], u[2]); s[21] = s3_xor3(s[21], t[0], u[2]);
	s[ 2] = s3_xor3(s[ 2], t[1], u[3]); s[ 7] = s3_xor3(s[ 7], t[1], u[3]); s[12] = s3_xor3(s[12], t[1], u[3]);
	s[17] = s3_xor3(s[17], t[1], u[3]); s[22] = s3_xor3(s[22], t[1], u[3]);
	s[ 3] = s3_xor3(s[ 3], t[2], u[4]); s[ 8] = s3_xor3(s[ 8], t[2], u[4]); s[13] = s3_xor3(s[13], t[2], u[4]);
	s[18] = s3_xor3(s[18], t[2], u[4]); s[23] = s3_xor3(s[23], t[2], u[4]);

	/* rho + pi */
	v = s[1];
	s[ 1] = s3_rol<44, R>(s[ 6]); s[ 6] = s3_rol<20, R>(s[ 9]); s[ 9] = s3_rol<61, R>(s[22]); s[22] = s3_rol<39, R>(s[14]);
	s[14] = s3_rol<18, R>(s[20]); s[20] = s3_rol<62, R>(s[ 2]); s[ 2] = s3_rol<43, R>(s[12]); s[12] = s3_rol<25, R>(s[13]);
	s[13] = s3_rol<8, R>(s[19]); s[19] = s3_rol<56, R>(s[23]); s[23] = s3_rol<41, R>(s[15]); s[15] = s3_rol<27, R>(s[ 4]);
	s[ 4] = s3_rol<14, R>(s[24]); s[24] = s3_rol<2, R>(s[21]); s[21] = s3_rol<55, R>(s[ 8]); s[ 8] = s3_rol<45, R>(s[16]);
	s[16] = s3_rol<36, R>(s[ 5]); s[ 5] = s3_rol<28, R>(s[ 3]); s[ 3] = s3_rol<21, R>(s[18]); s[18] = s3_rol<15, R>(s[17]);
	s[17] = s3_rol<10, R>(s[11]); s[11] = s3_rol<6, R>(s[ 7]); s[ 7] = s3_rol<3, R>(s[10]); s[10] = s3_rol<1, R>(v);

	s3_chi_iota(s, rc);
}

/* rounds 1..22; UNROLL (1, 2, 11 or 22) = rounds per loop iteration */
template <int UNROLL, int R>
S3_HD void s3_rounds_1_22(uint2 s[25])
{
	S3_PRAGMA(unroll 1)
	for (int r = 1; r < 23; r += UNROLL) {
		S3_PRAGMA(unroll)
		for (int k = 0; k < UNROLL; k++)
			s3_round<R>(s, s3_rc(r + k));
	}
}

/* round 23 restricted to lanes 0..3 (the 256-bit digest) */
template <int R>
S3_HD void s3_round23_digest(const uint2 s[25], uint2 out[4])
{
	uint2 t[5];
	t[0] = s3_xor5(s[0], s[5], s[10], s[15], s[20]);
	t[1] = s3_xor5(s[1], s[6], s[11], s[16], s[21]);
	t[2] = s3_xor5(s[2], s[7], s[12], s[17], s[22]);
	t[3] = s3_xor5(s[3], s[8], s[13], s[18], s[23]);
	t[4] = s3_xor5(s[4], s[9], s[14], s[19], s[24]);
	const uint2 b0 = s3_xor3(s[ 0], t[4], s3_rol<1, R>(t[1]));
	const uint2 b1 = s3_rol<44, R>(s3_xor3(s[ 6], t[0], s3_rol<1, R>(t[2])));
	const uint2 b2 = s3_rol<43, R>(s3_xor3(s[12], t[1], s3_rol<1, R>(t[3])));
	const uint2 b3 = s3_rol<21, R>(s3_xor3(s[18], t[2], s3_rol<1, R>(t[4])));
	const uint2 b4 = s3_rol<14, R>(s3_xor3(s[24], t[3], s3_rol<1, R>(t[0])));
	out[0] = s3_xor(s3_chi(b0, b1, b2), s3_rc(23));
	out[1] = s3_chi(b1, b2, b3);
	out[2] = s3_chi(b2, b3, b4);
	out[3] = s3_chi(b3, b4, b0);
}

/* round 23 restricted to lane 3: digest bytes 24..31 = the 64 most significant bits */
template <int R>
S3_HD uint2 s3_round23_lane3(const uint2 s[25])
{
	const uint2 t0 = s3_xor5(s[0], s[5], s[10], s[15], s[20]);
	const uint2 t1 = s3_xor5(s[1], s[6], s[11], s[16], s[21]);
	const uint2 t2 = s3_xor5(s[2], s[7], s[12], s[17], s[22]);
	const uint2 t3 = s3_xor5(s[3], s[8], s[13], s[18], s[23]);
	const uint2 t4 = s3_xor5(s[4], s[9], s[14], s[19], s[24]);
	const uint2 b0 = s3_xor3(s[ 0], t4, s3_rol<1, R>(t1));
	const uint2 b3 = s3_rol<21, R>(s3_xor3(s[18], t2, s3_rol<1, R>(t4)));
	const uint2 b4 = s3_rol<14, R>(s3_xor3(s[24], t3, s3_rol<1, R>(t0)));
	return s3_chi(b3, b4, b0);
}

/* ------------------------------------------------------------------ job (host-prepared constants) */

/* passed by value as a kernel parameter: the GPU reads it from the constant bank */
struct rgr_job {
	uint2 b[25];     /* round 0 after theta + rho + pi, nonce = 0 */
	uint2 target;    /* target words 6 (x) and 7 (y): the 64 most significant bits */
};

static inline uint64_t rgr_rotl64(const uint64_t x, const int n) { return n ? ((x << n) | (x >> (64 - n))) : x; }

static const int rgr_rho[25] = { 0, 1, 62, 28, 27, 36, 44, 6, 55, 20, 3, 10, 43, 25, 39, 41, 45, 15, 21, 8, 18, 2, 61, 56, 14 };

/* padded single rate block of the PoW preimage as 17 lanes (the 8 capacity lanes are 0) */
static inline void rgr_preimage_lanes(uint64_t a[25], const uint8_t header80[80])
{
	uint8_t blk[200];
	memset(blk, 0, sizeof(blk));
	memcpy(blk, RGR_DOMAIN, 8);
	memcpy(blk + 8, header80, 80);
	blk[88] = (uint8_t)RGR_PAD;
	blk[RGR_RATE - 1] |= 0x80;
	for (int i = 0; i < 25; i++) {
		uint64_t v = 0;
		for (int k = 7; k >= 0; k--) v = (v << 8) | blk[8 * i + k];
		a[i] = v;
	}
}

/*
 * header80: the 80-byte block header (its nonce bytes 76..79 are ignored),
 * ptarget : the target as 8 little-endian 32-bit words (word 7 = most significant).
 */
static inline void rgr_prepare_job(rgr_job *job, const uint8_t header80[80], const uint32_t ptarget[8])
{
	uint64_t a[25], c[5], d[5];
	uint8_t h[80];
	memcpy(h, header80, 80);
	memset(h + 76, 0, 4);                       /* nonce = 0 */
	rgr_preimage_lanes(a, h);
	for (int x = 0; x < 5; x++) c[x] = a[x] ^ a[x + 5] ^ a[x + 10] ^ a[x + 15] ^ a[x + 20];
	for (int x = 0; x < 5; x++) d[x] = c[(x + 4) % 5] ^ rgr_rotl64(c[(x + 1) % 5], 1);
	for (int i = 0; i < 25; i++) {
		const int x = i % 5, y = i / 5;
		const int dst = y + 5 * ((2 * x + 3 * y) % 5);
		job->b[dst] = s3_u2(rgr_rotl64(a[i] ^ d[x], rgr_rho[i]));
	}
	job->target = make_uint2(ptarget[6], ptarget[7]);
}

/* ------------------------------------------------------------------ per-nonce pipeline */

/*
 * nonce: the header nNonce (bytes 76..79, little-endian) = high half of lane 10.
 * Theta of round 0: column 0 parity carries N = nonce << 32, so
 *   d[1] = c[0] ^ rol(c[2],1) carries N        -> lanes 1, 6, 11, 16, 21 get ^ N
 *   d[4] = c[3] ^ rol(c[0],1) carries rol(N,1)  -> lanes 4, 9, 14, 19, 24 get ^ rol(N,1)
 *   lane 10 itself carries N (d[0] is constant).
 * FULL = true : out[0..3] = complete digest (tests);  FULL = false: out[3] only (mining)
 */
template <int UNROLL, bool FULL, int R>
S3_HD void rgr_hash_nonce(const rgr_job &job, const uint32_t nonce, uint2 out[4])
{
	uint2 s[25];
	const uint2 N = make_uint2(0, nonce);
	const uint2 M = make_uint2(nonce >> 31, nonce << 1);   /* rol(N, 1) */

	/* round 0: constant lanes */
	s[ 0] = job.b[ 0]; s[ 2] = job.b[ 2]; s[ 3] = job.b[ 3]; s[ 5] = job.b[ 5]; s[ 9] = job.b[ 9];
	s[11] = job.b[11]; s[12] = job.b[12]; s[14] = job.b[14]; s[16] = job.b[16]; s[18] = job.b[18];
	s[19] = job.b[19]; s[20] = job.b[20]; s[21] = job.b[21]; s[23] = job.b[23];
	/* round 0: lanes reached by the nonce (destination after pi, rotation of the source lane) */
	s[ 1] = s3_xor(job.b[ 1], s3_rol<44, R>(N));   /* lane 6  */
	s[ 4] = s3_xor(job.b[ 4], s3_rol<14, R>(M));   /* lane 24 */
	s[ 6] = s3_xor(job.b[ 6], s3_rol<20, R>(M));   /* lane 9  */
	s[ 7] = s3_xor(job.b[ 7], s3_rol< 3, R>(N));   /* lane 10 */
	s[ 8] = s3_xor(job.b[ 8], s3_rol<45, R>(N));   /* lane 16 */
	s[10] = s3_xor(job.b[10], s3_rol< 1, R>(N));   /* lane 1  */
	s[13] = s3_xor(job.b[13], s3_rol< 8, R>(M));   /* lane 19 */
	s[15] = s3_xor(job.b[15], s3_rol<27, R>(M));   /* lane 4  */
	s[17] = s3_xor(job.b[17], s3_rol<10, R>(N));   /* lane 11 */
	s[22] = s3_xor(job.b[22], s3_rol<39, R>(M));   /* lane 14 */
	s[24] = s3_xor(job.b[24], s3_rol< 2, R>(N));   /* lane 21 */
	s3_chi_iota(s, s3_rc(0));

	s3_rounds_1_22<UNROLL, R>(s);
	if (FULL) {
		s3_round23_digest<R>(s, out);
	} else {
		out[3] = s3_round23_lane3<R>(s);
	}
}

/* 64 high bits of the digest <= 64 high bits of the target (candidate; the host checks all 256 bits) */
S3_HD bool rgr_hi_le(const uint2 h, const uint2 t)
{
	return (h.y < t.y) || (h.y == t.y && h.x <= t.x);
}

/* ------------------------------------------------------------------ CPU reference (host only) */

static inline void rgr_keccakf_ref(uint64_t st[25])
{
	static const int rotc[24] = { 1, 3, 6, 10, 15, 21, 28, 36, 45, 55, 2, 14, 27, 41, 56, 8, 25, 43, 62, 18, 39, 61, 20, 44 };
	static const int piln[24] = { 10, 7, 11, 17, 18, 3, 5, 16, 8, 21, 24, 4, 15, 23, 19, 13, 12, 2, 20, 14, 22, 9, 6, 1 };
	uint64_t bc[5], t;
	for (int r = 0; r < 24; r++) {
		for (int i = 0; i < 5; i++)
			bc[i] = st[i] ^ st[i + 5] ^ st[i + 10] ^ st[i + 15] ^ st[i + 20];
		for (int i = 0; i < 5; i++) {
			t = bc[(i + 4) % 5] ^ rgr_rotl64(bc[(i + 1) % 5], 1);
			for (int j = 0; j < 25; j += 5) st[j + i] ^= t;
		}
		t = st[1];
		for (int i = 0; i < 24; i++) {
			const int j = piln[i];
			bc[0] = st[j];
			st[j] = rgr_rotl64(t, rotc[i]);
			t = bc[0];
		}
		for (int j = 0; j < 25; j += 5) {
			for (int i = 0; i < 5; i++) bc[i] = st[j + i];
			for (int i = 0; i < 5; i++) st[j + i] ^= (~bc[(i + 1) % 5]) & bc[(i + 2) % 5];
		}
		st[0] ^= h_rgr_rc[r];
	}
}

/* standard SHA3-256 of any length (little-endian host) */
static inline void sha3_256_ref(uint8_t out[32], const void *data, size_t len)
{
	const uint8_t *in = (const uint8_t *)data;
	uint64_t st[25];
	uint8_t block[RGR_RATE];
	memset(st, 0, sizeof(st));
	while (len >= RGR_RATE) {
		for (int i = 0; i < RGR_RATE / 8; i++) {
			uint64_t lane;
			memcpy(&lane, in + 8 * i, 8);
			st[i] ^= lane;
		}
		rgr_keccakf_ref(st);
		in += RGR_RATE;
		len -= RGR_RATE;
	}
	memset(block, 0, sizeof(block));
	if (len) memcpy(block, in, len);
	block[len] ^= (uint8_t)RGR_PAD;
	block[RGR_RATE - 1] ^= 0x80;
	for (int i = 0; i < RGR_RATE / 8; i++) {
		uint64_t lane;
		memcpy(&lane, block + 8 * i, 8);
		st[i] ^= lane;
	}
	rgr_keccakf_ref(st);
	memcpy(out, st, 32);
}

/* RIGOR PoW digest of an 80-byte header (32 bytes, read as a little-endian uint256) */
static inline void rgr_pow_ref(uint8_t out[32], const uint8_t header80[80])
{
	uint8_t pre[88];
	memcpy(pre, RGR_DOMAIN, 8);
	memcpy(pre + 8, header80, 80);
	sha3_256_ref(out, pre, 88);
}

/* digest (little-endian uint256 bytes) <= target (8 little-endian 32-bit words) */
static inline bool rgr_digest_le_target(const uint8_t d[32], const uint32_t t[8])
{
	for (int w = 7; w >= 0; w--) {
		const uint32_t v = (uint32_t)d[4 * w] | ((uint32_t)d[4 * w + 1] << 8) | ((uint32_t)d[4 * w + 2] << 16) | ((uint32_t)d[4 * w + 3] << 24);
		if (v < t[w]) return true;
		if (v > t[w]) return false;
	}
	return true;
}
