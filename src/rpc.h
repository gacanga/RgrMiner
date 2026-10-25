/**
 * RgrMiner - minimal JSON-RPC client over HTTP/1.1 or HTTPS (OpenSSL, certificate + host name verified).
 *   - local rigord: http://127.0.0.1:8761
 *   - Gozero pool:  https://rgr.pool.gozero.trade/rpc
 * Build with -DRGR_NO_TLS for a plain-HTTP-only version without OpenSSL.
 */
#pragma once
#include "util.h"
#include <string>
#include <mutex>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define SOCK_BAD INVALID_SOCKET
#define sock_close closesocket
#else
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/time.h>
typedef int sock_t;
#define SOCK_BAD (-1)
#define sock_close close
#endif

#ifndef RGR_NO_TLS
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#endif

static inline void net_init()
{
#if defined(_WIN32)
	WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
#endif
#ifndef RGR_NO_TLS
	OPENSSL_init_ssl(0, nullptr);
#endif
}

class Rpc {
public:
	std::string host = "127.0.0.1";
	int port = 8761;
	std::string path = "/";
	bool tls = false;
	std::string cafile;                 /* PEM bundle used to verify the server certificate (TLS) */
	std::string user, pass;
	std::string name = "node";          /* for messages */

	/* http://host[:port][/path]  or  https://host[:port][/path] */
	bool set_url(const std::string &url, std::string &err)
	{
		std::string u = url;
		tls = false;
		if (u.rfind("https://", 0) == 0) { tls = true; u = u.substr(8); }
		else if (u.rfind("http://", 0) == 0) u = u.substr(7);
		else { err = "URL must start with http:// or https://"; return false; }
		size_t s = u.find('/');
		path = s == std::string::npos ? "/" : u.substr(s);
		if (s != std::string::npos) u = u.substr(0, s);
		size_t c = u.rfind(':');
		port = tls ? 443 : 80;
		if (c != std::string::npos) { port = atoi(u.substr(c + 1).c_str()); u = u.substr(0, c); }
		host = u;
		if (host.empty() || port <= 0 || port > 65535) { err = "bad URL " + url; return false; }
#ifdef RGR_NO_TLS
		if (tls) { err = "this build has no TLS"; return false; }
#endif
		return true;
	}

	/* returns true and the "result" member, or false and an error text */
	bool call(const std::string &method, const std::string &params_json, JVal &result, std::string &err, int timeout_s = 30)
	{
		std::string body = "{\"jsonrpc\":\"1.0\",\"id\":1,\"method\":" + json_escape(method) + ",\"params\":" + params_json + "}";
		std::string resp;
		if (!http_post(body, resp, err, timeout_s)) return false;
		size_t hdr_end = resp.find("\r\n\r\n");
		if (hdr_end == std::string::npos) { err = "bad HTTP response"; return false; }
		std::string head = resp.substr(0, hdr_end), data = resp.substr(hdr_end + 4);
		int status = 0;
		if (sscanf(head.c_str(), "HTTP/%*s %d", &status) != 1) { err = "bad HTTP status line"; return false; }
		if (status == 401 || status == 403) { err = "HTTP " + std::to_string(status) + " (authentication refused)"; return false; }
		std::string lower = head;
		for (auto &c : lower) c = (char)tolower((unsigned char)c);
		if (lower.find("transfer-encoding: chunked") != std::string::npos) data = dechunk(data);
		JVal root;
		if (!JParser(data).parse(root)) { err = "HTTP " + std::to_string(status) + ", invalid JSON: " + data.substr(0, 200); return false; }
		const JVal *e = root.get("error");
		if (e && !e->is_null()) {
			err = "RPC error " + std::to_string(e->num("code")) + ": " + e->str("message");
			return false;
		}
		const JVal *r = root.get("result");
		if (!r) { err = "no result"; return false; }
		result = *r;
		return true;
	}

private:
#ifndef RGR_NO_TLS
	/* one SSL_CTX per CA bundle for the whole process (SSL_new from a shared context is thread safe) */
	SSL_CTX *ctx(std::string &err)
	{
		static std::mutex m;
		static SSL_CTX *c = nullptr;
		static std::string ca;
		std::lock_guard<std::mutex> g(m);
		if (c && ca == cafile) return c;
		SSL_CTX *n = SSL_CTX_new(TLS_client_method());
		if (!n) { err = "SSL_CTX_new failed"; return nullptr; }
		SSL_CTX_set_min_proto_version(n, TLS1_2_VERSION);
		if (cafile.empty() || SSL_CTX_load_verify_locations(n, cafile.c_str(), nullptr) != 1) {
			err = "cannot load the CA bundle '" + cafile + "' (needed to verify " + host + ")";
			SSL_CTX_free(n); return nullptr;
		}
		SSL_CTX_set_verify(n, SSL_VERIFY_PEER, nullptr);
		c = n;            /* an older context, if any, is kept alive (never freed) */
		ca = cafile;
		return c;
	}
	static std::string ssl_err()
	{
		unsigned long e = ERR_get_error();
		if (!e) return "TLS error";
		char b[256]; ERR_error_string_n(e, b, sizeof b); return b;
	}
#endif
	static std::string dechunk(const std::string &in)
	{
		std::string out; size_t p = 0;
		while (p < in.size()) {
			size_t e = in.find("\r\n", p);
			if (e == std::string::npos) break;
			unsigned long n = strtoul(in.substr(p, e - p).c_str(), nullptr, 16);
			if (n == 0) break;
			out += in.substr(e + 2, n);
			p = e + 2 + n + 2;
		}
		return out;
	}
	static bool response_complete(const std::string &resp)
	{
		size_t he = resp.find("\r\n\r\n");
		if (he == std::string::npos) return false;
		std::string lower = resp.substr(0, he);
		for (auto &c : lower) c = (char)tolower((unsigned char)c);
		size_t cl = lower.find("content-length:");
		if (cl != std::string::npos) return resp.size() >= he + 4 + strtoul(lower.c_str() + cl + 15, nullptr, 10);
		if (lower.find("transfer-encoding: chunked") != std::string::npos) return resp.find("\r\n0\r\n\r\n", he) != std::string::npos;
		return false;
	}
	bool http_post(const std::string &body, std::string &resp, std::string &err, int timeout_s)
	{
		struct addrinfo hints, *res = nullptr;
		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
		if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) { err = "cannot resolve " + host; return false; }
		sock_t s = SOCK_BAD;
		for (struct addrinfo *a = res; a; a = a->ai_next) {
			s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
			if (s == SOCK_BAD) continue;
#if defined(_WIN32)
			DWORD tv = (DWORD)timeout_s * 1000;
			setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
			setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));
#else
			struct timeval tv; tv.tv_sec = timeout_s; tv.tv_usec = 0;
			setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
			setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
			if (connect(s, a->ai_addr, (int)a->ai_addrlen) == 0) break;
			sock_close(s); s = SOCK_BAD;
		}
		freeaddrinfo(res);
		if (s == SOCK_BAD) { err = "cannot connect to " + host + ":" + std::to_string(port) + (name == "node" ? " (is rigord running?)" : ""); return false; }
		std::string hosthdr = host + ((tls && port == 443) || (!tls && port == 80) ? "" : ":" + std::to_string(port));
		std::string req = "POST " + path + " HTTP/1.1\r\nHost: " + hosthdr +
			"\r\nAuthorization: Basic " + base64(user + ":" + pass) +
			"\r\nUser-Agent: RgrMiner\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
		resp.clear();
		char buf[65536];
#ifndef RGR_NO_TLS
		if (tls) {
			SSL_CTX *c = ctx(err);
			if (!c) { sock_close(s); return false; }
			SSL *ssl = SSL_new(c);
			SSL_set_tlsext_host_name(ssl, host.c_str());
			SSL_set1_host(ssl, host.c_str());
			SSL_set_fd(ssl, (int)s);
			bool ok = SSL_connect(ssl) == 1;
			if (!ok) {
				long vr = SSL_get_verify_result(ssl);
				err = "TLS handshake with " + host + " failed: " + ssl_err();
				if (vr != X509_V_OK) err += std::string(" (certificate: ") + X509_verify_cert_error_string(vr) + ")";
			} else if (SSL_get_verify_result(ssl) != X509_V_OK) {
				ok = false; err = "certificate of " + host + " not verified";
			}
			size_t off = 0;
			while (ok && off < req.size()) {
				int n = SSL_write(ssl, req.data() + off, (int)(req.size() - off));
				if (n <= 0) { ok = false; err = "TLS send failed"; break; }
				off += (size_t)n;
			}
			while (ok) {
				int n = SSL_read(ssl, buf, sizeof(buf));
				if (n <= 0) break;
				resp.append(buf, (size_t)n);
				if (response_complete(resp)) break;
			}
			SSL_shutdown(ssl);
			SSL_free(ssl);
			sock_close(s);
			if (ok && resp.empty()) { ok = false; err = "empty TLS response from " + host; }
			return ok;
		}
#endif
		size_t off = 0;
		while (off < req.size()) {
			int n = send(s, req.data() + off, (int)(req.size() - off), 0);
			if (n <= 0) { sock_close(s); err = "send failed"; return false; }
			off += (size_t)n;
		}
		for (;;) {
			int n = recv(s, buf, sizeof(buf), 0);
			if (n < 0) { sock_close(s); err = "receive failed or timed out"; return false; }
			if (n == 0) break;
			resp.append(buf, (size_t)n);
			if (response_complete(resp)) break;
		}
		sock_close(s);
		return true;
	}
};
