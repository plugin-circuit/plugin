// Thin adapter over tanh-lib's HttpClient (platform-native HTTPS, vendored in third_party/tanh-net).
#include "core/Util.h"

#include <tanh/net/HttpClient.h>

namespace plugincircuit {

static HttpResult convert(const thl::net::HttpResult &r) {
	HttpResult out;
	out.status = r.m_status == thl::net::HttpStatus::Ok ? 200 : r.m_http_code;
	if (r.m_status != thl::net::HttpStatus::Ok && r.m_status != thl::net::HttpStatus::HttpError)
		out.error = r.m_message.empty() ? "network error" : r.m_message;
	return out;
}

HttpResult httpGetString(const std::string &url, long timeoutSeconds) {
	thl::net::HttpClient client(thl::net::HttpClient::Options{.m_timeout_seconds = (int)timeoutSeconds, .m_allow_resume = false});
	std::string body;
	auto r = client.get_to_string(url, body, size_t(16) * 1024 * 1024);
	auto out = convert(r);
	out.body.assign(body.begin(), body.end());
	return out;
}

HttpResult httpDownload(const std::string &url, const fs::path &destination, const HttpProgress &progress, long timeoutSeconds) {
	thl::net::HttpClient client(thl::net::HttpClient::Options{.m_timeout_seconds = (int)timeoutSeconds, .m_allow_resume = true});
	auto r = client.get_to_file(url, destination, [&](const thl::net::HttpProgress &p) {
		return progress ? progress(p.m_downloaded, p.m_total) : true;
	});
	return convert(r);
}

std::string urlEncode(const std::string &s) {
	static const char *hex = "0123456789ABCDEF";
	std::string out;
	for (unsigned char c : s) {
		if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
			out += char(c);
		} else {
			out += '%';
			out += hex[c >> 4];
			out += hex[c & 15];
		}
	}
	return out;
}

} // namespace plugincircuit
