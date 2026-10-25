/**
 * RgrMiner - GPU solo miner for RIGOR (ORU), proof of work rgrpow01 / OpuHash:
 *     SHA3-256("RGRPOW01" || header80)
 * Works against a LOCAL rigord (getblocktemplate / submitblock on 127.0.0.1:8761).  No dev fee.
 *
 * The node builds the coinbase (rigor_coinbase_immutable): only nNonce and nTime can change.
 * Each GPU takes its own nTime values (template time + slot counter, one slot = 2^32 nonces),
 * so no two cards ever hash the same header.
 *
 * Pool mode (Gozero, pool-url = https://rgr.pool.gozero.trade/rpc): the same getblocktemplate /
 * submitblock exchange over HTTPS with the pool fields gozero_pool / gozero_share_target / workid;
 * the coinbase pays the pool, shares (hash <= share target) are submitted as full blocks.
 *
 * Modes:  (default) mining   --test   --bench <s>   --propose   --check-chain <n>
 * License: GPL-3.0 (see LICENSE).
 */
#include "util.h"
#include "rpc.h"
#include "../gpu/rgr_core.h"
#include "../gpu/rgr_gpu.h"

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <thread>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <array>
#include <deque>
#include <cmath>
#include <cctype>

#define RGR_VERSION "0.2.1"

/* ------------------------------------------------------------------ configuration */

struct Config {
	std::string rpc_host = "127.0.0.1";
	int rpc_port = 8761;
	std::string auth_file, rpc_user, rpc_pass;
	std::string address;
	std::string gpus = "all";
	int tpb = 512;
	int batch_log2 = 27;
	int poll_ms = 500;
	int stats_s = 60;
	int selfcheck_min = 10;
	int max_ahead_s = 3600;          /* never put nTime more than this ahead of the clock */
	std::string log;
	std::string mode = "mine";
	int bench_s = 30;
	int check_n = 5;
	/* pool mode */
	std::string pool_url;            /* https://rgr.pool.gozero.trade/rpc ; empty = solo on the local node */
	std::string pool_user;           /* <rg1q address>.<worker> */
	std::string pool_pass = "x";
	double share_work = 1e10;        /* requested hashes per share (gozero_share_work) */
	std::string ca_file = "ca-bundle.crt";
	int pool_poll_ms = 3000;         /* pool template poll period */
	int tpl_max_age_s = 60;          /* switch to a fresh pool template after this age (pool: expires 120 s) */
	int tip_watch_ms = 250;          /* local node tip check (new block -> new pool work at once), 0 = off */
	int share_threads = 4;           /* parallel share submissions (each one is a full HTTPS request) */
	int run_s = 0;                   /* stop after this many seconds (tests), 0 = run forever */
};

static std::string trim(const std::string &s)
{
	size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
	return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

static bool set_opt(Config &c, std::string k, const std::string &v)
{
	if (k.rfind("--", 0) == 0) k = k.substr(2);
	if (k == "rpc-host") c.rpc_host = v;
	else if (k == "rpc-port") c.rpc_port = atoi(v.c_str());
	else if (k == "auth-file") c.auth_file = v;
	else if (k == "rpc-user") c.rpc_user = v;
	else if (k == "rpc-pass") c.rpc_pass = v;
	else if (k == "address") c.address = v;
	else if (k == "gpus") c.gpus = v;
	else if (k == "tpb") c.tpb = atoi(v.c_str());
	else if (k == "batch") c.batch_log2 = atoi(v.c_str());
	else if (k == "poll-ms") c.poll_ms = atoi(v.c_str());
	else if (k == "stats") c.stats_s = atoi(v.c_str());
	else if (k == "selfcheck") c.selfcheck_min = atoi(v.c_str());
	else if (k == "max-ahead") c.max_ahead_s = atoi(v.c_str());
	else if (k == "log") c.log = v;
	else if (k == "pool-url") c.pool_url = v;
	else if (k == "pool-user") c.pool_user = v;
	else if (k == "pool-pass") c.pool_pass = v;
	else if (k == "share-work") c.share_work = atof(v.c_str());
	else if (k == "ca-file") c.ca_file = v;
	else if (k == "pool-poll-ms") c.pool_poll_ms = atoi(v.c_str());
	else if (k == "template-max-age") c.tpl_max_age_s = atoi(v.c_str());
	else if (k == "tip-watch-ms") c.tip_watch_ms = atoi(v.c_str());
	else if (k == "share-threads") c.share_threads = atoi(v.c_str());
	else if (k == "run-seconds") c.run_s = atoi(v.c_str());
	else return false;
	return true;
}

static bool load_kv_file(const std::string &path, std::vector<std::pair<std::string, std::string>> &kv)
{
	std::ifstream f(path);
	if (!f) return false;
	std::string line;
	while (std::getline(f, line)) {
		line = trim(line);
		if (line.empty() || line[0] == '#' || line[0] == ';') continue;
		size_t e = line.find('=');
		if (e == std::string::npos) continue;
		kv.emplace_back(trim(line.substr(0, e)), trim(line.substr(e + 1)));
	}
	return true;
}

static void usage()
{
	printf(
		"RgrMiner %s - GPU miner for RIGOR (ORU, rgrpow01 / OpuHash): solo on a local node or Gozero pool, no dev fee\n"
		"usage: rgrminer [--config file] [--args-file file] [options] [mode]\n"
		"  --address rg1q...      payout address (ORU, witness v0) sent as coinbase_address\n"
		"  --rpc-host / --rpc-port local rigord RPC (127.0.0.1 / 8761)\n"
		"  --auth-file path       rpc-auth.conf written by rigord (rpcuser / rpcpassword)\n"
		"  --rpc-user / --rpc-pass explicit credentials instead of --auth-file\n"
		"  --gpus all|0,1,2       CUDA devices (set CUDA_DEVICE_ORDER=PCI_BUS_ID)\n"
		"  --tpb N                threads per block (512)    --batch L  2^L nonces per launch (27)\n"
		"  --poll-ms N            getblocktemplate poll period (500)\n"
		"  --stats S              hashrate report period (60)  --selfcheck M  GPU self-check period, 0 = off (10)\n"
		"  --log file             append the log to file\n"
		"pool mode (Gozero):\n"
		"  --pool-url https://rgr.pool.gozero.trade/rpc   --pool-user <rg1q address>.<worker>   [--pool-pass x]\n"
		"  --share-work N         requested hashes per share (1e10)   --ca-file ca-bundle.crt (TLS verification)\n"
		"  --pool-poll-ms N (3000)  --template-max-age S (60)  --tip-watch-ms N (250, needs the local node RPC)\n"
		"  --share-threads N      parallel share submissions (4)\n"
		"  --run-seconds S        stop after S seconds (tests)\n"
		"modes: --test | --bench S | --propose | --check-chain N | (default) mining\n", RGR_VERSION);
}

/* ------------------------------------------------------------------ globals */

static Config g_cfg;
static Rpc g_rpc;                       /* local node (solo; tip watch in pool mode) */
static Rpc g_pool;                      /* Gozero pool (pool mode) */
static bool g_pool_mode = false;
static std::atomic<bool> g_stop{ false };

/* ------------------------------------------------------------------ block template */

struct Template {
	uint64_t id = 0;
	std::string prevhash;               /* display hex */
	std::string longpollid;
	int64_t height = 0;
	uint8_t header[80];                 /* nTime = template time, nonce 0 */
	uint32_t target[8];                 /* block target, little-endian words */
	uint32_t gpu_target[8];             /* what the GPUs look for: share target (pool) or block target */
	std::string workid;                 /* pool: sent back with every share */
	std::string pool_address;           /* pool: coinbase payee */
	double expires_s = 120;
	uint32_t bits = 0;
	int64_t base_time = 0, maxtime = 0;
	int64_t coinbasevalue = 0;
	size_t ntx = 0;
	std::string txs_hex;                /* compactsize(n) + coinbase + transactions */
	std::atomic<uint32_t> slot{ 0 };    /* next nTime slot */
	std::atomic<bool> solved{ false };  /* a block was accepted on this template */
	double fetched_at = 0;
};

static std::mutex g_tpl_m;
static std::shared_ptr<Template> g_tpl;
static std::atomic<uint64_t> g_tpl_id{ 0 };
static std::atomic<double> g_last_ok{ 0 };
static std::mutex g_poll_m;
static std::condition_variable g_poll_cv;
static std::atomic<bool> g_poll_now{ false };

static std::shared_ptr<Template> current_tpl()
{
	std::lock_guard<std::mutex> g(g_tpl_m);
	return g_tpl;
}

static void put_le32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t get_le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static void compact_size(std::vector<uint8_t> &o, uint64_t n)
{
	if (n < 0xfd) o.push_back((uint8_t)n);
	else if (n <= 0xffff) { o.push_back(0xfd); o.push_back((uint8_t)n); o.push_back((uint8_t)(n >> 8)); }
	else { o.push_back(0xfe); for (int i = 0; i < 4; i++) o.push_back((uint8_t)(n >> (8 * i))); }
}

/* nBits -> 8 little-endian words (Bitcoin SetCompact); false if negative / zero / overflow */
static bool bits_to_target(uint32_t bits, uint32_t t[8])
{
	memset(t, 0, 32);
	const int e = (int)(bits >> 24);
	const uint32_t m = bits & 0x007fffff;
	if ((bits & 0x00800000) || m == 0) return false;
	uint8_t b[32];
	memset(b, 0, 32);
	if (e <= 3) {
		const uint32_t v = m >> (8 * (3 - e));
		if (v == 0) return false;
		b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); b[2] = (uint8_t)(v >> 16);
	} else {
		for (int i = 0; i < 3; i++) {
			const int pos = e - 3 + i;
			const uint8_t byte = (uint8_t)(m >> (8 * i));
			if (pos >= 32) { if (byte) return false; continue; }
			b[pos] = byte;
		}
	}
	for (int w = 0; w < 8; w++) t[w] = get_le32(b + 4 * w);
	return true;
}

static std::string target_hex(const uint32_t t[8])
{
	uint8_t b[32];
	for (int w = 0; w < 8; w++) put_le32(b + 4 * w, t[w]);
	return to_hex_rev(b, 32);
}

static double target_difficulty(const uint32_t t[8])
{
	/* Bitcoin-style difficulty: 0xffff * 2^208 / target */
	double v = 0;
	for (int w = 7; w >= 0; w--) v = v * 4294967296.0 + t[w];
	return v > 0 ? (65535.0 * pow(2.0, 208)) / v : 0;
}

/* strips the witness of a serialized transaction (for its txid); false on parse error */
static bool strip_witness(const std::vector<uint8_t> &tx, std::vector<uint8_t> &out)
{
	size_t p = 0;
	auto rd_cs = [&](uint64_t &v) -> bool {
		if (p >= tx.size()) return false;
		uint8_t c = tx[p++];
		if (c < 0xfd) { v = c; return true; }
		int n = c == 0xfd ? 2 : c == 0xfe ? 4 : 8;
		if (p + n > tx.size()) return false;
		v = 0; for (int i = 0; i < n; i++) v |= (uint64_t)tx[p + i] << (8 * i);
		p += n; return true;
	};
	if (tx.size() < 10) return false;
	bool wit = tx[4] == 0x00 && tx[5] == 0x01;
	if (!wit) { out = tx; return true; }
	out.assign(tx.begin(), tx.begin() + 4);
	p = 6;
	size_t start = p;
	uint64_t nin, nout, l;
	if (!rd_cs(nin)) return false;
	for (uint64_t i = 0; i < nin; i++) { p += 36; if (!rd_cs(l)) return false; p += l + 4; if (p > tx.size()) return false; }
	if (!rd_cs(nout)) return false;
	for (uint64_t i = 0; i < nout; i++) { p += 8; if (!rd_cs(l)) return false; p += l; if (p > tx.size()) return false; }
	out.insert(out.end(), tx.begin() + start, tx.begin() + p);
	if (tx.size() < 4) return false;
	out.insert(out.end(), tx.end() - 4, tx.end());
	return true;
}

static void merkle_root(std::vector<std::array<uint8_t, 32>> v, uint8_t out[32])
{
	if (v.empty()) { memset(out, 0, 32); return; }
	while (v.size() > 1) {
		if (v.size() & 1) v.push_back(v.back());
		std::vector<std::array<uint8_t, 32>> n(v.size() / 2);
		for (size_t i = 0; i < n.size(); i++) {
			uint8_t cat[64]; memcpy(cat, v[2 * i].data(), 32); memcpy(cat + 32, v[2 * i + 1].data(), 32);
			sha256d(n[i].data(), cat, 64);
		}
		v.swap(n);
	}
	memcpy(out, v[0].data(), 32);
}

static bool hex32_rev(const std::string &h, uint8_t out[32])
{
	std::vector<uint8_t> b;
	if (!from_hex(h, b) || b.size() != 32) return false;
	for (int i = 0; i < 32; i++) out[i] = b[31 - i];
	return true;
}

static std::string gbt_params(const char *mode = nullptr, const std::string &data = "")
{
	if (mode) return std::string("[{\"mode\":\"") + mode + "\",\"data\":\"" + data + "\",\"rules\":[\"segwit\"]}]";
	if (g_pool_mode) {
		char w[64];
		snprintf(w, sizeof(w), "%.0f", g_cfg.share_work);
		return std::string("[{\"gozero_pool\":1,\"gozero_share_work\":") + w +
			",\"capabilities\":[\"coinbasevalue\",\"longpoll\",\"workid\"],\"rules\":[\"segwit\"]}]";
	}
	return "[{\"rules\":[\"segwit\"],\"capabilities\":[\"coinbasevalue\",\"longpoll\"],\"coinbase_address\":" + json_escape(g_cfg.address) + "}]";
}

/* parses / checks a getblocktemplate result into t; false + err on any inconsistency */
static bool build_template(const JVal &r, Template &t, std::string &err)
{
	if (r.t != JVal::OBJ) { err = "template is not an object"; return false; }
	const JVal *imm = r.get("rigor_coinbase_immutable");
	if (!imm || imm->t != JVal::BOOL || !imm->b) { err = "rigor_coinbase_immutable missing or false"; return false; }
	if (g_pool_mode) {
		if (r.num("gozero_pool") != 1) { err = "not a Gozero pool template (gozero_pool != 1)"; return false; }
		t.pool_address = r.str("gozero_pool_address");
		t.workid = r.str("workid");
		if (t.pool_address.rfind("rg1q", 0) != 0 || r.str("coinbase_address") != t.pool_address) { err = "pool coinbase address missing or inconsistent"; return false; }
		if (t.workid.empty()) { err = "pool template without workid"; return false; }
		t.expires_s = (double)r.num("expires", 120);
	} else if (r.str("coinbase_address") != g_cfg.address) { err = "node did not echo our coinbase_address"; return false; }
	const JVal *mut = r.get("mutable");
	bool time_mut = false;
	if (mut && mut->t == JVal::ARR) for (auto &m : mut->a) if (m.s == "time") time_mut = true;
	if (!time_mut) { err = "template does not allow changing time"; return false; }
	t.prevhash = r.str("previousblockhash");
	t.longpollid = r.str("longpollid");
	t.height = r.num("height");
	t.coinbasevalue = r.num("coinbasevalue");
	uint32_t version = (uint32_t)r.num("version");
	uint8_t prev[32], mroot[32];
	if (!hex32_rev(t.prevhash, prev)) { err = "bad previousblockhash"; return false; }
	std::string bits_s = r.str("bits");
	if (bits_s.size() != 8) { err = "bad bits"; return false; }
	t.bits = (uint32_t)strtoul(bits_s.c_str(), nullptr, 16);
	if (!bits_to_target(t.bits, t.target)) { err = "bad compact target"; return false; }
	if (target_hex(t.target) != r.str("target")) { err = "target does not match bits (" + target_hex(t.target) + " vs " + r.str("target") + ")"; return false; }
	int64_t curtime = r.num("curtime"), mintime = r.num("mintime");
	t.maxtime = r.num("maxtime", curtime + 7000);
	memcpy(t.gpu_target, t.target, sizeof(t.target));
	if (g_pool_mode) {
		std::vector<uint8_t> st;
		if (!from_hex(r.str("gozero_share_target"), st) || st.size() != 32) { err = "bad gozero_share_target"; return false; }
		uint32_t w[8];
		for (int i = 0; i < 8; i++) w[i] = (uint32_t)st[31 - 4 * i] | (uint32_t)st[30 - 4 * i] << 8 | (uint32_t)st[29 - 4 * i] << 16 | (uint32_t)st[28 - 4 * i] << 24;
		/* the share target must be at least as easy as the block target */
		int cmp = 0;
		for (int i = 7; i >= 0 && !cmp; i--) if (w[i] != t.target[i]) cmp = w[i] > t.target[i] ? 1 : -1;
		if (cmp < 0) { err = "share target harder than the block target"; return false; }
		memcpy(t.gpu_target, w, sizeof(w));
	}
	t.base_time = std::max(curtime, mintime);

	/* coinbase: copy the node bytes, check its txid */
	std::vector<uint8_t> cb, cb_nw;
	if (!from_hex(r.str("default_coinbase_tx"), cb) || cb.empty()) { err = "missing default_coinbase_tx"; return false; }
	if (!strip_witness(cb, cb_nw)) { err = "cannot parse default_coinbase_tx"; return false; }
	std::array<uint8_t, 32> leaf;
	sha256d(leaf.data(), cb_nw.data(), cb_nw.size());
	uint8_t cbid[32];
	if (!hex32_rev(r.str("default_coinbase_txid"), cbid) || memcmp(cbid, leaf.data(), 32)) { err = "default_coinbase_txid does not match the coinbase bytes"; return false; }
	std::vector<std::array<uint8_t, 32>> leaves{ leaf };
	const JVal *txs = r.get("transactions");
	if (!txs || txs->t != JVal::ARR) { err = "no transactions array"; return false; }
	std::vector<uint8_t> blk;
	compact_size(blk, 1 + txs->a.size());
	std::string hex = to_hex(blk.data(), blk.size()) + r.str("default_coinbase_tx");
	for (auto &tx : txs->a) {
		std::string d = tx.str("data"), id = tx.str("txid");
		std::array<uint8_t, 32> l;
		if (d.empty() || (d.size() & 1) || !hex32_rev(id, l.data())) { err = "bad transaction entry"; return false; }
		leaves.push_back(l);
		hex += d;
	}
	merkle_root(leaves, mroot);
	uint8_t node_root[32];
	if (!hex32_rev(r.str("merkleroot"), node_root) || memcmp(node_root, mroot, 32)) { err = "merkle root mismatch with the template"; return false; }
	t.ntx = txs->a.size();
	t.txs_hex = hex;

	put_le32(t.header, version);
	memcpy(t.header + 4, prev, 32);
	memcpy(t.header + 36, mroot, 32);
	put_le32(t.header + 68, (uint32_t)t.base_time);
	put_le32(t.header + 72, t.bits);
	put_le32(t.header + 76, 0);
	return true;
}

static bool fetch_template(std::shared_ptr<Template> &out, std::string &err)
{
	JVal r;
	if (!(g_pool_mode ? g_pool : g_rpc).call("getblocktemplate", gbt_params(), r, err, 20)) return false;
	auto t = std::make_shared<Template>();
	if (!build_template(r, *t, err)) return false;
	t->fetched_at = now_s();
	out = t;
	return true;
}

static void template_thread()
{
	std::string last_err;
	double last_err_log = 0;
	while (!g_stop) {
		std::shared_ptr<Template> t;
		std::string err;
		if (fetch_template(t, err)) {
			g_last_ok = now_s();
			if (!last_err.empty()) { LOG("node OK again"); last_err.clear(); }
			auto cur = current_tpl();
			bool change = !cur || cur->prevhash != t->prevhash;
			if (!change && g_pool_mode) change = now_s() - cur->fetched_at > std::min((double)g_cfg.tpl_max_age_s, cur->expires_s * 0.75);
			else if (!change) change = cur->longpollid != t->longpollid;
			if (change) {
				bool new_block = !cur || cur->prevhash != t->prevhash;
				t->id = g_tpl_id + 1;
				{
					std::lock_guard<std::mutex> g(g_tpl_m);
					g_tpl = t;
				}
				g_tpl_id = t->id;
				if (new_block && g_pool_mode)
					LOG("new pool job: height %lld, diff %.1f, share diff %.4f, %zu tx, prev %s",
						(long long)t->height, target_difficulty(t->target), target_difficulty(t->gpu_target), t->ntx, t->prevhash.substr(0, 16).c_str());
				else if (new_block)
					LOG("new block template: height %lld, diff %.1f, %zu tx, reward %.8f ORU, prev %s",
						(long long)t->height, target_difficulty(t->target), t->ntx, t->coinbasevalue / 1e8, t->prevhash.substr(0, 16).c_str());
			}
		} else {
			if (err != last_err || now_s() - last_err_log > 60) { LOG("getblocktemplate (%s) failed: %s", g_pool_mode ? "pool" : "node", err.c_str()); last_err_log = now_s(); }
			last_err = err;
		}
		std::unique_lock<std::mutex> lk(g_poll_m);
		g_poll_cv.wait_for(lk, std::chrono::milliseconds(g_pool_mode ? g_cfg.pool_poll_ms : g_cfg.poll_ms), [] { return g_poll_now.load() || g_stop.load(); });
		g_poll_now = false;
	}
}

static void poll_now()
{
	g_poll_now = true;
	g_poll_cv.notify_all();
}

/* pool mode: the local node sees new blocks within ~1 s; ask the pool for new work at once */
static void tip_watch_thread()
{
	std::string last, err;
	bool warned = false;
	while (!g_stop) {
		std::this_thread::sleep_for(std::chrono::milliseconds(g_cfg.tip_watch_ms));
		JVal r;
		if (!g_rpc.call("getbestblockhash", "[]", r, err, 5)) {
			if (!warned) { LOG("tip watch: local node unavailable (%s), pool polling only", err.c_str()); warned = true; }
			std::this_thread::sleep_for(std::chrono::seconds(10));
			continue;
		}
		if (warned) { LOG("tip watch: local node OK"); warned = false; }
		auto cur = current_tpl();
		if (r.t == JVal::STR && !last.empty() && r.s != last && cur && cur->prevhash != r.s) poll_now();
		if (r.t == JVal::STR) last = r.s;
	}
}

/* ------------------------------------------------------------------ statistics */

struct GpuStat {
	std::atomic<uint64_t> hashes{ 0 };
	std::atomic<uint64_t> wrong{ 0 };
	std::atomic<uint64_t> found{ 0 };
	std::atomic<double> last_ms{ 0 };
	int dev = -1;
	std::string name;
};
static std::vector<std::unique_ptr<GpuStat>> g_stats;
static std::atomic<uint64_t> g_blocks_ok{ 0 }, g_blocks_rej{ 0 };
static std::atomic<uint64_t> g_sh_ok{ 0 }, g_sh_rej{ 0 }, g_sh_stale{ 0 }, g_sh_blocks{ 0 };
static std::atomic<uint64_t> g_sh_sent{ 0 }, g_sh_ms{ 0 }, g_sh_qmax{ 0 };   /* submissions, total ms, longest queue */
static double g_start_s = 0;

/* ------------------------------------------------------------------ submission */

static std::mutex g_submit_m;

static void submit_block(Template &t, const uint8_t header[80], int gpu)
{
	uint8_t digest[32], bh[32];
	rgr_pow_ref(digest, header);
	sha256d(bh, header, 80);
	std::string hex = to_hex(header, 80) + t.txs_hex;
	std::lock_guard<std::mutex> g(g_submit_m);
	if (t.solved || g_tpl_id != t.id) { LOG("GPU %d: block candidate on an outdated template, not submitted", gpu); return; }
	LOG("BLOCK candidate by GPU %d at height %lld: hash %s pow %s", gpu, (long long)t.height, to_hex_rev(bh, 32).c_str(), to_hex_rev(digest, 32).c_str());
	JVal r;
	std::string err;
	bool ok = g_rpc.call("submitblock", "[\"" + hex + "\"]", r, err, 60);
	if (ok && r.is_null()) {
		t.solved = true;
		g_blocks_ok++;
		LOG("BLOCK ACCEPTED by the node: height %lld, %.8f ORU to %s (blocks %llu)", (long long)t.height, t.coinbasevalue / 1e8, g_cfg.address.c_str(), (unsigned long long)g_blocks_ok.load());
	} else {
		g_blocks_rej++;
		LOG("BLOCK REJECTED: %s", ok ? (r.t == JVal::STR ? r.s.c_str() : "unexpected answer") : err.c_str());
	}
	poll_now();
}

/* ------------------------------------------------------------------ pool shares */

struct Share {
	std::shared_ptr<Template> t;
	uint8_t header[80];
	int gpu;
	bool block;
};
static std::mutex g_shq_m;
static std::condition_variable g_shq_cv;
static std::deque<Share> g_shq;

static void queue_share(const std::shared_ptr<Template> &t, const uint8_t header[80], int gpu, bool block)
{
	Share s;
	s.t = t; memcpy(s.header, header, 80); s.gpu = gpu; s.block = block;
	{
		std::lock_guard<std::mutex> g(g_shq_m);
		if (g_shq.size() > 256) g_shq.pop_front();       /* pool unreachable for a long time */
		g_shq.push_back(s);
		uint64_t q = g_shq.size();
		if (q > g_sh_qmax) g_sh_qmax = q;
	}
	g_shq_cv.notify_one();
}

static void share_thread()
{
	while (!g_stop) {
		Share s;
		{
			std::unique_lock<std::mutex> lk(g_shq_m);
			g_shq_cv.wait_for(lk, std::chrono::milliseconds(500), [] { return !g_shq.empty() || g_stop.load(); });
			if (g_shq.empty()) continue;
			s = g_shq.front();
			g_shq.pop_front();
		}
		auto cur = current_tpl();
		if (!s.block && ((cur && cur->prevhash != s.t->prevhash) || now_s() - s.t->fetched_at > s.t->expires_s - 5)) {
			g_sh_stale++;                 /* found on a previous block or an expired job: do not send */
			continue;
		}
		std::string hex = to_hex(s.header, 80) + s.t->txs_hex;
		JVal r;
		std::string err;
		bool ok = false;
		for (int attempt = 0; attempt < 3 && !ok; attempt++) {
			double t0 = now_s();
			ok = g_pool.call("submitblock", "[\"" + hex + "\",{\"workid\":" + json_escape(s.t->workid) + "}]", r, err, 30);
			g_sh_sent++;
			g_sh_ms += (uint64_t)((now_s() - t0) * 1000.0);
			if (!ok && err.rfind("RPC error", 0) == 0) break;      /* answered with an error: no retry */
			if (!ok) std::this_thread::sleep_for(std::chrono::seconds(2));
		}
		if (ok && r.is_null()) {
			uint64_t n = ++g_sh_ok;
			if (s.block) {
				g_sh_blocks++;
				uint8_t bh[32];
				sha256d(bh, s.header, 80);
				LOG("BLOCK found for the pool by GPU %d: height %lld, hash %s (share accepted)", s.gpu, (long long)s.t->height, to_hex_rev(bh, 32).c_str());
			} else {
				LOG("share accepted (GPU %d) [%llu ok / %llu rejected / %llu stale]", s.gpu, (unsigned long long)n,
					(unsigned long long)g_sh_rej.load(), (unsigned long long)g_sh_stale.load());
			}
		} else {
			std::string why = ok ? (r.t == JVal::STR ? r.s : "unexpected answer") : err;
			std::string lw = why;
			for (auto &c : lw) c = (char)tolower((unsigned char)c);
			if (lw.find("stale") != std::string::npos || lw.find("duplicate") != std::string::npos ||
			    lw.find("inconclusive") != std::string::npos || lw.find("expired") != std::string::npos ||
			    lw.find("prevblk") != std::string::npos || lw.find("unknown work") != std::string::npos) {
				g_sh_stale++;
				LOG("share stale (GPU %d): %s", s.gpu, why.c_str());
			} else {
				g_sh_rej++;
				LOG("share REJECTED (GPU %d)%s: %s", s.gpu, s.block ? " [block-level]" : "", why.c_str());
			}
			poll_now();
		}
	}
}

/* ------------------------------------------------------------------ GPU worker */

static bool selfcheck_gpu(void *ctx, int gpu, std::string &why)
{
	/* fixed pseudo-random header, 4096 nonces: GPU digests == CPU reference */
	uint8_t h[80];
	std::mt19937 rng(20261025u);
	for (auto &b : h) b = (uint8_t)rng();
	const uint32_t n = 4096, start = 0xfffff000u;
	std::vector<uint8_t> out(32 * (size_t)n);
	int e = rg_digest(ctx, h, start, n, out.data());
	if (e) { why = std::string("CUDA error ") + rg_error_string(e); return false; }
	for (uint32_t i = 0; i < n; i++) {
		uint8_t hh[80], d[32];
		memcpy(hh, h, 80); put_le32(hh + 76, start + i);
		rgr_pow_ref(d, hh);
		if (memcmp(d, out.data() + 32 * (size_t)i, 32)) { why = "digest mismatch at nonce " + std::to_string(start + i); return false; }
	}
	(void)gpu;
	return true;
}

static void gpu_thread(int idx, int dev)
{
	GpuStat &st = *g_stats[idx];
	char err[256] = { 0 };
	void *ctx = rg_create(dev, g_cfg.tpb, err, sizeof(err));
	if (!ctx) { LOG("GPU %d: init failed: %s", dev, err); return; }
	std::string why;
	if (!selfcheck_gpu(ctx, dev, why)) { LOG("GPU %d SELF-CHECK FAILED: %s", dev, why.c_str()); fflush(stdout); _Exit(6); }
	LOG("GPU %d: %s ready, self-check OK", dev, st.name.c_str());
	const uint32_t batch = 1u << g_cfg.batch_log2;
	double next_check = now_s() + 60.0 * g_cfg.selfcheck_min;
	while (!g_stop) {
		auto t = current_tpl();
		if (!t || now_s() - g_last_ok > 30) { std::this_thread::sleep_for(std::chrono::milliseconds(200)); continue; }
		const uint32_t slot = t->slot.fetch_add(1);
		const int64_t ntime = t->base_time + slot;
		if (ntime > t->maxtime || ntime > unix_now() + g_cfg.max_ahead_s) {
			/* nTime space of this template used up: wait for a refresh */
			t->slot.fetch_sub(1);
			poll_now();
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
			continue;
		}
		uint8_t header[80];
		memcpy(header, t->header, 80);
		put_le32(header + 68, (uint32_t)ntime);
		for (uint64_t start = 0; start < (1ull << 32) && !g_stop; start += batch) {
			if (g_tpl_id != t->id || t->solved) break;
			uint32_t found[RG_MAX_FOUND];
			int nf = 0;
			float ms = 0;
			int e = rg_scan(ctx, header, t->gpu_target, (uint32_t)start, batch, found, RG_MAX_FOUND, &nf, &ms);
			if (e) { LOG("GPU %d CUDA error: %s", dev, rg_error_string(e)); fflush(stdout); _Exit(5); }
			st.hashes += batch;
			st.last_ms = ms;
			for (int i = 0; i < nf; i++) {
				uint8_t hh[80], d[32];
				memcpy(hh, header, 80);
				put_le32(hh + 76, found[i]);
				rgr_pow_ref(d, hh);
				/* the GPU only compared the 64 high bits: recheck them, then all 256 */
				uint2 hi = make_uint2(get_le32(d + 24), get_le32(d + 28));
				if (!rgr_hi_le(hi, make_uint2(t->gpu_target[6], t->gpu_target[7]))) {
					st.wrong++;
					LOG("GPU %d WRONG candidate (nonce %08x): hardware error, check clocks", dev, found[i]);
					continue;
				}
				if (g_pool_mode) {
					if (rgr_digest_le_target(d, t->gpu_target)) {
						const bool block = rgr_digest_le_target(d, t->target);
						if (block) st.found++;
						queue_share(t, hh, dev, block);
					}
				} else if (rgr_digest_le_target(d, t->target)) {
					st.found++;
					submit_block(*t, hh, dev);
				}
			}
		}
		if (g_cfg.selfcheck_min > 0 && now_s() > next_check) {
			if (!selfcheck_gpu(ctx, dev, why)) { LOG("GPU %d SELF-CHECK FAILED: %s", dev, why.c_str()); fflush(stdout); _Exit(6); }
			next_check = now_s() + 60.0 * g_cfg.selfcheck_min;
		}
	}
	rg_destroy(ctx);
}

/* ------------------------------------------------------------------ GPU list */

static std::vector<int> parse_gpus(int ndev)
{
	std::vector<int> v;
	if (g_cfg.gpus == "all") { for (int i = 0; i < ndev; i++) v.push_back(i); return v; }
	std::stringstream ss(g_cfg.gpus);
	std::string x;
	while (std::getline(ss, x, ',')) { x = trim(x); if (!x.empty()) { int d = atoi(x.c_str()); if (d >= 0 && d < ndev) v.push_back(d); else LOG("GPU %d does not exist (%d devices)", d, ndev); } }
	return v;
}

static bool open_gpus(std::vector<int> &devs)
{
	int n = rg_device_count();
	if (n <= 0) { LOG("no CUDA device (%d)", n); return false; }
	devs = parse_gpus(n);
	if (devs.empty()) { LOG("no GPU selected"); return false; }
	for (int d : devs) {
		auto s = std::make_unique<GpuStat>();
		char name[256];
		rg_device_name(d, name, sizeof(name));
		s->dev = d;
		s->name = name;
		g_stats.push_back(std::move(s));
	}
	return true;
}

/* ------------------------------------------------------------------ modes */

static int mode_test()
{
	int fails = 0;
	uint8_t d[32];
	/* reference vectors (FIPS 202 and RGRPOW01_REFERENCE_VECTOR_v1.0) */
	sha3_256_ref(d, "", 0);
	fails += to_hex(d, 32) != "a7ffc6f8bf1ed76651c14756a061d662f580ff4de43b49fa82d80a4b80f8434a";
	sha3_256_ref(d, "abc", 3);
	fails += to_hex(d, 32) != "3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532";
	uint8_t h0[80] = { 0 };
	rgr_pow_ref(d, h0);
	fails += to_hex(d, 32) != "dd057fc1df8f5b9466e834cc9d7b819e2d511c75b43675b577f6d9f5d6de04fc";
	{
		/* vector #002: every field distinct */
		std::vector<uint8_t> h;
		from_hex("00000020" "1817161514131211100f0e0d0c0b0a89f7e6d5c4b3a201000000000000000000"
			 "dd15358bf3bca0e89265959d05bf135984d5d875e9572c8efc27f97a20bd3f9d" "201d766a" "0080001d" "efcdab89", h);
		rgr_pow_ref(d, h.data());
		fails += to_hex(d, 32) != "8b3571829ad202105cc7fc4b1c8beef1f7190e6bfdbf2f0ac54355e7b4f041f0";
	}
	LOG("CPU reference vectors: %s", fails ? "FAILED" : "OK");
	/* CPU emulation of the GPU code path */
	std::mt19937_64 rng(1);
	long bad = 0;
	for (int t = 0; t < 100; t++) {
		uint8_t h[80];
		for (auto &b : h) b = (uint8_t)rng();
		uint32_t tg[8] = { 0 };
		rgr_job job;
		rgr_prepare_job(&job, h, tg);
		for (int k = 0; k < 40; k++) {
			uint32_t n = (uint32_t)rng();
			uint2 o[4];
			rgr_hash_nonce<2, true, 0>(job, n, o);
			put_le32(h + 76, n);
			rgr_pow_ref(d, h);
			uint64_t ref[4];
			memcpy(ref, d, 32);
			for (int l = 0; l < 4; l++) bad += s3_u64(o[l]) != ref[l];
		}
	}
	LOG("CPU emulation of the GPU path: %s (%ld mismatches)", bad ? "FAILED" : "OK", bad);
	fails += bad != 0;
	/* compact target */
	uint32_t tg[8];
	bits_to_target(0x1d008000, tg);
	fails += target_hex(tg) != "0000000080000000000000000000000000000000000000000000000000000000";
	bits_to_target(0x1b058bd0, tg);
	fails += target_hex(tg) != "0000000000058bd0000000000000000000000000000000000000000000000000";
	LOG("compact targets: %s", fails ? "check above" : "OK");

	std::vector<int> devs;
	if (!open_gpus(devs)) return 3;
	for (size_t gi = 0; gi < devs.size(); gi++) {
		int dev = devs[gi];
		char err[256];
		void *ctx = rg_create(dev, g_cfg.tpb, err, sizeof(err));
		if (!ctx) { LOG("GPU %d: init failed: %s", dev, err); fails++; continue; }
		int gf = 0;
		/* 1) digests of 65536 nonces x 3 headers vs CPU */
		for (int hdr = 0; hdr < 3; hdr++) {
			uint8_t h[80];
			for (auto &b : h) b = (uint8_t)rng();
			const uint32_t n = 65536, start = hdr == 2 ? 0xffff0000u : (uint32_t)rng() & 0xfff00000u;
			std::vector<uint8_t> out(32 * (size_t)n);
			int e = rg_digest(ctx, h, start, n, out.data());
			if (e) { LOG("GPU %d rg_digest: %s", dev, rg_error_string(e)); gf++; break; }
			int mism = 0;
			for (uint32_t i = 0; i < n; i++) {
				put_le32(h + 76, start + i);
				rgr_pow_ref(d, h);
				if (memcmp(d, out.data() + 32 * (size_t)i, 32)) mism++;
			}
			if (mism) { LOG("GPU %d digest test %d: %d mismatches", dev, hdr, mism); gf++; }
		}
		/* 2) scan with an easy target vs CPU brute force over 2^20 nonces */
		for (int hdr = 0; hdr < 2; hdr++) {
			uint8_t h[80];
			for (auto &b : h) b = (uint8_t)rng();
			uint32_t t8[8];
			memset(t8, 0xff, sizeof(t8));
			t8[7] = 0x00003fffu;                 /* ~1 / 2^18 */
			const uint32_t count = 1u << 20, start = (uint32_t)rng() & 0xfff00000u;
			std::set<uint32_t> cpu, gpu;
			for (uint32_t i = 0; i < count; i++) {
				put_le32(h + 76, start + i);
				rgr_pow_ref(d, h);
				if (rgr_digest_le_target(d, t8)) cpu.insert(start + i);
			}
			for (uint32_t off = 0; off < count; off += 1u << 18) {
				uint32_t f[RG_MAX_FOUND];
				int nf = 0;
				int e = rg_scan(ctx, h, t8, start + off, 1u << 18, f, RG_MAX_FOUND, &nf, nullptr);
				if (e) { LOG("GPU %d rg_scan: %s", dev, rg_error_string(e)); gf++; break; }
				for (int i = 0; i < nf; i++) gpu.insert(f[i]);
			}
			if (cpu != gpu) { LOG("GPU %d scan test %d: CPU %zu hits, GPU %zu hits", dev, hdr, cpu.size(), gpu.size()); gf++; }
			else LOG("GPU %d scan test %d: %zu hits, identical", dev, hdr, cpu.size());
		}
		std::string why;
		if (!selfcheck_gpu(ctx, dev, why)) { LOG("GPU %d self-check: %s", dev, why.c_str()); gf++; }
		LOG("GPU %d (%s): %s", dev, g_stats[gi]->name.c_str(), gf ? "FAILED" : "ALL OK");
		fails += gf;
		rg_destroy(ctx);
	}
	LOG("TEST %s", fails ? "FAILED" : "ALL OK");
	return fails ? 1 : 0;
}

static int mode_bench()
{
	std::vector<int> devs;
	if (!open_gpus(devs)) return 3;
	std::vector<std::thread> th;
	std::vector<double> rate(devs.size(), 0);
	for (size_t gi = 0; gi < devs.size(); gi++) {
		th.emplace_back([gi, &devs, &rate] {
			int dev = devs[gi];
			char err[256];
			void *ctx = rg_create(dev, g_cfg.tpb, err, sizeof(err));
			if (!ctx) { LOG("GPU %d: init failed: %s", dev, err); return; }
			uint8_t h[80];
			for (int i = 0; i < 80; i++) h[i] = (uint8_t)(i * 7 + gi);
			uint32_t t8[8] = { 0 };
			const uint32_t batch = 1u << g_cfg.batch_log2;
			double t0 = now_s(), tl = t0;
			uint64_t n = 0, nl = 0;
			uint32_t start = 0;
			float gpu_ms = 0, ms;
			while (now_s() - t0 < g_cfg.bench_s) {
				uint32_t f[RG_MAX_FOUND];
				int nf;
				int e = rg_scan(ctx, h, t8, start, batch, f, RG_MAX_FOUND, &nf, &ms);
				if (e) { LOG("GPU %d: %s", dev, rg_error_string(e)); break; }
				start += batch;
				n += batch;
				gpu_ms += ms;
				if (now_s() - tl >= 10) { LOG("GPU %d: %.1f MH/s", dev, (n - nl) / (now_s() - tl) / 1e6); tl = now_s(); nl = n; }
			}
			double dt = now_s() - t0;
			rate[gi] = n / dt;
			LOG("GPU %d: %.1f MH/s average over %.0f s (GPU busy %.1f %%)", dev, n / dt / 1e6, dt, 100.0 * gpu_ms / 1000.0 / dt);
			rg_destroy(ctx);
		});
	}
	for (auto &t : th) t.join();
	double tot = 0;
	for (double r : rate) tot += r;
	LOG("BENCH total %.1f MH/s (tpb %d, batch 2^%d)", tot / 1e6, g_cfg.tpb, g_cfg.batch_log2);
	return 0;
}

static int mode_propose()
{
	std::shared_ptr<Template> t;
	std::string err;
	if (!fetch_template(t, err)) { LOG("getblocktemplate: %s", err.c_str()); return 4; }
	LOG("template height %lld, %zu tx, diff %.1f, reward %.8f ORU, time %lld..%lld",
		(long long)t->height, t->ntx, target_difficulty(t->target), t->coinbasevalue / 1e8, (long long)t->base_time, (long long)t->maxtime);
	int fails = 0;
	for (int k = 0; k < 3; k++) {
		uint8_t h[80];
		memcpy(h, t->header, 80);
		int64_t ntime = t->base_time + (k == 0 ? 0 : k == 1 ? 7 : 900);
		put_le32(h + 68, (uint32_t)ntime);
		put_le32(h + 76, 0x12345678u + k);
		JVal r;
		bool ok = g_rpc.call("getblocktemplate", gbt_params("proposal", to_hex(h, 80) + t->txs_hex), r, err, 30);
		std::string res = ok ? (r.is_null() ? "null (block valid apart from the proof of work)" : (r.t == JVal::STR ? r.s : "?")) : err;
		LOG("proposal time +%lld s: %s", (long long)(ntime - t->base_time), res.c_str());
		if (!ok || !r.is_null()) fails++;
	}
	LOG("PROPOSE %s", fails ? "FAILED" : "OK");
	return fails ? 1 : 0;
}

static int mode_check_chain()
{
	JVal r;
	std::string err;
	if (!g_rpc.call("getblockcount", "[]", r, err)) { LOG("getblockcount: %s", err.c_str()); return 4; }
	long long tip = r.as_int();
	int fails = 0;
	for (long long h = tip - g_cfg.check_n + 1; h <= tip; h++) {
		JVal hash, hdr;
		if (!g_rpc.call("getblockhash", "[" + std::to_string(h) + "]", hash, err) ||
		    !g_rpc.call("getblockheader", "[\"" + hash.s + "\",false]", hdr, err)) { LOG("height %lld: %s", h, err.c_str()); fails++; continue; }
		std::vector<uint8_t> b;
		if (!from_hex(hdr.s, b) || b.size() != 80) { LOG("height %lld: bad header", h); fails++; continue; }
		uint8_t d[32], bh[32];
		uint32_t t8[8];
		rgr_pow_ref(d, b.data());
		sha256d(bh, b.data(), 80);
		bits_to_target(get_le32(b.data() + 72), t8);
		bool ok = rgr_digest_le_target(d, t8) && to_hex_rev(bh, 32) == hash.s;
		LOG("height %lld: pow %s %s target %s : %s", h, to_hex_rev(d, 32).c_str(), ok ? "<=" : "?", target_hex(t8).c_str(), ok ? "OK" : "FAILED");
		fails += !ok;
	}
	LOG("CHECK-CHAIN %s", fails ? "FAILED" : "OK");
	return fails ? 1 : 0;
}

static void stats_thread()
{
	std::vector<uint64_t> last(g_stats.size(), 0);
	double tl = now_s();
	while (!g_stop) {
		for (int i = 0; i < g_cfg.stats_s * 10 && !g_stop; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
		double dt = now_s() - tl;
		tl = now_s();
		double tot = 0;
		std::string per;
		for (size_t i = 0; i < g_stats.size(); i++) {
			uint64_t h = g_stats[i]->hashes;
			double r = (h - last[i]) / dt;
			last[i] = h;
			tot += r;
			char b[64];
			snprintf(b, sizeof(b), "%sGPU%d %.0f", per.empty() ? "" : ", ", g_stats[i]->dev, r / 1e6);
			per += b;
		}
		auto t = current_tpl();
		double diff = t ? target_difficulty(t->target) : 0;
		double eta_h = (diff > 0 && tot > 0) ? diff * 4294967296.0 / tot / 3600.0 : 0;
		uint64_t wrong = 0;
		for (auto &s : g_stats) wrong += s->wrong;
		if (g_pool_mode) {
			double el = now_s() - g_start_s;
			double eff = el > 0 ? g_sh_ok.load() * g_cfg.share_work / el : 0;
			uint64_t ns = g_sh_sent.load();
			double avg_ms = ns ? (double)g_sh_ms.load() / ns : 0;
			uint64_t qmax = g_sh_qmax.exchange(0);
			LOG("%.1f MH/s total (%s) | pool: shares %llu ok, %llu rejected, %llu stale, %.0f MH/s from shares, submit %.0f ms, queue max %llu | height %lld diff %.1f | pool blocks by us %llu | hw errors %llu",
				tot / 1e6, per.c_str(), (unsigned long long)g_sh_ok.load(), (unsigned long long)g_sh_rej.load(), (unsigned long long)g_sh_stale.load(),
				eff / 1e6, avg_ms, (unsigned long long)qmax, t ? (long long)t->height : 0LL, diff, (unsigned long long)g_sh_blocks.load(), (unsigned long long)wrong);
		} else
		LOG("%.1f MH/s total (%s) | height %lld diff %.1f | expected 1 block / %.1f h at this diff | blocks %llu accepted, %llu rejected | hw errors %llu",
			tot / 1e6, per.c_str(), t ? (long long)t->height : 0LL, diff, eta_h,
			(unsigned long long)g_blocks_ok.load(), (unsigned long long)g_blocks_rej.load(), (unsigned long long)wrong);
	}
}

static int mode_mine()
{
	if (g_pool_mode) {
		size_t dot = g_cfg.pool_user.find('.');
		std::string addr = g_cfg.pool_user.substr(0, dot);
		std::string worker = dot == std::string::npos ? "" : g_cfg.pool_user.substr(dot + 1);
		bool wok = !worker.empty() && worker.size() <= 48;
		for (char c : worker) if (!isalnum((unsigned char)c) && c != '_' && c != '-') wok = false;
		if (addr.rfind("rg1q", 0) != 0 || addr.size() != 42 || !wok) {
			LOG("--pool-user must be <rg1q address>.<worker> (worker: 1-48 letters, digits, _ or -)");
			return 2;
		}
		if (g_cfg.share_work < 1e6) { LOG("--share-work too small"); return 2; }
		if (g_cfg.share_threads < 1 || g_cfg.share_threads > 32) { LOG("--share-threads must be 1..32"); return 2; }
	} else if (g_cfg.address.rfind("rg1q", 0) != 0) { LOG("--address must be a mainnet ORU address starting with rg1q"); return 2; }
	std::vector<int> devs;
	if (!open_gpus(devs)) return 3;
	g_start_s = now_s();
	if (g_pool_mode)
		LOG("RgrMiner %s, POOL %s://%s%s as %s, share work %.3g, %d share sender(s), %zu GPU(s), tpb %d, batch 2^%d",
			RGR_VERSION, g_pool.tls ? "https" : "http", g_pool.host.c_str(), g_pool.path.c_str(), g_cfg.pool_user.c_str(),
			g_cfg.share_work, g_cfg.share_threads, devs.size(), g_cfg.tpb, g_cfg.batch_log2);
	else
		LOG("RgrMiner %s, solo on %s:%d, payout %s, %zu GPU(s), tpb %d, batch 2^%d",
			RGR_VERSION, g_cfg.rpc_host.c_str(), g_cfg.rpc_port, g_cfg.address.c_str(), devs.size(), g_cfg.tpb, g_cfg.batch_log2);
	std::vector<std::thread> shq;
	std::thread tw;
	if (g_pool_mode) {
		for (int i = 0; i < g_cfg.share_threads; i++) shq.emplace_back(share_thread);
		if (g_cfg.tip_watch_ms > 0 && !g_rpc.user.empty()) tw = std::thread(tip_watch_thread);
	}
	std::thread tt(template_thread);
	for (int i = 0; i < 100 && !current_tpl(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
	std::vector<std::thread> th;
	for (size_t i = 0; i < devs.size(); i++) th.emplace_back(gpu_thread, (int)i, devs[i]);
	std::thread st(stats_thread);
	if (g_cfg.run_s > 0)
		std::thread([] { for (int i = 0; i < g_cfg.run_s * 10 && !g_stop; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
			LOG("run-seconds reached, stopping"); g_stop = true; }).detach();
	for (auto &t : th) t.join();
	g_stop = true;
	poll_now();
	tt.join();
	st.join();
	g_shq_cv.notify_all();
	for (auto &x : shq) x.join();
	if (tw.joinable()) tw.join();
	if (g_pool_mode) LOG("pool session end: shares %llu ok, %llu rejected, %llu stale", (unsigned long long)g_sh_ok.load(), (unsigned long long)g_sh_rej.load(), (unsigned long long)g_sh_stale.load());
	return g_cfg.run_s > 0 ? 0 : 1;   /* the GPU threads never end normally: let the .bat restart us */
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv)
{
	std::vector<std::pair<std::string, std::string>> kv;
	std::vector<std::string> args(argv + 1, argv + argc);
	/* expand --config / --args-file first */
	for (size_t i = 0; i < args.size(); i++) {
		if ((args[i] == "--config") && i + 1 < args.size()) {
			if (!load_kv_file(args[i + 1], kv)) { fprintf(stderr, "cannot read %s\n", args[i + 1].c_str()); return 2; }
			args.erase(args.begin() + i, args.begin() + i + 2); i--;
		} else if (args[i] == "--args-file" && i + 1 < args.size()) {
			std::ifstream f(args[i + 1]);
			std::string w;
			std::vector<std::string> extra;
			while (f >> w) extra.push_back(w);
			args.erase(args.begin() + i, args.begin() + i + 2);
			args.insert(args.end(), extra.begin(), extra.end());
			i--;
		}
	}
	for (auto &p : kv) if (!set_opt(g_cfg, p.first, p.second)) { fprintf(stderr, "unknown config key %s\n", p.first.c_str()); return 2; }
	for (size_t i = 0; i < args.size(); i++) {
		const std::string &a = args[i];
		if (a == "-h" || a == "--help") { usage(); return 0; }
		else if (a == "--test") g_cfg.mode = "test";
		else if (a == "--propose") g_cfg.mode = "propose";
		else if (a == "--bench") { g_cfg.mode = "bench"; if (i + 1 < args.size() && isdigit((unsigned char)args[i + 1][0])) g_cfg.bench_s = atoi(args[++i].c_str()); }
		else if (a == "--check-chain") { g_cfg.mode = "check"; if (i + 1 < args.size() && isdigit((unsigned char)args[i + 1][0])) g_cfg.check_n = atoi(args[++i].c_str()); }
		else if (i + 1 < args.size() && set_opt(g_cfg, a, args[i + 1])) i++;
		else { fprintf(stderr, "unknown option %s\n", a.c_str()); usage(); return 2; }
	}
	if (!g_cfg.log.empty()) Log::get().open(g_cfg.log);
	if (g_cfg.batch_log2 < 16 || g_cfg.batch_log2 > 31) { LOG("--batch must be 16..31"); return 2; }
	if ((1u << g_cfg.batch_log2) % (uint32_t)g_cfg.tpb) { LOG("2^batch must be a multiple of tpb"); return 2; }
	net_init();
	g_rpc.host = g_cfg.rpc_host;
	g_rpc.port = g_cfg.rpc_port;
	g_rpc.user = g_cfg.rpc_user;
	g_rpc.pass = g_cfg.rpc_pass;
	if (!g_cfg.auth_file.empty()) {
		std::vector<std::pair<std::string, std::string>> a;
		if (!load_kv_file(g_cfg.auth_file, a)) { LOG("cannot read auth file %s", g_cfg.auth_file.c_str()); if (g_cfg.mode != "test" && g_cfg.mode != "bench") return 2; }
		for (auto &p : a) { if (p.first == "rpcuser") g_rpc.user = p.second; if (p.first == "rpcpassword") g_rpc.pass = p.second; }
	}
	if (g_cfg.mode == "test") return mode_test();
	if (g_cfg.mode == "bench") return mode_bench();
	if (!g_cfg.pool_url.empty()) {
		std::string err;
		g_pool_mode = true;
		g_pool.name = "pool";
		g_pool.cafile = g_cfg.ca_file;
		g_pool.user = g_cfg.pool_user;
		g_pool.pass = g_cfg.pool_pass;
		if (!g_pool.set_url(g_cfg.pool_url, err)) { LOG("--pool-url: %s", err.c_str()); return 2; }
		if (g_cfg.mode != "mine") { LOG("pool mode only mines (use --test / --bench without pool settings)"); return 2; }
		return mode_mine();
	}
	if (g_cfg.address.empty() && g_cfg.mode != "check") { LOG("--address is required"); return 2; }
	if (g_cfg.mode == "propose") return mode_propose();
	if (g_cfg.mode == "check") return mode_check_chain();
	return mode_mine();
}
