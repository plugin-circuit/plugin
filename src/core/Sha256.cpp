#include "core/Util.h"

#include <tanh/net/Sha256.h>

namespace plugincircuit {

std::string sha256Hex(const void *data, size_t size) {
	return thl::net::Sha256::to_hex(thl::net::Sha256::of(data, size));
}

std::string sha256HexOfFile(const fs::path &file) {
	auto r = thl::net::Sha256::of_file(file);
	return r.m_ok ? thl::net::Sha256::to_hex(r.m_digest) : std::string();
}

} // namespace plugincircuit
