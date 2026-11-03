#include "HostProbe.h"

#if defined(_WIN32)
    #include <shlobj.h>
    #include <windows.h>
#else
    #include <sys/stat.h>
    #include <sys/types.h>

    #include <cstdlib>
#endif

#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

#include "ofxsImageEffect.h"
#include "ofxsSupportPrivate.h"

namespace am {

namespace {

std::mutex g_mutex;
std::set<std::string> g_seen;
bool g_hostDumped = false;

// Where each platform expects an application's own log to live. Falling back
// to the working directory is deliberate: a host that starts the plugin from
// somewhere unwritable should lose the diagnostic, not the plugin.
std::string LogDir()
{
#if defined(_WIN32)
    char path[MAX_PATH] = {0};
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, path)))
    {
        std::string dir = std::string(path) + "\\AutoMask";
        CreateDirectoryA(dir.c_str(), nullptr);
        return dir;
    }
    return ".";
#else
    // ~/Library/Logs is the documented home for this on macOS and is what
    // Console.app shows, so the probe turns up where someone would look.
    const char* home = std::getenv("HOME");
    if (!home || !*home) return ".";

    std::string dir = std::string(home) + "/Library/Logs";
    ::mkdir(dir.c_str(), 0755);   // already present on macOS; harmless if not
    dir += "/AutoMask";
    ::mkdir(dir.c_str(), 0755);
    return dir;
#endif
}

// One implementation rather than two. GetLocalTime and sprintf_s were doing
// nothing chrono and snprintf do not, and a second copy of "format the time"
// is a second place for the platforms to disagree about it.
std::string Stamp()
{
    using namespace std::chrono;
    const auto now = system_clock::now();
    const std::time_t t = system_clock::to_time_t(now);
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

    std::tm local{};
#if defined(_WIN32)
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif

    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d.%03d ", local.tm_hour, local.tm_min,
                  local.tm_sec, int(ms.count()));
    return stamp;
}

const char* YesNo(bool b) { return b ? "yes" : "NO"; }

} // namespace

std::string ProbeLogPath()
{
#if defined(_WIN32)
    static const std::string p = LogDir() + "\\probe.log";
#else
    static const std::string p = LogDir() + "/probe.log";
#endif
    return p;
}

void ProbeLog(const std::string& line)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    std::ofstream f(ProbeLogPath(), std::ios::app);
    if (!f) return;

    f << Stamp() << line << "\n";
}

void ProbeOnce(const std::string& key, const std::string& line)
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_seen.count(key)) return;
        g_seen.insert(key);
    }
    ProbeLog(line);
}

namespace {

struct Timing
{
    unsigned long long calls = 0;
    double totalMs = 0.0;
    double worstMs = 0.0;
    unsigned long long nextReport = 1;
};

std::map<std::string, Timing> g_timings;

} // namespace

void ProbeTiming(const char* what, double ms)
{
    std::string line;
    {
        std::lock_guard<std::mutex> lock(g_mutex);

        Timing& t = g_timings[what];
        ++t.calls;
        t.totalMs += ms;
        if (ms > t.worstMs) t.worstMs = ms;

        if (t.calls < t.nextReport) return;
        t.nextReport *= 10;

        std::ostringstream o;
        o << "timing " << what
          << ": calls=" << t.calls
          << " total=" << t.totalMs << "ms"
          << " mean=" << (t.totalMs / static_cast<double>(t.calls)) << "ms"
          << " worst=" << t.worstMs << "ms";
        line = o.str();
    }
    ProbeLog(line);
}

ProbeScope::ProbeScope(const char* what) : _what(what)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    _start = c.QuadPart;
}

ProbeScope::~ProbeScope()
{
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    if (f.QuadPart == 0) return;
    ProbeTiming(_what, 1000.0 * static_cast<double>(c.QuadPart - _start)
                              / static_cast<double>(f.QuadPart));
}

void ProbeHostOnce()
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_hostDumped) return;
        g_hostDumped = true;
    }

    const OFX::ImageEffectHostDescription* h = OFX::getImageEffectHostDescription();
    if (!h)
    {
        ProbeLog("=== HOST PROBE === getImageEffectHostDescription() returned null");
        return;
    }

    std::ostringstream o;
    o << "\n"
      << "================= AutoMask host probe =================\n"
      << "  host name .................. " << h->hostName  << "\n"
      << "  host label ................. " << h->hostLabel << "\n"
      << "  host version ............... " << h->versionMajor << "." << h->versionMinor
                                           << "." << h->versionMicro
                                           << " (" << h->versionLabel << ")\n"
      << "  OFX API version ............ " << h->APIVersionMajor << "." << h->APIVersionMinor << "\n"
      << "  -- the parameter budget: this plugin's whole data model rests on it --\n"
      << "  maxParameters .............. " << h->maxParameters << "   (0 means unlimited)\n"
      << "  maxPages ................... " << h->maxPages << "   (0 means no tabs are available)\n"
      << "  -- things this project depends on --\n"
      << "  supportsOverlays ........... " << YesNo(h->supportsOverlays) << "\n"
      << "  DrawSuite fetched .......... " << YesNo(OFX::Private::gDrawSuite != nullptr) << "\n"
      << "  supportsCustomInteract ..... " << YesNo(h->supportsCustomInteract) << "\n"
      << "  supportsParametricParameter  " << YesNo(h->supportsParametricParameter) << "\n"
      << "  supportsCudaRender ......... " << YesNo(h->supportsCudaRender) << "\n"
      << "  supportsCudaStream ......... " << YesNo(h->supportsCudaStream) << "\n"
      << "  supportsOpenCLRender ....... " << YesNo(h->supportsOpenCLRender) << "\n"
      << "  -- secondary --\n"
      << "  supportsMultiResolution .... " << YesNo(h->supportsMultiResolution) << "\n"
      << "  supportsTiles .............. " << YesNo(h->supportsTiles) << "\n"
      << "  temporalClipAccess ......... " << YesNo(h->temporalClipAccess) << "\n"
      << "  supportsBooleanAnimation ... " << YesNo(h->supportsBooleanAnimation) << "\n"
      << "  supportsChoiceAnimation .... " << YesNo(h->supportsChoiceAnimation) << "\n"
      << "  nativeOrigin ............... " << static_cast<int>(h->nativeOrigin) << "\n"
      << "========================================================";

    ProbeLog(o.str());
}

} // namespace am

