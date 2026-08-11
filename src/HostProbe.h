#pragma once

#include <string>

namespace am {

/** @brief Append a line to the probe log (%LOCALAPPDATA%\AutoMask\probe.log).
 *
 * Deliberately cheap and fire-and-forget: opens, appends, closes. The log is a
 * development aid for establishing what the host actually supports, since the
 * published documentation for Resolve's OFX host is thin and partly wrong.
 */
void ProbeLog(const std::string& line);

/** @brief Dump the full host description exactly once per process.
 *
 * Safe to call repeatedly; only the first call writes.
 */
void ProbeHostOnce();

/** @brief Log something only the first time this call site sees it.
 *
 * Used for events that fire per-frame (render, overlay draw) where we want
 * evidence that they happen at all, not a flood.
 */
void ProbeOnce(const std::string& key, const std::string& line);

/** @brief Path of the probe log, for reporting to the user. */
std::string ProbeLogPath();

/** @brief Accumulate a timing and report it on a logarithmic schedule.
 *
 * Where time actually goes in a host is not guessable -- an effect can be fast
 * per frame and still make the editor sluggish, because the editor does things
 * per EDIT that have nothing to do with rendering: rebuilding instances, taking
 * undo snapshots, refreshing a parameter panel. This measures the candidates
 * separately so the answer comes from data.
 *
 * Reported at call 1, 10, 100, 1000 ... so early behaviour is visible
 * immediately and long runs do not flood the log.
 */
void ProbeTiming(const char* what, double ms);

/** @brief RAII timer feeding ProbeTiming. */
class ProbeScope
{
public:
    explicit ProbeScope(const char* what);
    ~ProbeScope();

    ProbeScope(const ProbeScope&)            = delete;
    ProbeScope& operator=(const ProbeScope&) = delete;

private:
    const char* _what;
    long long   _start;
};

} // namespace am

