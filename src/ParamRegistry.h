#pragma once

// Every parameter this plugin defines, recorded as it is defined.
//
// The alternative -- what resolve-multi-transform does -- is a struct mirroring
// each parameter by hand, written out three times: once in the struct, once in
// the serialiser, once in the parser. That is 370 lines there for ~100
// parameters, and this plugin has well over 200. Worse, every new parameter has
// to be added in all three places, and forgetting one produces a preset that
// silently does not save that setting: a bug with no symptom until someone
// reloads a preset and quietly loses their work.
//
// Registering name and type at the point of definition means the preset system
// covers every parameter automatically, including ones added later, and there is
// exactly one list which cannot fall out of step with itself.

#include <string>
#include <vector>

namespace am {

enum ParamKind
{
    kPKNone = 0,   ///< groups, pages, push buttons -- nothing to save
    kPKDouble,
    kPKDouble2D,
    kPKInt,
    kPKBool,
    kPKChoice,
    kPKRGBA,
    kPKString
};

struct ParamEntry
{
    std::string name;
    int         kind = kPKNone;
};

/** @brief The registry, populated during describeInContext.
 *
 * A function-local static rather than a namespace-scope one so it is
 * initialised on first use: describeInContext runs during host plugin loading,
 * which is exactly when static initialisation order is least predictable.
 */
inline std::vector<ParamEntry>& ParamRegistry()
{
    static std::vector<ParamEntry> entries;
    return entries;
}

inline void RegisterParam(const std::string& name, int kind)
{
    if (kind == kPKNone || name.empty()) return;

    // describeInContext can be called more than once -- once per supported
    // context -- and the registry must not accumulate duplicates, or a preset
    // would write every value several times over.
    for (const ParamEntry& e : ParamRegistry())
        if (e.name == name) return;

    ParamRegistry().push_back(ParamEntry{name, kind});
}

} // namespace am

