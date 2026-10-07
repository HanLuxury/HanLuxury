#include "RenderQueueBridge.h"
#include "GameRenderBridge.h"
#include "GraphicsLog.h"

#include <sys/types.h>
#include <unistd.h>
#include <atomic>
#include <cstring>

namespace gfx {
namespace {

constexpr char kTag[] = "RenderQueue";
constexpr const char* kCallbackName = "rqEagleGfxCallback";

std::atomic<bool> g_ready{false};
std::atomic<pid_t> g_gameThread{0};
std::atomic<pid_t> g_renderThread{0};
std::atomic<RenderQueueBridge::SelectObserver> g_selectObserver{nullptr};
GtaRenderQueue::Command g_origSelectShader = nullptr;
GtaRenderQueue* g_queue = nullptr;

void CallbackCommand(char*& data) {
    RenderQueueBridge::Callback fn = nullptr;
    void* arg = nullptr;
    std::memcpy(&fn, data, sizeof(fn));
    std::memcpy(&arg, data + sizeof(fn), sizeof(arg));
    data += sizeof(fn) + sizeof(arg);
    if (g_renderThread.load(std::memory_order_relaxed) == 0)
        g_renderThread.store(gettid(), std::memory_order_relaxed);
    if (fn) fn(arg);
}

void SelectShaderCommand(char*& data) {
    void* shader = nullptr;
    std::memcpy(&shader, data, sizeof(shader)); // first payload field: ES2Shader*
    g_origSelectShader(data);
    if (auto observer = g_selectObserver.load(std::memory_order_acquire)) observer(shader);
}

bool QueueLooksSane(const GtaRenderQueue* q) {
    if (!q || !q->queueStart || !q->queueEnd || q->queueEnd <= q->queueStart) return false;
    if (q->mainPointer < q->queueStart || q->mainPointer > q->queueEnd) return false;
    if (q->mainWorkPointer < q->queueStart || q->mainWorkPointer > q->queueEnd) return false;
    if (static_cast<size_t>(q->queueEnd - q->queueStart) < 64 * 1024) return false;
    return true;
}

// Mirrors the commit sequence that every inlined RQ command uses
// (e.g. RQRenderTarget::Clear at 0x2698E0): atomic add of the work delta to
// mainPointer under the command mutex, then Process() in single-thread mode
// and Flush() when fewer than 0x400 bytes remain.
void Commit(GtaRenderQueue* q) {
    if (q->useMutex) GameRenderBridge::MutexObtain(q->commandMutex);
    const ptrdiff_t delta = q->mainWorkPointer - q->mainPointer;
    __atomic_fetch_add(&q->mainPointer, delta, __ATOMIC_ACQ_REL);
    if (q->useMutex) GameRenderBridge::MutexRelease(q->commandMutex);
    if (!q->multiThread) GameRenderBridge::RenderQueueProcess(q);
    if (q->mainPointer + 0x400 > q->queueEnd) GameRenderBridge::RenderQueueFlush(q);
}

} // namespace

bool RenderQueueBridge::Install() {
    if (g_ready.load(std::memory_order_acquire)) return true;
    if (!GameRenderBridge::Init()) return false;

    GtaRenderQueue* q = GameRenderBridge::RenderQueue();
    if (!q) return false; // RenderQueue::Initialize() has not run yet

    static bool reportedInvalid = false;
    auto fail = [&](const char* why) {
        if (!reportedInvalid) GFX_LOGE(kTag, "bridge NOT installed: %s (graphics engine stays off)", why);
        reportedInvalid = true;
        return false;
    };

    if (!QueueLooksSane(q)) return fail("queue pointers out of range");

    auto expectedSelect = reinterpret_cast<GtaRenderQueue::Command>(
        GameRenderBridge::Symbol("_Z25RQ_Command_rqSelectShaderRPc"));
    auto expectedMarker = reinterpret_cast<GtaRenderQueue::Command>(
        GameRenderBridge::Symbol("_Z24RQ_Command_rqDebugMarkerRPc"));
    if (!expectedSelect || !expectedMarker) return fail("RQ handler symbols missing");

    GtaRenderQueue::Command select = q->commands[kSelectShaderCommand];
    GtaRenderQueue::Command marker = q->commands[kCallbackCommand];
    if (select != expectedSelect && select != &SelectShaderCommand)
        return fail("commands[17] is not rqSelectShader (table hooked by someone else?)");
    if (marker != expectedMarker && marker != &CallbackCommand)
        return fail("commands[47] is not rqDebugMarker");

    g_queue = q;
    if (select == expectedSelect) {
        g_origSelectShader = expectedSelect;
        __atomic_store_n(&q->commands[kSelectShaderCommand], &SelectShaderCommand, __ATOMIC_RELEASE);
    }
    if (marker == expectedMarker) {
        __atomic_store_n(&q->commands[kCallbackCommand], &CallbackCommand, __ATOMIC_RELEASE);
        q->commandNames[kCallbackCommand] = kCallbackName;
    }

    g_gameThread.store(gettid(), std::memory_order_relaxed);
    g_ready.store(true, std::memory_order_release);
    GFX_LOGI(kTag, "bridge installed: queue=%p size=%zu KiB multiThread=%d (slot 47 callback, slot 17 observer)",
             static_cast<void*>(q), static_cast<size_t>(q->queueEnd - q->queueStart) / 1024, q->multiThread);
    return true;
}

bool RenderQueueBridge::IsReady() { return g_ready.load(std::memory_order_acquire); }

bool RenderQueueBridge::OnRenderThread() {
    const pid_t t = g_renderThread.load(std::memory_order_relaxed);
    return t != 0 && t == gettid();
}

bool RenderQueueBridge::OnGameThread() {
    const pid_t t = g_gameThread.load(std::memory_order_relaxed);
    return t != 0 && t == gettid();
}

void RenderQueueBridge::SetSelectObserver(SelectObserver observer) {
    g_selectObserver.store(observer, std::memory_order_release);
}

bool RenderQueueBridge::Enqueue(Callback fn, void* arg) {
    if (!fn || !IsReady()) return false;
    if (!OnGameThread()) {
        if (GraphicsLog::Once("rq-enqueue-thread"))
            GFX_LOGE(kTag, "Enqueue refused: called from tid %d, RQ producer is tid %d", static_cast<int>(gettid()),
                     static_cast<int>(g_gameThread.load()));
        return false;
    }
    GtaRenderQueue* q = g_queue;
    constexpr size_t kPayload = 4 + sizeof(fn) + sizeof(arg);

    // Same pre-check as ES2Shader::Select (0x2625AC): make room before writing.
    if (q->mainPointer + 0x400 + kPayload > q->queueEnd) GameRenderBridge::RenderQueueFlush(q);
    if (q->mainWorkPointer != q->mainPointer || q->mainWorkPointer + kPayload > q->queueEnd) {
        // A command is half-written (we were called from inside a command) or
        // the queue is full: never interleave with it.
        if (GraphicsLog::Once("rq-enqueue-busy")) GFX_LOGE(kTag, "Enqueue refused: queue busy/full");
        return false;
    }

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

} // namespace gfx
