#pragma once
// The bridge's diagnostics: errors, WebAssembly traps with their message, refused resources. Not connected yet: the
// pinned bridge has no log API (it writes to stderr); upstream's will be wired in install(). A bridge would call on
// the thread that hit the problem, the audio thread included, so messages go into a fixed ring
// (no lock, no allocation; a message is cut at 480 bytes; the oldest are overwritten). Readers (each instance's
// housekeeping, on the main thread) keep their own cursor.
#include <cstdint>
#include <string>

namespace plugincircuit::bridgelog {

void install();       // routes the bridge's log here (before wclap_global_init)
uint64_t end();       // the cursor after the newest message
// The message at `cursor` if it is still in the ring, else false (overwritten: skip it).
bool read(uint64_t cursor, std::string &message);
void writeForTest(const char *message); // as the bridge would

} // namespace plugincircuit::bridgelog
