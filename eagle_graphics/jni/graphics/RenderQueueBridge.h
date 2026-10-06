#pragma once
// EAGLE graphics engine - bridge to GTA's RenderQueue (libGTASA 2.10 arm64).
//
// Verified in libGTASA.so (DWARF + disassembly):
//   * RenderQueue::RenderQueue() sets multiThread = useMutex = 1 (strh 0x101 at
//     +0x3E8), RenderQueue::Initialize() then launches GraphicsThread which
//     makes the EGL context current. ALL GL calls of the game run on that
//     thread; the game thread only writes commands. Direct GL calls from a game
//     hook therefore have no context (or race with the RQ thread).
//   * Commands are dispatched through RenderQueue::commands[id] (50 entries).
//     Command 47 (rqDebugMarker -> glHint) is never enqueued by the game (no
//     store of 47 to curQueueingCommand anywhere in .text). The engine reuses
//     that slot as a "call this function on the GL thread" command, keeping GL
//     work strictly ordered with the game's own draw commands.
//   * Command 17 (rqSelectShader) inlines ES2Shader::SetActive; it is wrapped
//     in the table so the shadow receiver uniforms can be uploaded right after
//     the game binds a program.
// The original table entries are validated against the exported handler
// symbols before anything is patched; on mismatch nothing is touched.

#include <cstddef>
#include <cstdint>

namespace gfx {

struct GtaRenderQueue {
    using Command = void (*)(char*& data);

    Command commands[50];          // 0x000
    int32_t commandSizes[50];      // 0x190
    const char* commandNames[50];  // 0x258
    bool multiThread;              // 0x3E8
    bool useMutex;                 // 0x3E9
    void* commandMutex;            // 0x3F0  OSMutex
    char* queueStart;              // 0x3F8
    char* queueEnd;                // 0x400
    char* renderPointer;           // 0x408
    bool flushQueue;               // 0x410
    bool finishQueue;              // 0x411
    char* mainPointer;             // 0x418
    char* mainWorkPointer;         // 0x420
    int32_t curQueueingCommand;    // 0x428  RQCommand
};
static_assert(offsetof(GtaRenderQueue, commandSizes) == 0x190, "RenderQueue layout");
static_assert(offsetof(GtaRenderQueue, commandNames) == 0x258, "RenderQueue layout");
static_assert(offsetof(GtaRenderQueue, multiThread) == 0x3E8, "RenderQueue layout");
static_assert(offsetof(GtaRenderQueue, commandMutex) == 0x3F0, "RenderQueue layout");
static_assert(offsetof(GtaRenderQueue, queueStart) == 0x3F8, "RenderQueue layout");
static_assert(offsetof(GtaRenderQueue, renderPointer) == 0x408, "RenderQueue layout");
static_assert(offsetof(GtaRenderQueue, flushQueue) == 0x410, "RenderQueue layout");
static_assert(offsetof(GtaRenderQueue, mainPointer) == 0x418, "RenderQueue layout");
static_assert(offsetof(GtaRenderQueue, mainWorkPointer) == 0x420, "RenderQueue layout");
static_assert(offsetof(GtaRenderQueue, curQueueingCommand) == 0x428, "RenderQueue layout");
static_assert(sizeof(GtaRenderQueue) == 0x430, "RenderQueue layout");

class RenderQueueBridge {
public:
    using Callback = void (*)(void* arg);
    using SelectObserver = void (*)(void* es2Shader);

    static constexpr int kCallbackCommand = 47;     // rqDebugMarker slot
    static constexpr int kSelectShaderCommand = 17; // rqSelectShader

    // Game thread. Safe to call every frame; does the work once the RQ exists.
    static bool Install();
    static bool IsReady();

    // Game thread only (the thread that issues RenderWare/RQ commands).
    // The callback runs later on the GL thread, in command order.
    static bool Enqueue(Callback fn, void* arg);

    // Set from any thread; invoked on the GL thread after rqSelectShader.
    static void SetSelectObserver(SelectObserver observer);

    // True on the thread that executes RQ commands (known after the first callback).
    static bool OnRenderThread();
    static bool OnGameThread();
};

} // namespace gfx
