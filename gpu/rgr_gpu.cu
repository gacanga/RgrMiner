/**
 * RgrMiner - CUDA engine of the RIGOR proof of work (rgrpow01 / OpuHash), built as rgpu.dll
 *   SHA3-256("RGRPOW01" || header80) = one Keccak-f[1600] per nonce (see rgr_core.h).
 * Target: NVIDIA P104-100 (Pascal, sm_61).
 * Compile-time tuning: RGR_UNROLL (rounds per loop iteration, default 2), RGR_ROT (0 = funnel
 * shifts, 1 = shift + add), RGR_MINB (minimum blocks per SM for __launch_bounds__, default 2).
 */
#define RGPU_BUILD
#include "rgr_gpu.h"
#include "rgr_core.h"
#include <cuda_runtime.h>
#include <stdio.h>
#include <string.h>

#ifndef RGR_UNROLL
#define RGR_UNROLL 2
#endif
#ifndef RGR_ROT
#define RGR_ROT 0
#endif
#ifndef RGR_MAXTPB
#define RGR_MAXTPB 512
#endif
#ifndef RGR_MINB
#define RGR_MINB 1
#endif

struct rg_ctx {
	int dev;
	int tpb;
	uint32_t *d_found;      /* [0] = count, [1..RG_MAX_FOUND] = nonces */
	uint8_t *d_digest;      /* test buffer */
	uint32_t d_digest_cap;
	cudaEvent_t ev0, ev1;
};

__global__ void __launch_bounds__(RGR_MAXTPB, RGR_MINB)
rgr_scan_kernel(const rgr_job job, const uint32_t start, uint32_t *found)
{
	const uint32_t nonce = start + blockIdx.x * blockDim.x + threadIdx.x;
	uint2 out[4];
	rgr_hash_nonce<RGR_UNROLL, false, RGR_ROT>(job, nonce, out);
	if (rgr_hi_le(out[3], job.target)) {
		const uint32_t i = atomicAdd(&found[0], 1u);
		if (i < RG_MAX_FOUND) found[1 + i] = nonce;
	}
}

__global__ void rgr_digest_kernel(const rgr_job job, const uint32_t start, const uint32_t count, uint8_t *outp)
{
	const uint32_t i = blockIdx.x * blockDim.x + threadIdx.x;
	if (i >= count) return;
	uint2 out[4];
	rgr_hash_nonce<RGR_UNROLL, true, RGR_ROT>(job, start + i, out);
	for (int l = 0; l < 4; l++) {
		const uint64_t v = s3_u64(out[l]);
		for (int k = 0; k < 8; k++) outp[32 * (size_t)i + 8 * l + k] = (uint8_t)(v >> (8 * k));
	}
}

RG_API const char *rg_error_string(int code) { return cudaGetErrorString((cudaError_t)code); }

RG_API int rg_device_count(void)
{
	int n = 0;
	cudaError_t e = cudaGetDeviceCount(&n);
	return e == cudaSuccess ? n : -(int)e;
}

RG_API int rg_device_name(int dev, char *buf, int len)
{
	cudaDeviceProp p;
	cudaError_t e = cudaGetDeviceProperties(&p, dev);
	if (e != cudaSuccess) { snprintf(buf, len, "?"); return (int)e; }
	snprintf(buf, len, "%s (bus %02x, %d SM, %d MHz)", p.name, p.pciBusID, p.multiProcessorCount, p.clockRate / 1000);
	return 0;
}

RG_API void *rg_create(int dev, int tpb, char *err, int errlen)
{
	cudaError_t e;
	if (tpb <= 0) tpb = 256;
	if (tpb > RGR_MAXTPB || (tpb & 31)) { snprintf(err, errlen, "invalid tpb %d", tpb); return NULL; }
	if ((e = cudaSetDevice(dev)) != cudaSuccess) { snprintf(err, errlen, "cudaSetDevice: %s", cudaGetErrorString(e)); return NULL; }
	cudaSetDeviceFlags(cudaDeviceScheduleBlockingSync);
	rg_ctx *c = new rg_ctx;
	memset(c, 0, sizeof(*c));
	c->dev = dev;
	c->tpb = tpb;
	if ((e = cudaMalloc(&c->d_found, (1 + RG_MAX_FOUND) * sizeof(uint32_t))) != cudaSuccess ||
	    (e = cudaEventCreateWithFlags(&c->ev0, cudaEventBlockingSync)) != cudaSuccess ||
	    (e = cudaEventCreateWithFlags(&c->ev1, cudaEventBlockingSync)) != cudaSuccess) {
		snprintf(err, errlen, "init: %s", cudaGetErrorString(e));
		delete c;
		return NULL;
	}
	return c;
}

RG_API void rg_destroy(void *p)
{
	rg_ctx *c = (rg_ctx *)p;
	if (!c) return;
	cudaSetDevice(c->dev);
	if (c->d_found) cudaFree(c->d_found);
	if (c->d_digest) cudaFree(c->d_digest);
	cudaEventDestroy(c->ev0);
	cudaEventDestroy(c->ev1);
	delete c;
}

RG_API int rg_scan(void *p, const uint8_t header80[80], const uint32_t target[8],
		uint32_t start, uint32_t count, uint32_t *found, int max_found, int *nfound, float *ms_gpu)
{
	rg_ctx *c = (rg_ctx *)p;
	cudaError_t e;
	rgr_job job;
	uint32_t h_found[1 + RG_MAX_FOUND];
	*nfound = 0;
	if (count == 0 || count % (uint32_t)c->tpb) return (int)cudaErrorInvalidValue;
	cudaSetDevice(c->dev);
	rgr_prepare_job(&job, header80, target);
	if ((e = cudaMemsetAsync(c->d_found, 0, sizeof(uint32_t))) != cudaSuccess) return (int)e;
	cudaEventRecord(c->ev0);
	rgr_scan_kernel<<<count / c->tpb, c->tpb>>>(job, start, c->d_found);
	cudaEventRecord(c->ev1);
	if ((e = cudaGetLastError()) != cudaSuccess) return (int)e;
	if ((e = cudaEventSynchronize(c->ev1)) != cudaSuccess) return (int)e;
	if ((e = cudaMemcpy(h_found, c->d_found, sizeof(h_found), cudaMemcpyDeviceToHost)) != cudaSuccess) return (int)e;
	if (ms_gpu) cudaEventElapsedTime(ms_gpu, c->ev0, c->ev1);
	int n = (int)(h_found[0] < RG_MAX_FOUND ? h_found[0] : RG_MAX_FOUND);
	if (n > max_found) n = max_found;
	for (int i = 0; i < n; i++) found[i] = h_found[1 + i];
	*nfound = n;
	return 0;
}

RG_API int rg_digest(void *p, const uint8_t header80[80], uint32_t start, uint32_t count, uint8_t *out)
{
	rg_ctx *c = (rg_ctx *)p;
	cudaError_t e;
	rgr_job job;
	const uint32_t zero[8] = { 0 };
	cudaSetDevice(c->dev);
	if (count > c->d_digest_cap) {
		if (c->d_digest) cudaFree(c->d_digest);
		c->d_digest = NULL;
		c->d_digest_cap = 0;
		if ((e = cudaMalloc(&c->d_digest, (size_t)count * 32)) != cudaSuccess) return (int)e;
		c->d_digest_cap = count;
	}
	rgr_prepare_job(&job, header80, zero);
	rgr_digest_kernel<<<(count + 127) / 128, 128>>>(job, start, count, c->d_digest);
	if ((e = cudaGetLastError()) != cudaSuccess) return (int)e;
	if ((e = cudaMemcpy(out, c->d_digest, (size_t)count * 32, cudaMemcpyDeviceToHost)) != cudaSuccess) return (int)e;
	return 0;
}
