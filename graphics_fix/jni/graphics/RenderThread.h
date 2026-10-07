#pragma once
// Runs the client's existing graphics work (WorldSunShadow, EglPostFX) on
// GTA's render thread, in order with the game's own draw commands.
//
// libGTASA 2.10 arm64 (verified in the binary):
//   RenderQueue::RenderQueue (0x266F94) stores 0x101 at +0x3E8, so multiThread
//   and useMutex are always 1. RenderQueue::Initialize then releases the EGL
//   context on the game thread and starts GraphicsThread, which owns it. Every
//   GL call of the game runs there; Idle(), RenderScene() and Render2dStuff()
//   run on the game thread and only append commands to the queue. GL calls
//   made directly from Render2dStuff have no current context.
//
//   Command 47 (rqDebugMarker) is never queued by the game. Its table entry is
//   replaced by a "call fn(arg) on the render thread" command after both table
//   entries were checked against the exported handler symbols. The commit
//   sequence is the one every inlined RQ command uses (RQRenderTarget::Clear
//   at 0x2698E0): atomic add under the command mutex, Process() when single
//   threaded, Flush() when fewer than 0x400 bytes are left.

namespace GraphicsRenderThread {

using Callback = void (*)(void* arg);

// Game thread. Cheap after the first success; false until RenderQueue exists
// or when the table does not look like libGTASA 2.10 (nothing is patched then).
bool Install();
bool Ready();

// Game thread only: fn(arg) runs later on the render thread, after every
// command queued before it and before every command queued after it.
bool Enqueue(Callback fn, void* arg);

// True on the thread that executes RenderQueue commands (known after the
// first callback ran).
bool OnRenderThread();

} // namespace GraphicsRenderThread
