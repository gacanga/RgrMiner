// CPU stand-in for rgpu.dll (cloud tests): same API, same code path (rgr_core.h) run on the CPU.
#define RGPU_BUILD
#include "../gpu/rgr_gpu.h"
#include "../gpu/rgr_core.h"
#include <cstdio>
#include <thread>
#include <vector>
#include <atomic>
struct stub_ctx { int dev; int tpb; };
RG_API int rg_device_count(void) { return 2; }
RG_API int rg_device_name(int dev, char *buf, int len) { snprintf(buf, len, "CPU stub %d", dev); return 0; }
RG_API void *rg_create(int dev, int tpb, char *, int) { return new stub_ctx{ dev, tpb ? tpb : 256 }; }
RG_API void rg_destroy(void *c) { delete (stub_ctx *)c; }
RG_API const char *rg_error_string(int) { return "stub error"; }
RG_API int rg_scan(void *, const uint8_t header80[80], const uint32_t target[8], uint32_t start, uint32_t count,
		uint32_t *found, int max_found, int *nfound, float *ms)
{
	rgr_job job; rgr_prepare_job(&job, header80, target);
	std::atomic<int> n{ 0 }; uint32_t f[RG_MAX_FOUND];
	const int T = 4; std::vector<std::thread> th;
	for (int k = 0; k < T; k++) th.emplace_back([&, k] {
		for (uint32_t i = k; i < count; i += T) {
			uint2 o[4]; rgr_hash_nonce<1, false, 0>(job, start + i, o);
			if (rgr_hi_le(o[3], job.target)) { int j = n++; if (j < RG_MAX_FOUND) f[j] = start + i; }
		}
	});
	for (auto &t : th) t.join();
	int m = n < RG_MAX_FOUND ? n.load() : RG_MAX_FOUND; if (m > max_found) m = max_found;
	for (int i = 0; i < m; i++) found[i] = f[i];
	*nfound = m; if (ms) *ms = 0; return 0;
}
RG_API int rg_digest(void *, const uint8_t header80[80], uint32_t start, uint32_t count, uint8_t *out)
{
	const uint32_t z[8] = { 0 }; rgr_job job; rgr_prepare_job(&job, header80, z);
	for (uint32_t i = 0; i < count; i++) {
		uint2 o[4]; rgr_hash_nonce<2, true, 0>(job, start + i, o);
		for (int l = 0; l < 4; l++) { uint64_t v = s3_u64(o[l]); for (int k = 0; k < 8; k++) out[32 * (size_t)i + 8 * l + k] = (uint8_t)(v >> (8 * k)); }
	}
	return 0;
}
