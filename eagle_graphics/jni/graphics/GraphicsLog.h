#pragma once
// EAGLE graphics engine - logging to logcat (tag "EagleGFX") and to
// /storage/emulated/0/TESTLIT/graphics/logOutput.log.
//
// Thread-safe: used from the game thread, the RenderQueue (GL) thread and JNI.
// Never throws, never aborts. If the file cannot be opened only logcat is used.

#include <cstdarg>
#include <cstdint>

namespace gfx {

enum class LogLevel : uint8_t { Debug, Info, Warn, Error };

class GraphicsLog {
public:
    // Opens (truncates) the log file. Safe to call more than once.
    static void Init(const char* path);
    static void Shutdown();

    static void Write(LogLevel level, const char* tag, const char* fmt, ...)
        __attribute__((format(printf, 3, 4)));
    static void WriteV(LogLevel level, const char* tag, const char* fmt, va_list args);

    // Debug-level lines are dropped unless enabled ([debug] log=1).
    static void SetDebugEnabled(bool enabled);
    static bool DebugEnabled();

    // Returns true only the first time a given key is seen (per session).
    // Use it for errors that could otherwise repeat every frame.
    static bool Once(const char* key);
};

} // namespace gfx

#define GFX_LOGD(tag, ...) ::gfx::GraphicsLog::Write(::gfx::LogLevel::Debug, tag, __VA_ARGS__)
#define GFX_LOGI(tag, ...) ::gfx::GraphicsLog::Write(::gfx::LogLevel::Info,  tag, __VA_ARGS__)
#define GFX_LOGW(tag, ...) ::gfx::GraphicsLog::Write(::gfx::LogLevel::Warn,  tag, __VA_ARGS__)
#define GFX_LOGE(tag, ...) ::gfx::GraphicsLog::Write(::gfx::LogLevel::Error, tag, __VA_ARGS__)
