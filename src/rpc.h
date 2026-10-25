/**
 * RgrMiner - minimal JSON-RPC client over plain HTTP/1.1 (local rigord, 127.0.0.1:8761)
 */
#pragma once
#include "util.h"
#include <string>

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

static inline void net_init()
{
#if defined(_WIN32)
	WSADATA w; WSAStartup(MAKEWORD(2, 2), &w);
#endif
}

class Rpc {
public:
	std::string host = "127.0.0.1";
	int port = 8761;
	std::string user, pass;

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
		if (status == 401) { err = "HTTP 401 (wrong RPC user / password)"; return false; }
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
		if (s == SOCK_BAD) { err = "cannot connect to " + host + ":" + std::to_string(port) + " (is rigord running?)"; return false; }
		std::string req = "POST / HTTP/1.1\r\nHost: " + host + ":" + std::to_string(port) +
			"\r\nAuthorization: Basic " + base64(user + ":" + pass) +
			"\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
		size_t off = 0;
		while (off < req.size()) {
			int n = send(s, req.data() + off, (int)(req.size() - off), 0);
			if (n <= 0) { sock_close(s); err = "send failed"; return false; }
			off += (size_t)n;
		}
		char buf[65536];
		resp.clear();
		for (;;) {
			int n = recv(s, buf, sizeof(buf), 0);
			if (n < 0) { sock_close(s); err = "receive failed or timed out"; return false; }
			if (n == 0) break;
			resp.append(buf, (size_t)n);
			/* stop when Content-Length is satisfied (the server may keep the socket open) */
			size_t he = resp.find("\r\n\r\n");
			if (he != std::string::npos) {
				std::string lower = resp.substr(0, he);
				for (auto &c : lower) c = (char)tolower((unsigned char)c);
				size_t cl = lower.find("content-length:");
				if (cl != std::string::npos && resp.size() >= he + 4 + strtoul(lower.c_str() + cl + 15, nullptr, 10)) break;
			}
		}
		sock_close(s);
		return true;
	}
};
