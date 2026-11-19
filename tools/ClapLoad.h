#pragma once
// Loads a CLAP and returns its entry, for the tools and tests that drive the built PluginCircuit.clap.
// macOS: the bundle (<path>/Contents/MacOS/PluginCircuit); Windows: the .clap DLL itself; Linux: the .clap .so.
#include <clap/clap.h>

#include <cstdio>
#include <string>

#if defined(_WIN32)
#	ifndef NOMINMAX
#		define NOMINMAX
#	endif
#	include <windows.h>
#else
#	include <dlfcn.h>
#endif

inline const clap_plugin_entry *loadClapEntry(const char *path) {
#if defined(_WIN32)
	HMODULE lib = LoadLibraryA(path);
	if (!lib) {
		std::printf("LoadLibrary %s failed: %lu\n", path, GetLastError());
		return nullptr;
	}
	return (const clap_plugin_entry *)GetProcAddress(lib, "clap_entry");
#else
#	if defined(__APPLE__)
	const std::string bin = std::string(path) + "/Contents/MacOS/PluginCircuit";
#	else
	const std::string bin = path;
#	endif
	void *lib = dlopen(bin.c_str(), RTLD_NOW);
	if (!lib) {
		std::printf("dlopen: %s\n", dlerror());
		return nullptr;
	}
	return (const clap_plugin_entry *)dlsym(lib, "clap_entry");
#endif
}
