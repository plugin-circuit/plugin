// The catalog check (docs/PLAN.md 5.1): offline while nothing answers, online once the catalog does, checks only
// while an editor wants them, the thread joined at shutdown. A one-route HTTP server stands in for the catalog.
#include "core/CatalogHealth.h"

#if defined(_WIN32)
#	define WIN32_LEAN_AND_MEAN
#	include <winsock2.h>
#	include <ws2tcpip.h>
#	pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
static void closeSocket(socket_t s) { closesocket(s); }
static bool invalid(socket_t s) { return s == INVALID_SOCKET; }
using socklen_t = int;
#else
#	include <arpa/inet.h>
#	include <netinet/in.h>
#	include <sys/socket.h>
#	include <unistd.h>
using socket_t = int;
static void closeSocket(socket_t s) { close(s); }
static bool invalid(socket_t s) { return s < 0; }
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

using namespace plugincircuit;

static int failures = 0;
#define CHECK(cond)                                                                                                    \
	do {                                                                                                               \
		if (!(cond)) {                                                                                                 \
			std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                                \
			++failures;                                                                                                \
		}                                                                                                              \
	} while (0)

// answers every request with 200 until stopped; counts them
struct Server {
	socket_t fd{};
	int port = 0;
	std::atomic<bool> stop{false};
	std::atomic<int> requests{0};
	std::thread t;
	void start() {
#if defined(_WIN32)
		WSADATA wsa;
		WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
		fd = socket(AF_INET, SOCK_STREAM, 0);
		int one = 1;
		setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
		sockaddr_in a{};
		a.sin_family = AF_INET;
		a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		a.sin_port = 0;
		bind(fd, (sockaddr *)&a, sizeof(a));
		socklen_t len = sizeof(a);
		getsockname(fd, (sockaddr *)&a, &len);
		port = ntohs(a.sin_port);
		listen(fd, 8);
		t = std::thread([this] {
			while (!stop) {
				timeval tv{0, 100000};
				fd_set set;
				FD_ZERO(&set);
				FD_SET(fd, &set);
				if (select(int(fd) + 1, &set, nullptr, nullptr, &tv) <= 0) continue; // the first argument is ignored on Windows
				socket_t c = accept(fd, nullptr, nullptr);
				if (invalid(c)) continue;
				char buf[2048];
				(void)recv(c, buf, sizeof(buf), 0);
				const char *reply = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 11\r\nConnection: close\r\n\r\n{\"ok\":true}";
				(void)send(c, reply, int(strlen(reply)), 0);
				closeSocket(c);
				++requests;
			}
		});
	}
	void halt() {
		stop = true;
		if (t.joinable()) t.join();
		closeSocket(fd);
	}
};

template <typename F> static bool waitFor(F f, double seconds) {
	const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
	while (std::chrono::steady_clock::now() < end) {
		if (f()) return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	return f();
}

int main() {
	Server server;
	server.start();
	CatalogHealth::setIntervals(0.3, 0.1, 0.4);

	// nothing listens on the API base yet (a port we closed): offline
	setenv("PLUGINCIRCUIT_API", "http://127.0.0.1:9", 1);
	CHECK(CatalogHealth::status().online == -1);
	CHECK(server.requests == 0);
	CatalogHealth::want(true); // an editor opened
	CHECK(waitFor([] { return CatalogHealth::status().online == 0; }, 5));
	CHECK(!CatalogHealth::status().error.empty());

	// the catalog comes up: online after the next check
	setenv("PLUGINCIRCUIT_API", ("http://127.0.0.1:" + std::to_string(server.port)).c_str(), 1);
	const auto v = CatalogHealth::version();
	CatalogHealth::checkNow();
	CHECK(waitFor([] { return CatalogHealth::status().online == 1; }, 5));
	CHECK(CatalogHealth::version() > v);
	CHECK(CatalogHealth::status().apiBase == "http://127.0.0.1:" + std::to_string(server.port));

	// it keeps checking while wanted (every 0.3 s here) ...
	const int before = server.requests;
	std::this_thread::sleep_for(std::chrono::milliseconds(900));
	CHECK(server.requests >= before + 2);
	// ... and stops when the last editor closes
	CatalogHealth::want(false);
	std::this_thread::sleep_for(std::chrono::milliseconds(200));
	const int idle = server.requests;
	std::this_thread::sleep_for(std::chrono::milliseconds(900));
	CHECK(server.requests == idle);

	// an editor again: a fresh check at once; the catalog goes away: offline
	CatalogHealth::want(true);
	CHECK(waitFor([&] { return server.requests > idle; }, 2));
	server.halt();
	CHECK(waitFor([] { return CatalogHealth::status().online == 0; }, 5));
	CatalogHealth::want(false);
	CatalogHealth::shutdown();

	std::printf(failures ? "%d failure(s)\n" : "PASS: catalog health\n", failures);
	return failures ? 1 : 0;
}
