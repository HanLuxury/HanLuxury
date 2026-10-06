#include "GraphicsLog.h"

#include <android/log.h>
#include <sys/stat.h>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>

namespace gfx {
namespace {

constexpr long kMaxFileBytes = 4L * 1024L * 1024L; // stop writing the file after 4 MiB
constexpr int kOnceSlots = 512;

std::mutex g_mutex;
FILE* g_file = nullptr;
long g_written = 0;
bool g_capped = false;
std::atomic<bool> g_debug{false};
uint64_t g_onceKeys[kOnceSlots]{};
int g_onceCount = 0;

// mkdir -p for the directory part of 'path'.
void MakeParentDirs(const char* path) {
    char buffer[512];
    const size_t len = std::strlen(path);
    if (len == 0 || len >= sizeof(buffer)) return;
    std::memcpy(buffer, path, len + 1);
    for (size_t i = 1; i < len; ++i) {
        if (buffer[i] != '/') continue;
        buffer[i] = '\0';
        if (mkdir(buffer, 0775) != 0 && errno != EEXIST) {
            // Not fatal: the open below reports the real failure.
        }
        buffer[i] = '/';
    }
}

uint64_t Hash(const char* s) {
    uint64_t h = 1469598103934665603ULL; // FNV-1a
    for (; s && *s; ++s) h = (h ^ static_cast<uint8_t>(*s)) * 1099511628211ULL;
    return h ? h : 1;
}

int ToAndroid(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return ANDROID_LOG_DEBUG;
        case LogLevel::Info:  return ANDROID_LOG_INFO;
        case LogLevel::Warn:  return ANDROID_LOG_WARN;
        default:              return ANDROID_LOG_ERROR;
    }
}

const char* LevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "";
        case LogLevel::Warn:  return " WARN";
        default:              return " ERROR";
    }
}

} // namespace

void GraphicsLog::Init(const char* path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) return;
    if (!path) return;
    MakeParentDirs(path);
    g_file = std::fopen(path, "w");
    g_written = 0;
    g_capped = false;
    if (!g_file) {
        __android_log_print(ANDROID_LOG_WARN, "EagleGFX", "graphics.log unavailable (%s): %s",
                            path, std::strerror(errno));
    }
}

void GraphicsLog::Shutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) {
        std::fclose(g_file);
        g_file = nullptr;
    }
}

void GraphicsLog::SetDebugEnabled(bool enabled) { g_debug.store(enabled, std::memory_order_relaxed); }

bool GraphicsLog::DebugEnabled() { return g_debug.load(std::memory_order_relaxed); }

bool GraphicsLog::Once(const char* key) {
    const uint64_t h = Hash(key);
    std::lock_guard<std::mutex> lock(g_mutex);
    for (int i = 0; i < g_onceCount; ++i)
        if (g_onceKeys[i] == h) return false;
    if (g_onceCount < kOnceSlots) g_onceKeys[g_onceCount++] = h;
    return true;
}

void GraphicsLog::Write(LogLevel level, const char* tag, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    WriteV(level, tag, fmt, args);
    va_end(args);
}

void GraphicsLog::WriteV(LogLevel level, const char* tag, const char* fmt, va_list args) {
    if (level == LogLevel::Debug && !DebugEnabled()) return;

    char message[1024];
    std::vsnprintf(message, sizeof(message), fmt ? fmt : "", args);
    if (!tag) tag = "Graphics";

    __android_log_print(ToAndroid(level), "EagleGFX", "[%s] %s", tag, message);

    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_file || g_capped) return;

    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    tm local{};
    const time_t seconds = ts.tv_sec;
    localtime_r(&seconds, &local);

    const int n = std::fprintf(g_file, "%02d:%02d:%02d.%03ld [%s%s] %s\n", local.tm_hour, local.tm_min,
                               local.tm_sec, static_cast<long>(ts.tv_nsec / 1000000L), tag,
                               LevelName(level), message);
    if (n > 0) g_written += n;
    std::fflush(g_file);
    if (g_written > kMaxFileBytes) {
        std::fputs("[Graphics] log size limit reached, further file output disabled\n", g_file);
        std::fflush(g_file);
        g_capped = true;
    }
}

} // namespace gfx
