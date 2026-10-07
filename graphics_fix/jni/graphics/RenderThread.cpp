#include "RenderThread.h"

#include <android/log.h>
#include <dlfcn.h>
#include <sys/types.h>
#include <unistd.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#define RT_LOG(...) __android_log_print(ANDROID_LOG_INFO, "GfxRenderThread", __VA_ARGS__)

namespace GraphicsRenderThread {
namespace {

// RenderQueue layout of libGTASA 2.10 arm64 (DWARF of RenderQueue, size 0x430).
struct GtaRenderQueue {
    using Command = void (*)(char*& data);
    Command commands[50];          // 0x000
    int32_t commandSizes[50];      // 0x190
    const char* commandNames[50];  // 0x258
    bool multiThread;              // 0x3E8
    bool useMutex;                 // 0x3E9
    void* commandMutex;            // 0x3F0
    char* queueStart;              // 0x3F8
    char* queueEnd;                // 0x400
    char* renderPointer;           // 0x408
    bool flushQueue;               // 0x410
    bool finishQueue;              // 0x411
    char* mainPointer;             // 0x418
    char* mainWorkPointer;         // 0x420
    int32_t curQueueingCommand;    // 0x428
};
#if defined(__aarch64__)
static_assert(offsetof(GtaRenderQueue, commandSizes) == 0x190, "RenderQueue layout");
static_assert(offsetof(GtaRenderQueue, commandNames) == 0x258, "RenderQueue layout");
static_assert(offsetof(GtaRenderQueue, multiThread) == 0x3E8, "RenderQueue layout");
static_assert(offsetof(GtaRenderQueue, commandMutex) == 0x3F0, "RenderQueue layout");
static_assert(offsetof(GtaRenderQueue, queueStart) == 0x3F8, "RenderQueue layout");
static_assert(offsetof(GtaRenderQueue, mainPointer) == 0x418, "RenderQueue layout");
static_assert(offsetof(GtaRenderQueue, mainWorkPointer) == 0x420, "RenderQueue layout");
static_assert(offsetof(GtaRenderQueue, curQueueingCommand) == 0x428, "RenderQueue layout");
static_assert(sizeof(GtaRenderQueue) == 0x430, "RenderQueue layout");
#endif

constexpr int kCallbackCommand = 47; // rqDebugMarker, never queued by the game
constexpr const char* kCallbackName = "rqClientGraphicsCallback";

using QueueFn = void (*)(GtaRenderQueue*);
using MutexFn = void (*)(void*);

struct Symbols {
    GtaRenderQueue** queue = nullptr; // "renderQueue"
    QueueFn flush = nullptr;          // RenderQueue::Flush()
    QueueFn process = nullptr;        // RenderQueue::Process()
    MutexFn obtain = nullptr;         // OS_MutexObtain(void*)
    MutexFn release = nullptr;        // OS_MutexRelease(void*)
    GtaRenderQueue::Command marker = nullptr; // RQ_Command_rqDebugMarker(char*&)
};

Symbols g_sym;
std::atomic<bool> g_ready{false};
std::atomic<bool> g_failed{false};
std::atomic<pid_t> g_gameThread{0};
std::atomic<pid_t> g_renderThread{0};
GtaRenderQueue* g_queue = nullptr;

void CallbackCommand(char*& data) {
    Callback fn = nullptr;
    void* arg = nullptr;
    std::memcpy(&fn, data, sizeof(fn));
    std::memcpy(&arg, data + sizeof(fn), sizeof(arg));
    data += sizeof(fn) + sizeof(arg);
    if (g_renderThread.load(std::memory_order_relaxed) == 0)
        g_renderThread.store(gettid(), std::memory_order_relaxed);
    if (fn) fn(arg);
}

bool ResolveSymbols() {
    void* gta = dlopen("libGTASA.so", RTLD_NOW | RTLD_NOLOAD);
    if (!gta) return false;
    g_sym.queue = reinterpret_cast<GtaRenderQueue**>(dlsym(gta, "renderQueue"));
    g_sym.flush = reinterpret_cast<QueueFn>(dlsym(gta, "_ZN11RenderQueue5FlushEv"));
    g_sym.process = reinterpret_cast<QueueFn>(dlsym(gta, "_ZN11RenderQueue7ProcessEv"));
    g_sym.obtain = reinterpret_cast<MutexFn>(dlsym(gta, "_Z14OS_MutexObtainPv"));
    g_sym.release = reinterpret_cast<MutexFn>(dlsym(gta, "_Z15OS_MutexReleasePv"));
    g_sym.marker = reinterpret_cast<GtaRenderQueue::Command>(dlsym(gta, "_Z24RQ_Command_rqDebugMarkerRPc"));
    // The handle stays referenced on purpose: libGTASA lives as long as the process.
    return g_sym.queue && g_sym.flush && g_sym.process && g_sym.obtain && g_sym.release && g_sym.marker;
}

bool QueueLooksSane(const GtaRenderQueue* q) {
    if (!q || !q->queueStart || !q->queueEnd || q->queueEnd <= q->queueStart) return false;
    if (q->mainPointer < q->queueStart || q->mainPointer > q->queueEnd) return false;
    if (q->mainWorkPointer < q->queueStart || q->mainWorkPointer > q->queueEnd) return false;
    return static_cast<size_t>(q->queueEnd - q->queueStart) >= 64 * 1024;
}

void Commit(GtaRenderQueue* q) {
    if (q->useMutex) g_sym.obtain(q->commandMutex);
    const ptrdiff_t delta = q->mainWorkPointer - q->mainPointer;
    __atomic_fetch_add(&q->mainPointer, delta, __ATOMIC_ACQ_REL);
    if (q->useMutex) g_sym.release(q->commandMutex);
    if (!q->multiThread) g_sym.process(q);
    if (q->mainPointer + 0x400 > q->queueEnd) g_sym.flush(q);
}

} // namespace

bool Install() {
    if (g_ready.load(std::memory_order_acquire)) return true;
    if (g_failed.load(std::memory_order_relaxed)) return false;
    static bool resolved = false;
    if (!resolved) {
        if (!ResolveSymbols()) {
            RT_LOG("render-thread bridge unavailable: libGTASA symbols missing");
            g_failed = true;
            return false;
        }
        resolved = true;
    }
    GtaRenderQueue* q = *g_sym.queue;
    if (!q) return false; // RenderQueue::Initialize() has not run yet
    if (!QueueLooksSane(q)) {
        // The queue lives for the whole process (RenderQueue::Initialize runs
        // once): a reading taken mid-update is retried on the next frames.
        static int insane = 0;
        if (++insane < 300) return false;
        RT_LOG("render-thread bridge NOT installed: queue pointers out of range");
        g_failed = true;
        return false;
    }
    GtaRenderQueue::Command current = q->commands[kCallbackCommand];
    if (current != g_sym.marker && current != &CallbackCommand) {
        RT_LOG("render-thread bridge NOT installed: commands[47] is not rqDebugMarker");
        g_failed = true;
        return false;
    }
    g_queue = q;
    if (current == g_sym.marker) {
        __atomic_store_n(&q->commands[kCallbackCommand], &CallbackCommand, __ATOMIC_RELEASE);
        q->commandNames[kCallbackCommand] = kCallbackName;
    }
    g_gameThread.store(gettid(), std::memory_order_relaxed);
    g_ready.store(true, std::memory_order_release);
    RT_LOG("render-thread bridge ready: queue=%p multiThread=%d", static_cast<void*>(q), q->multiThread);
    return true;
}

bool Ready() { return g_ready.load(std::memory_order_acquire); }

bool OnRenderThread() {
    const pid_t t = g_renderThread.load(std::memory_order_relaxed);
    return t != 0 && t == gettid();
}

bool Enqueue(Callback fn, void* arg) {
    if (!fn || !Ready()) return false;
    const pid_t producer = g_gameThread.load(std::memory_order_relaxed);
    if (producer != gettid()) return false; // only the thread that writes RQ commands may append
    GtaRenderQueue* q = g_queue;
    constexpr size_t kPayload = 4 + sizeof(fn) + sizeof(arg);
    // Same pre-check as ES2Shader::Select (0x2625AC): make room before writing.
    if (q->mainPointer + 0x400 + kPayload > q->queueEnd) g_sym.flush(q);
    // A command is half-written (called from inside another command) or the
    // queue is full: never interleave with it.
    if (q->mainWorkPointer != q->mainPointer || q->mainWorkPointer + kPayload > q->queueEnd) return false;

    q->curQueueingCommand = kCallbackCommand;
    char* w = q->mainWorkPointer;
    const uint32_t id = kCallbackCommand;
    std::memcpy(w, &id, 4);
    std::memcpy(w + 4, &fn, sizeof(fn));
    std::memcpy(w + 4 + sizeof(fn), &arg, sizeof(arg));
    q->mainWorkPointer = w + kPayload;
    Commit(q);
    return true;
}

} // namespace GraphicsRenderThread
