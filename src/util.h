/**
 * RgrMiner - small helpers: JSON parser, hex, SHA-256, base64, logging, time.
 */
#pragma once
#include <stdint.h>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <utility>
#include <mutex>
#include <cstdio>
#include <cstdarg>
#include <ctime>
#include <chrono>

/* ------------------------------------------------------------------ JSON */

struct JVal {
	enum Type { NUL, BOOL, NUM, STR, ARR, OBJ } t = NUL;
	bool b = false;
	std::string s;                                  /* string value, or the raw text of a number */
	std::vector<JVal> a;
	std::vector<std::pair<std::string, JVal>> o;
	const JVal *get(const char *k) const
	{
		if (t != OBJ) return nullptr;
		for (auto &kv : o) if (kv.first == k) return &kv.second;
		return nullptr;
	}
	bool is_null() const { return t == NUL; }
	long long as_int(long long def = 0) const { return t == NUM ? strtoll(s.c_str(), nullptr, 10) : def; }
	double as_num(double def = 0) const { return t == NUM ? strtod(s.c_str(), nullptr) : def; }
	std::string str(const char *k, const char *def = "") const { const JVal *v = get(k); return (v && v->t == STR) ? v->s : def; }
	long long num(const char *k, long long def = 0) const { const JVal *v = get(k); return v ? v->as_int(def) : def; }
};

class JParser {
public:
	explicit JParser(const std::string &txt) : p_(txt.c_str()), e_(txt.c_str() + txt.size()) {}
	bool parse(JVal &v) { ws(); if (!value(v, 0)) return false; ws(); return p_ == e_; }
private:
	const char *p_, *e_;
	void ws() { while (p_ < e_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\r' || *p_ == '\n')) p_++; }
	bool lit(const char *w) { size_t n = strlen(w); if ((size_t)(e_ - p_) < n || strncmp(p_, w, n)) return false; p_ += n; return true; }
	static void utf8(std::string &out, unsigned c)
	{
		if (c < 0x80) out += (char)c;
		else if (c < 0x800) { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 0x3F)); }
		else if (c < 0x10000) { out += (char)(0xE0 | (c >> 12)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
		else { out += (char)(0xF0 | (c >> 18)); out += (char)(0x80 | ((c >> 12) & 0x3F)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
	}
	bool string(std::string &out)
	{
		if (p_ >= e_ || *p_ != '"') return false;
		p_++;
		while (p_ < e_ && *p_ != '"') {
			char c = *p_++;
			if (c != '\\') { out += c; continue; }
			if (p_ >= e_) return false;
			c = *p_++;
			switch (c) {
			case '"': out += '"'; break; case '\\': out += '\\'; break; case '/': out += '/'; break;
			case 'b': out += '\b'; break; case 'f': out += '\f'; break; case 'n': out += '\n'; break;
			case 'r': out += '\r'; break; case 't': out += '\t'; break;
			case 'u': {
				if (e_ - p_ < 4) return false;
				unsigned u = (unsigned)strtoul(std::string(p_, 4).c_str(), nullptr, 16); p_ += 4;
				if (u >= 0xD800 && u < 0xDC00 && e_ - p_ >= 6 && p_[0] == '\\' && p_[1] == 'u') {
					unsigned l = (unsigned)strtoul(std::string(p_ + 2, 4).c_str(), nullptr, 16);
					if (l >= 0xDC00 && l < 0xE000) { p_ += 6; u = 0x10000 + ((u - 0xD800) << 10) + (l - 0xDC00); }
				}
				utf8(out, u); break;
			}
			default: return false;
			}
		}
		if (p_ >= e_) return false;
		p_++;
		return true;
	}
	bool value(JVal &v, int depth)
	{
		if (depth > 64 || p_ >= e_) return false;
		char c = *p_;
		if (c == '{') {
			v.t = JVal::OBJ; p_++; ws();
			if (p_ < e_ && *p_ == '}') { p_++; return true; }
			for (;;) {
				std::string k; ws(); if (!string(k)) return false; ws();
				if (p_ >= e_ || *p_ != ':') return false;
				p_++; ws();
				JVal x; if (!value(x, depth + 1)) return false;
				v.o.emplace_back(std::move(k), std::move(x)); ws();
				if (p_ < e_ && *p_ == ',') { p_++; continue; }
				if (p_ < e_ && *p_ == '}') { p_++; return true; }
				return false;
			}
		}
		if (c == '[') {
			v.t = JVal::ARR; p_++; ws();
			if (p_ < e_ && *p_ == ']') { p_++; return true; }
			for (;;) {
				JVal x; ws(); if (!value(x, depth + 1)) return false;
				v.a.push_back(std::move(x)); ws();
				if (p_ < e_ && *p_ == ',') { p_++; continue; }
				if (p_ < e_ && *p_ == ']') { p_++; return true; }
				return false;
			}
		}
		if (c == '"') { v.t = JVal::STR; return string(v.s); }
		if (lit("true")) { v.t = JVal::BOOL; v.b = true; return true; }
		if (lit("false")) { v.t = JVal::BOOL; v.b = false; return true; }
		if (lit("null")) { v.t = JVal::NUL; return true; }
		const char *s = p_;
		if (p_ < e_ && (*p_ == '-' || *p_ == '+')) p_++;
		while (p_ < e_ && ((*p_ >= '0' && *p_ <= '9') || *p_ == '.' || *p_ == 'e' || *p_ == 'E' || *p_ == '-' || *p_ == '+')) p_++;
		if (p_ == s) return false;
		v.t = JVal::NUM; v.s.assign(s, p_);
		return true;
	}
};

static inline std::string json_escape(const std::string &s)
{
	std::string o = "\"";
	for (unsigned char c : s) {
		if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
		else if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
		else o += (char)c;
	}
	return o + "\"";
}

/* ------------------------------------------------------------------ hex */

static inline int hexval(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}
static inline bool from_hex(const std::string &h, std::vector<uint8_t> &out)
{
	if (h.size() & 1) return false;
	out.resize(h.size() / 2);
	for (size_t i = 0; i < out.size(); i++) {
		int a = hexval(h[2 * i]), b = hexval(h[2 * i + 1]);
		if (a < 0 || b < 0) return false;
		out[i] = (uint8_t)(a << 4 | b);
	}
	return true;
}
static inline std::string to_hex(const uint8_t *p, size_t n)
{
	static const char *d = "0123456789abcdef";
	std::string s(n * 2, '0');
	for (size_t i = 0; i < n; i++) { s[2 * i] = d[p[i] >> 4]; s[2 * i + 1] = d[p[i] & 15]; }
	return s;
}
/* 32 bytes in internal order -> RPC display hex (byte-reversed) */
static inline std::string to_hex_rev(const uint8_t *p, size_t n)
{
	std::vector<uint8_t> r(p, p + n);
	for (size_t i = 0; i < n / 2; i++) std::swap(r[i], r[n - 1 - i]);
	return to_hex(r.data(), n);
}

/* ------------------------------------------------------------------ SHA-256 */

struct Sha256 {
	uint32_t h[8]; uint8_t buf[64]; uint64_t len = 0; size_t n = 0;
	Sha256() { static const uint32_t i[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 }; memcpy(h, i, 32); }
	static uint32_t ror(uint32_t x, int r) { return (x >> r) | (x << (32 - r)); }
	void block(const uint8_t *p)
	{
		static const uint32_t k[64] = {
			0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
			0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
			0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
			0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
		uint32_t w[64], a, b, c, d, e, f, g, hh;
		for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
		for (int i = 16; i < 64; i++) w[i] = w[i - 16] + (ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3)) + w[i - 7] + (ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10));
		a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4]; f = h[5]; g = h[6]; hh = h[7];
		for (int i = 0; i < 64; i++) {
			uint32_t t1 = hh + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
			uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
			hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
		}
		h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
	}
	void update(const uint8_t *p, size_t l)
	{
		len += l;
		while (l) { size_t k = 64 - n < l ? 64 - n : l; memcpy(buf + n, p, k); n += k; p += k; l -= k; if (n == 64) { block(buf); n = 0; } }
	}
	void final(uint8_t out[32])
	{
		uint64_t bits = len * 8; uint8_t pad = 0x80; update(&pad, 1); pad = 0;
		while (n != 56) update(&pad, 1);
		uint8_t lb[8]; for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(bits >> (56 - 8 * i));
		update(lb, 8);
		for (int i = 0; i < 8; i++) { out[4 * i] = (uint8_t)(h[i] >> 24); out[4 * i + 1] = (uint8_t)(h[i] >> 16); out[4 * i + 2] = (uint8_t)(h[i] >> 8); out[4 * i + 3] = (uint8_t)h[i]; }
	}
};
static inline void sha256d(uint8_t out[32], const uint8_t *p, size_t n)
{
	uint8_t t[32];
	{ Sha256 s; s.update(p, n); s.final(t); }
	{ Sha256 s; s.update(t, 32); s.final(out); }
}

/* ------------------------------------------------------------------ base64 */

static inline std::string base64(const std::string &in)
{
	static const char *t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string o;
	size_t i = 0;
	for (; i + 2 < in.size(); i += 3) {
		uint32_t v = (uint8_t)in[i] << 16 | (uint8_t)in[i + 1] << 8 | (uint8_t)in[i + 2];
		o += t[v >> 18]; o += t[(v >> 12) & 63]; o += t[(v >> 6) & 63]; o += t[v & 63];
	}
	if (i + 1 == in.size()) { uint32_t v = (uint8_t)in[i] << 16; o += t[v >> 18]; o += t[(v >> 12) & 63]; o += "=="; }
	else if (i + 2 == in.size()) { uint32_t v = (uint8_t)in[i] << 16 | (uint8_t)in[i + 1] << 8; o += t[v >> 18]; o += t[(v >> 12) & 63]; o += t[(v >> 6) & 63]; o += '='; }
	return o;
}

/* ------------------------------------------------------------------ time / logging */

static inline double now_s()
{
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
static inline int64_t unix_now() { return (int64_t)time(nullptr); }

class Log {
public:
	static Log &get() { static Log l; return l; }
	void open(const std::string &path) { std::lock_guard<std::mutex> g(m_); if (f_) fclose(f_); f_ = fopen(path.c_str(), "a"); }
	void write(const char *fmt, ...)
	{
		char msg[4096];
		va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof(msg), fmt, ap); va_end(ap);
		time_t t = time(nullptr);
		std::lock_guard<std::mutex> g(m_);
		struct tm tm = *localtime(&t);
		char ts[32]; strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
		printf("[%s] %s\n", ts, msg); fflush(stdout);
		if (f_) { fprintf(f_, "[%s] %s\n", ts, msg); fflush(f_); }
	}
private:
	std::mutex m_; FILE *f_ = nullptr;
};
#define LOG(...) Log::get().write(__VA_ARGS__)
