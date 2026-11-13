#include "core/Util.h"

#include <cstring>
#include <fstream>
#include <choc/containers/choc_zlib.h>
#include <sstream>

namespace plugincircuit {

static bool gunzip(const std::vector<uint8_t> &in, std::vector<uint8_t> &out, std::string &error) {
	// Parse the gzip header (RFC 1952) ourselves and inflate the raw deflate payload.
	if (in.size() < 18 || in[0] != 0x1f || in[1] != 0x8b || in[2] != 8) {
		error = "not a gzip file";
		return false;
	}
	uint8_t flags = in[3];
	size_t pos = 10;
	if (flags & 4) { // FEXTRA
		if (pos + 2 > in.size()) { error = "gzip: bad header"; return false; }
		pos += 2 + size_t(in[pos] | (in[pos + 1] << 8));
	}
	if (flags & 8) while (pos < in.size() && in[pos++]) {}  // FNAME
	if (flags & 16) while (pos < in.size() && in[pos++]) {} // FCOMMENT
	if (flags & 2) pos += 2;                                  // FHCRC
	if (pos >= in.size()) { error = "gzip: bad header"; return false; }
	try {
		auto src = std::make_shared<std::istringstream>(std::string(in.begin() + (long)pos, in.end()), std::ios::binary);
		choc::zlib::InflaterStream inflater(src, choc::zlib::InflaterStream::FormatType::deflate);
		out.clear();
		std::vector<char> chunk(1 << 18);
		while (inflater) {
			inflater.read(chunk.data(), (std::streamsize)chunk.size());
			auto n = inflater.gcount();
			if (n <= 0) break;
			out.insert(out.end(), chunk.data(), chunk.data() + n);
		}
	} catch (const std::exception &e) {
		error = std::string("gzip: ") + e.what();
		return false;
	}
	if (out.empty()) {
		error = "gzip: no data";
		return false;
	}
	return true;
}

static uint64_t parseOctal(const char *p, size_t n) {
	uint64_t v = 0;
	for (size_t i = 0; i < n && p[i]; ++i) {
		if (p[i] == ' ') continue;
		if (p[i] < '0' || p[i] > '7') break;
		v = v * 8 + uint64_t(p[i] - '0');
	}
	return v;
}

static bool safeRelative(const std::string &name) {
	if (name.empty() || name[0] == '/') return false;
	fs::path p(name);
	for (const auto &part : p) {
		if (part == "..") return false;
	}
	return true;
}

std::string extractTarGz(const std::vector<uint8_t> &gz, const fs::path &destDir) {
	std::vector<uint8_t> tar;
	std::string error;
	if (!gunzip(gz, tar, error)) return error;

	std::error_code ec;
	fs::create_directories(destDir, ec);
	if (ec) return "can't create " + destDir.string() + ": " + ec.message();

	size_t pos = 0;
	std::string longName; // GNU 'L' long names and pax 'path'
	while (pos + 512 <= tar.size()) {
		const char *h = reinterpret_cast<const char *>(tar.data() + pos);
		bool allZero = true;
		for (int i = 0; i < 512; ++i)
			if (h[i]) { allZero = false; break; }
		if (allZero) break;

		std::string name(h, strnlen(h, 100));
		std::string prefix(h + 345, strnlen(h + 345, 155));
		if (std::memcmp(h + 257, "ustar", 5) == 0 && !prefix.empty()) name = prefix + "/" + name;
		uint64_t size = parseOctal(h + 124, 12);
		char type = h[156];
		size_t dataPos = pos + 512;
		size_t next = dataPos + size_t((size + 511) / 512 * 512);
		if (dataPos + size > tar.size()) return "tar: truncated entry " + name;

		if (type == 'L') { // GNU long name for the next entry
			longName.assign(reinterpret_cast<const char *>(tar.data() + dataPos), size);
			while (!longName.empty() && longName.back() == '\0') longName.pop_back();
			pos = next;
			continue;
		}
		if (type == 'x') { // pax header: look for "path="
			std::string pax(reinterpret_cast<const char *>(tar.data() + dataPos), size);
			auto at = pax.find(" path=");
			if (at != std::string::npos) {
				auto end = pax.find('\n', at);
				longName = pax.substr(at + 6, end - (at + 6));
			}
			pos = next;
			continue;
		}
		if (type == 'g') { pos = next; continue; }
		if (!longName.empty()) {
			name = longName;
			longName.clear();
		}
		while (name.rfind("./", 0) == 0) name = name.substr(2);

		if (!name.empty() && name != ".") {
			if (!safeRelative(name)) return "tar: unsafe path " + name;
			fs::path out = destDir / name;
			if (type == '5') {
				fs::create_directories(out, ec);
			} else if (type == '0' || type == '\0' || type == '7') {
				fs::create_directories(out.parent_path(), ec);
				std::ofstream f(out, std::ios::binary);
				if (!f) return "can't write " + out.string();
				f.write(reinterpret_cast<const char *>(tar.data() + dataPos), (std::streamsize)size);
			} // symlinks, devices etc. are skipped on purpose
		}
		pos = next;
	}
	return {};
}

fs::path findWclapRoot(const fs::path &dir) {
	std::error_code ec;
	if (fs::exists(dir / "module.wasm", ec)) return dir;
	for (auto it = fs::recursive_directory_iterator(dir, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
		if (ec) break;
		if (it->is_regular_file() && it->path().filename() == "module.wasm") return it->path().parent_path();
	}
	return {};
}

} // namespace plugincircuit
