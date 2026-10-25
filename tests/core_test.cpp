// CPU emulation of the GPU path (rgr_core.h) against the reference SHA3-256 and known vectors.
#include "rgr_core.h"
#include <cstdio>
#include <cstdlib>
#include <random>
static void hex(const uint8_t *p, int n, char *o) { for (int i = 0; i < n; i++) sprintf(o + 2 * i, "%02x", p[i]); }
int main()
{
	int fails = 0; char buf[200]; uint8_t d[32];
	sha3_256_ref(d, "", 0); hex(d, 32, buf);
	fails += strcmp(buf, "a7ffc6f8bf1ed76651c14756a061d662f580ff4de43b49fa82d80a4b80f8434a") != 0;
	sha3_256_ref(d, "abc", 3); hex(d, 32, buf);
	fails += strcmp(buf, "3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532") != 0;
	uint8_t h[80] = {0}; rgr_pow_ref(d, h); hex(d, 32, buf);
	fails += strcmp(buf, "dd057fc1df8f5b9466e834cc9d7b819e2d511c75b43675b577f6d9f5d6de04fc") != 0;
	printf("vectors: %s\n", fails ? "FAIL" : "OK");
	std::mt19937_64 rng(12345); long checks = 0, bad = 0;
	for (int t = 0; t < 200; t++) {
		for (int i = 0; i < 80; i++) h[i] = (uint8_t)rng();
		uint32_t tg[8] = {0}; rgr_job job; rgr_prepare_job(&job, h, tg);
		for (int k = 0; k < 50; k++) {
			uint32_t n = (k < 5) ? (uint32_t[]){0u, 1u, 0x80000000u, 0xffffffffu, 0x7fffffffu}[k] : (uint32_t)rng();
			uint2 o[4]; rgr_hash_nonce<2, true, 0>(job, n, o);
			uint2 o3[4]; rgr_hash_nonce<1, false, 0>(job, n, o3);
			uint8_t hh[80]; memcpy(hh, h, 80); memcpy(hh + 76, &n, 4);
			rgr_pow_ref(d, hh);
			uint64_t ref[4]; memcpy(ref, d, 32);
			for (int l = 0; l < 4; l++) if (s3_u64(o[l]) != ref[l]) bad++;
			if (s3_u64(o3[3]) != ref[3]) bad++;
			checks++;
		}
	}
	printf("emulation: %ld nonces, %ld mismatches\n", checks, bad);
	return (fails || bad) ? 1 : 0;
}
