#pragma once
// Decides which objects of the map exist at the moment: the ones near the player and the camera. The client
// places the map itself (a 0.3.7 server cannot hand out enough objects for it), so this takes the place of
// the streamer plugin the original script relies on.
//
// No game dependencies: objects are created and destroyed through StreamHost, time comes from it too.
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vc {

struct StreamItem {
    float pos[3] = {};
    float streamDistance = 0.0f;   // created when a focus point comes this close to the object's origin
    float reach = 0.0f;            // how far the object extends from its origin
    uint8_t priority = 0;          // higher first when not everything fits or not everything can be made at once
    bool usable = true;            // false: never created (model or file missing)
};

struct StreamFocus {
    float pos[3] = {};
};

struct StreamSettings {
    float distanceScale = 1.0f;       // multiplies every streamDistance
    float keepMargin = 30.0f;         // an object is kept until it is this much farther away than its create distance
    float criticalDistance = 40.0f;   // objects that reach this close to a focus point are created at once
    size_t maxActive = 2500;          // objects that may exist at the same time
    uint32_t rescanMs = 250;          // how often the whole map is looked at ...
    float rescanMove = 8.0f;          // ... or sooner, when a focus point has moved this far
    uint32_t budgetUs = 4000;         // time per tick for objects that are not critical
    size_t maxDestroyPerTick = 256;
};

class StreamHost {
public:
    enum class Result {
        Done,    // the object exists now
        Later,   // not now (a pool is full): asked again after the next scan
        Never    // cannot be created at all: never asked again
    };
    virtual ~StreamHost() = default;
    virtual Result Create(uint32_t index) = 0;
    virtual void Destroy(uint32_t index) = 0;
    virtual uint64_t NowUs() = 0;   // monotonic
};

class Streamer {
public:
    // Replaces the map. Objects that still exist must have been cleared before.
    void SetItems(std::vector<StreamItem> items);
    void SetSettings(const StreamSettings& settings);
    const StreamSettings& settings() const { return m_settings; }

    // One step. `focus`: where the player is, and the camera when it is somewhere else. Without a focus
    // point nothing is wanted.
    void Tick(const StreamFocus* focus, size_t focusCount, StreamHost& host);
    // Destroys every object (the map is switched off).
    void Clear(StreamHost& host);
    // The objects are gone without having been destroyed through the host (the game cleared its pools).
    void Forget();
    // Marks an object as impossible to create from outside the host callbacks.
    void SetUsable(uint32_t index, bool usable);
    // One object is gone without having been destroyed through the host: it is created again when wanted.
    void Lost(uint32_t index);

    bool IsActive(uint32_t index) const { return index < m_state.size() && m_state[index] == kActive; }
    size_t itemCount() const { return m_items.size(); }
    size_t activeCount() const { return m_active; }
    size_t wantedCount() const { return m_wanted; }       // after the last scan
    size_t pendingCount() const { return m_create.size() - m_createNext; }
    size_t droppedCount() const { return m_dropped; }     // wanted but over maxActive, at the last scan
    uint64_t createdTotal() const { return m_createdTotal; }
    uint64_t destroyedTotal() const { return m_destroyedTotal; }

private:
    enum State : uint8_t { kIdle, kActive };
    struct Candidate {
        uint32_t index;
        float distanceSq;
        uint8_t priority;
        bool critical;
    };
    void Scan(const StreamFocus* focus, size_t focusCount);
    bool NeedScan(const StreamFocus* focus, size_t focusCount, uint64_t nowUs) const;

    StreamSettings m_settings;
    std::vector<StreamItem> m_items;
    std::vector<uint8_t> m_state;
    std::vector<uint8_t> m_want;         // decided by the last scan
    std::vector<Candidate> m_create;     // wanted and not there, most urgent first
    size_t m_createNext = 0;
    std::vector<uint32_t> m_destroy;     // there and not wanted
    std::vector<Candidate> m_scratch;
    std::vector<StreamFocus> m_lastFocus;
    uint64_t m_lastScanUs = 0;
    bool m_scanned = false;
    bool m_dirty = true;
    size_t m_active = 0;
    size_t m_wanted = 0;
    size_t m_dropped = 0;
    uint64_t m_createdTotal = 0;
    uint64_t m_destroyedTotal = 0;
};

}  // namespace vc
