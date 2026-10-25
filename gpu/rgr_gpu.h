/**
 * RgrMiner - rgpu.dll API (CUDA engine of the RIGOR rgrpow01 / OpuHash proof of work)
 */
#pragma once
#include <stdint.h>

#if defined(_WIN32)
#  if defined(RGPU_BUILD)
#    define RG_API extern "C" __declspec(dllexport)
#  else
#    define RG_API extern "C" __declspec(dllimport)
#  endif
#else
#  define RG_API extern "C"
#endif

#define RG_MAX_FOUND 16

/* number of CUDA devices (CUDA_DEVICE_ORDER is honoured), < 0 on error */
RG_API int rg_device_count(void);
/* name and PCI bus of a device into buf */
RG_API int rg_device_name(int dev, char *buf, int len);
/* opens a device; tpb = threads per block (0 = default 256); returns NULL and fills err on error */
RG_API void *rg_create(int dev, int tpb, char *err, int errlen);
RG_API void rg_destroy(void *ctx);
/*
 * Scans nonces [start, start + count) of header80 (its nonce bytes are ignored) against target
 * (8 little-endian 32-bit words).  count must be a multiple of tpb.  Writes up to max_found
 * candidate nonces (64 high bits of the digest <= 64 high bits of the target; the caller checks
 * all 256 bits) and their number into *nfound.  Blocking (the CPU sleeps while the GPU works).
 * ms_gpu (optional) = GPU time.  Returns 0, or a CUDA error code.
 */
RG_API int rg_scan(void *ctx, const uint8_t header80[80], const uint32_t target[8],
		uint32_t start, uint32_t count, uint32_t *found, int max_found, int *nfound, float *ms_gpu);
/* test: complete 32-byte digests of nonces [start, start + count) into out (count * 32 bytes) */
RG_API int rg_digest(void *ctx, const uint8_t header80[80], uint32_t start, uint32_t count, uint8_t *out);
/* last CUDA error text */
RG_API const char *rg_error_string(int code);
