"""Compile-checks the RgrMiner kernels with NVRTC (sm_61) and runs ptxas -v (cloud, no GPU needed).
   usage: python3 nvrtc_check.py [-DRGR_UNROLL=2 ...]"""
import ctypes, glob, os, subprocess, sys, tempfile
here = os.path.dirname(os.path.abspath(__file__))
gpu = os.path.join(here, '..', 'gpu')
# pip install nvidia-cuda-nvrtc-cu12 nvidia-cuda-nvcc-cu12 ; CUDA_PIP_ROOT = the site-packages/nvidia folder
root = os.environ.get('CUDA_PIP_ROOT') or (glob.glob('/usr/local/lib/python3*/*-packages/nvidia') + glob.glob(os.path.expanduser('~/.local/lib/python3*/site-packages/nvidia')))[0]
lib = ctypes.CDLL(glob.glob(root + '/cuda_nvrtc/lib/libnvrtc.so.12')[0])
ptxas = glob.glob(root + '/cuda_nvcc/bin/ptxas')[0]
src = open(os.path.join(gpu, 'rgr_gpu.cu')).read()
# keep only the device part: core header + kernels (drop the host API and runtime include)
core = open(os.path.join(gpu, 'rgr_core.h')).read().replace('#pragma once', '')
start = src.index('#ifndef RGR_UNROLL'); end = src.index('RG_API const char *rg_error_string')
body = src[start:end].replace('#include "rgr_gpu.h"', '')
body = body[:body.index('struct rg_ctx')] + body[body.index('__global__'):]
prog_src = ('typedef unsigned int uint32_t; typedef unsigned long long uint64_t; typedef unsigned char uint8_t;\n'
            '#define RG_MAX_FOUND 16\n#define memset(a,b,c) __builtin_memset(a,b,c)\n#define memcpy(a,b,c) __builtin_memcpy(a,b,c)\n')
core = core.replace('#include <stdint.h>', '').replace('#include <string.h>', '')
# host-only reference code is not needed on the device
core = core[:core.index('/* ------------------------------------------------------------------ CPU reference')]
core = core[:core.index('static inline uint64_t rgr_rotl64')] + core[core.index('/* ------------------------------------------------------------------ per-nonce'):]
prog_src += core + body
prog_src = prog_src.replace('S3_HD uint2 s3_rc(const int i)\n{\n#if defined(__CUDA_ARCH__)\n\treturn c_rgr_rc[i];\n#else\n\treturn s3_u2(h_rgr_rc[i]);\n#endif', 'S3_HD uint2 s3_rc(const int i)\n{\n\treturn c_rgr_rc[i];')
prog = ctypes.c_void_p()
r = lib.nvrtcCreateProgram(ctypes.byref(prog), prog_src.encode(), b'rgr.cu', 0, None, None)
opts = [b'--gpu-architecture=compute_61', b'-std=c++17', b'-D__CUDACC__'] + [a.encode() for a in sys.argv[1:]]
arr = (ctypes.c_char_p * len(opts))(*opts)
r = lib.nvrtcCompileProgram(prog, len(opts), arr)
n = ctypes.c_size_t(); lib.nvrtcGetProgramLogSize(prog, ctypes.byref(n))
log = ctypes.create_string_buffer(n.value); lib.nvrtcGetProgramLog(prog, log)
if log.value.strip(): print(log.value.decode())
if r != 0:
    open('/tmp/rgr_nvrtc_src.cu', 'w').write(prog_src); print('NVRTC FAILED, source in /tmp/rgr_nvrtc_src.cu'); sys.exit(1)
lib.nvrtcGetPTXSize(prog, ctypes.byref(n)); ptx = ctypes.create_string_buffer(n.value); lib.nvrtcGetPTX(prog, ptx)
with tempfile.NamedTemporaryFile('wb', suffix='.ptx', delete=False) as f: f.write(ptx.value)
out = subprocess.run([ptxas, '-arch=sm_61', '-v', '-O3', f.name, '-o', '/dev/null'], capture_output=True, text=True)
print(out.stdout + out.stderr)
sys.exit(out.returncode)
