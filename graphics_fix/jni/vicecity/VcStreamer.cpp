#include "VcStreamer.h"

#include <algorithm>
#include <limits>

namespace vc {
namespace {

float DistanceSq(const float a[3], const float b[3]) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz;
}

}  // namespace

void Streamer::SetItems(std::vector<StreamItem> items) {
    m_items = std::move(items);
    m_state.assign(m_items.size(), kIdle);
    m_want.assign(m_items.size(), 0);
    m_create.clear();
    m_createNext = 0;
    m_destroy.clear();
    m_scratch.clear();
    m_scratch.reserve(m_items.size());
    m_create.reserve(m_items.size());
    m_destroy.reserve(m_items.size());
    m_lastFocus.clear();
    m_active = m_wanted = m_dropped = 0;
    m_scanned = false;
    m_dirty = true;
}

void Streamer::SetSettings(const StreamSettings& settings) {
    m_settings = settings;
    if (!(m_settings.distanceScale > 0.0f)) m_settings.distanceScale = 1.0f;
    if (!(m_settings.keepMargin >= 0.0f)) m_settings.keepMargin = 0.0f;
    if (!(m_settings.criticalDistance >= 0.0f)) m_settings.criticalDistance = 0.0f;
    if (!(m_settings.rescanMove >= 0.0f)) m_settings.rescanMove = 0.0f;
    if (m_settings.maxDestroyPerTick == 0) m_settings.maxDestroyPerTick = 1;
    m_dirty = true;
}

void Streamer::SetUsable(uint32_t index, bool usable) {
    if (index >= m_items.size() || m_items[index].usable == usable) return;
    m_items[index].usable = usable;
    m_dirty = true;
}

void Streamer::Lost(uint32_t index) {
    if (index >= m_state.size() || m_state[index] != kActive) return;
    m_state[index] = kIdle;
    --m_active;
    m_dirty = true;
}

bool Streamer::NeedScan(const StreamFocus* focus, size_t focusCount, uint64_t nowUs) const {
    if (!m_scanned || m_dirty || focusCount != m_lastFocus.size()) return true;
    if (nowUs - m_lastScanUs >= static_cast<uint64_t>(m_settings.rescanMs) * 1000u) return true;
    const float moveSq = m_settings.rescanMove * m_settings.rescanMove;
    for (size_t i = 0; i < focusCount; ++i) {
        if (DistanceSq(focus[i].pos, m_lastFocus[i].pos) >= moveSq && moveSq > 0.0f) return true;
    }
    return false;
}

void Streamer::Scan(const StreamFocus* focus, size_t focusCount) {
    // Most urgent first: what the player could touch, then the more important type, then the nearer one.
    const auto moreUrgent = [](const Candidate& a, const Candidate& b) {
        if (a.critical != b.critical) return a.critical;
        if (a.priority != b.priority) return a.priority > b.priority;
        if (a.distanceSq != b.distanceSq) return a.distanceSq < b.distanceSq;
        return a.index < b.index;
    };

    m_create.clear();
    m_createNext = 0;
    m_destroy.clear();
    m_scratch.clear();

    const float scale = m_settings.distanceScale;
    for (size_t i = 0; i < m_items.size(); ++i) {
        const StreamItem& item = m_items[i];
        m_want[i] = 0;
        if (!item.usable || focusCount == 0) continue;
        float nearest = std::numeric_limits<float>::max();
        for (size_t f = 0; f < focusCount; ++f) nearest = std::min(nearest, DistanceSq(focus[f].pos, item.pos));
        // An object the focus point is standing on or next to has to exist whatever its type says.
        const float criticalRange = item.reach + m_settings.criticalDistance;
        float range = std::max(item.streamDistance * scale, criticalRange);
        if (m_state[i] == kActive) range += m_settings.keepMargin;
        if (!(nearest <= range * range)) continue;
        m_want[i] = 1;
        m_scratch.push_back({static_cast<uint32_t>(i), nearest, item.priority, nearest <= criticalRange * criticalRange});
    }

    // Not everything fits: the most urgent ones stay.
    m_dropped = 0;
    if (m_scratch.size() > m_settings.maxActive) {
        std::nth_element(m_scratch.begin(), m_scratch.begin() + static_cast<std::ptrdiff_t>(m_settings.maxActive),
                         m_scratch.end(), moreUrgent);
        for (size_t k = m_settings.maxActive; k < m_scratch.size(); ++k) m_want[m_scratch[k].index] = 0;
        m_dropped = m_scratch.size() - m_settings.maxActive;
        m_scratch.resize(m_settings.maxActive);
    }
    m_wanted = m_scratch.size();

    for (const Candidate& c : m_scratch) {
        if (m_state[c.index] == kIdle) m_create.push_back(c);
    }
    std::sort(m_create.begin(), m_create.end(), moreUrgent);
    for (size_t i = 0; i < m_items.size(); ++i) {
        if (m_state[i] == kActive && !m_want[i]) m_destroy.push_back(static_cast<uint32_t>(i));
    }

    m_lastFocus.assign(focus, focus + focusCount);
    m_scanned = true;
    m_dirty = false;
}

void Streamer::Tick(const StreamFocus* focus, size_t focusCount, StreamHost& host) {
    if (!focus) focusCount = 0;
    const uint64_t now = host.NowUs();
    if (NeedScan(focus, focusCount, now)) {
        Scan(focus, focusCount);
        m_lastScanUs = now;
    }

    size_t destroyed = 0;
    while (!m_destroy.empty() && destroyed < m_settings.maxDestroyPerTick) {
        const uint32_t index = m_destroy.back();
        m_destroy.pop_back();
        if (m_state[index] != kActive || m_want[index]) continue;
        host.Destroy(index);
        m_state[index] = kIdle;
        --m_active;
        ++m_destroyedTotal;
        ++destroyed;
    }

    const uint64_t start = host.NowUs();
    size_t made = 0;
    while (m_createNext < m_create.size()) {
        const Candidate c = m_create[m_createNext];
        if (!c.critical) {
            // Objects on their way out still take their place: let them go first.
            if (m_active >= m_settings.maxActive) break;
            // Always one per tick, so a clock that jumps cannot stop the map from appearing.
            if (made > 0 && host.NowUs() - start >= m_settings.budgetUs) break;
        }
        ++m_createNext;
        if (m_state[c.index] != kIdle || !m_want[c.index] || !m_items[c.index].usable) continue;
        switch (host.Create(c.index)) {
            case StreamHost::Result::Done:
                m_state[c.index] = kActive;
                ++m_active;
                ++m_createdTotal;
                ++made;
                break;
            case StreamHost::Result::Later:
                break;   // the next scan puts it back in the queue
            case StreamHost::Result::Never:
                m_items[c.index].usable = false;
                m_want[c.index] = 0;
                break;
        }
    }
}

void Streamer::Clear(StreamHost& host) {
    for (size_t i = 0; i < m_items.size(); ++i) {
        if (m_state[i] != kActive) continue;
        host.Destroy(static_cast<uint32_t>(i));
        m_state[i] = kIdle;
        ++m_destroyedTotal;
    }
    Forget();
}

void Streamer::Forget() {
    std::fill(m_state.begin(), m_state.end(), static_cast<uint8_t>(kIdle));
    std::fill(m_want.begin(), m_want.end(), static_cast<uint8_t>(0));
    m_create.clear();
    m_createNext = 0;
    m_destroy.clear();
    m_active = m_wanted = m_dropped = 0;
    m_scanned = false;
    m_dirty = true;
}

}  // namespace vc
