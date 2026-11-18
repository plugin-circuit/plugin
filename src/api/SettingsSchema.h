#pragma once
// The one place a setting is declared (docs/MESSAGING.md, "Settings"): its key, type, default, range, scope, and the
// label and help text the Advanced tab renders it with. The core validates every change against it.
//
//   machine   settings.json: every instance, every DAW         (the `settings` store)
//   instance  our CLAP state: this instance in this session     (the `ui` store)
//   session   memory only: nothing declared here yet
#include <choc/containers/choc_Value.h>

#include <string>
#include <vector>

namespace plugincircuit {

enum class Scope { Machine, Instance, Session };

struct Setting {
	enum class Type { Bool, Number, Choice };
	const char *key;
	Scope scope;
	Type type;
	double def = 0, min = 0, max = 1, step = 0;   // Number (step 0: any value); Bool: def 0 / 1
	std::vector<std::string> choices;             // Choice; the first is the default
	const char *label = "", *help = "";           // the Advanced tab shows settings with a label
	const char *format = "";                      // Number: "percent" shows 0.2 as 20%
	const char *suffix = "", *zeroLabel = "";     // Number: after each value; the label for 0
};

namespace settings {

const std::vector<Setting> &schema();
const Setting *find(const std::string &key);

// The defaults of one scope, as an object.
choc::value::Value defaults(Scope scope);

// Stored values (a file, a state: untrusted, maybe older or newer): the scope's keys with valid values, clamped; the
// rest default. Unknown keys are left out.
choc::value::Value sanitize(Scope scope, const choc::value::ValueView &stored);

// A change from the page: applied onto `values` (an object of the scope's keys). Numbers are clamped to their range
// and snapped to their step; an unknown key, a key of another scope, or a wrong type is refused with the error.
std::string apply(Scope scope, const choc::value::ValueView &change, choc::value::Value &values);

// The scope's settings with a label, for the page to render.
choc::value::Value describe(Scope scope);

} // namespace settings
} // namespace plugincircuit
