// VcStreamer: which objects exist for a given player position, in which order they are made, and what happens
// when the game cannot make one.
#include "vctest.h"

#include <algorithm>
#include <limits>
#include <set>

#include "../VcStreamer.h"

using namespace vc;

namespace {

class FakeHost : public StreamHost {
public:
    Result Create(uint32_t index) override {
        nowUs += createCostUs;
        if (later.count(index)) return Result::Later;
        if (never.count(index)) return Result::Never;
        if (alive.count(index)) ++doubleCreates;
        alive.insert(index);
        createOrder.push_back(index);
        return Result::Done;
    }
    void Destroy(uint32_t index) override {
        if (!alive.count(index)) ++badDestroys;
        alive.erase(index);
        destroyOrder.push_back(index);
    }
    uint64_t NowUs() override { return nowUs; }

    uint64_t nowUs = 1000000;
    uint64_t createCostUs = 0;
    std::set<uint32_t> alive, later, never;
    std::vector<uint32_t> createOrder, destroyOrder;
    int doubleCreates = 0, badDestroys = 0;
};

StreamItem Item(float x, float y, float distance, uint8_t priority = 3, float reach = 1.0f) {
    StreamItem item;
    item.pos[0] = x;
    item.pos[1] = y;
    item.pos[2] = 0.0f;
    item.streamDistance = distance;
    item.priority = priority;
    item.reach = reach;
    return item;
}

StreamFocus At(float x, float y, float z = 0.0f) { return StreamFocus{{x, y, z}}; }

// Ticks until nothing is pending any more, advancing the clock so that a scan is due on each one.
void Settle(Streamer& s, FakeHost& host, const StreamFocus* focus, size_t n, int limit = 2000) {
    for (int i = 0; i < limit; ++i) {
        host.nowUs += 300000;
        s.Tick(focus, n, host);
        if (s.pendingCount() == 0 && s.activeCount() == s.wantedCount()) return;
    }
}

}  // namespace

ML_TEST(streamer_nothing_exists_without_a_focus_point) {
    Streamer s;
    s.SetItems({Item(0, 0, 100), Item(50, 0, 100)});
    FakeHost host;
    s.Tick(nullptr, 0, host);
    ML_CHECK_EQ(s.activeCount(), static_cast<size_t>(0));
    ML_CHECK_EQ(s.wantedCount(), static_cast<size_t>(0));
    ML_CHECK(host.createOrder.empty());

    // An empty map is fine too.
    Streamer empty;
    const StreamFocus f = At(0, 0);
    empty.Tick(&f, 1, host);
    ML_CHECK_EQ(empty.activeCount(), static_cast<size_t>(0));
}

ML_TEST(streamer_objects_in_range_are_created_and_far_ones_are_not) {
    Streamer s;
    s.SetItems({Item(0, 0, 100), Item(90, 0, 100), Item(150, 0, 100), Item(600, 0, 500), Item(0, 700, 500)});
    FakeHost host;
    const StreamFocus f = At(0, 0);
    Settle(s, host, &f, 1);
    ML_CHECK_EQ(host.alive, (std::set<uint32_t>{0, 1}));
    ML_CHECK_EQ(s.activeCount(), static_cast<size_t>(2));
    ML_CHECK(s.IsActive(0));
    ML_CHECK(!s.IsActive(2));
    ML_CHECK_EQ(host.doubleCreates, 0);

    // Each object has its own distance.
    const StreamFocus g = At(160, 0);
    Settle(s, host, &g, 1);
    ML_CHECK_EQ(host.alive, (std::set<uint32_t>{1, 2, 3}));
    ML_CHECK_EQ(host.badDestroys, 0);
    ML_CHECK_EQ(s.createdTotal(), static_cast<uint64_t>(4));
    ML_CHECK_EQ(s.destroyedTotal(), static_cast<uint64_t>(1));
}

ML_TEST(streamer_an_object_is_kept_a_little_longer_than_it_is_created) {
    Streamer s;
    StreamSettings settings;
    settings.keepMargin = 30.0f;
    settings.criticalDistance = 0.0f;
    s.SetSettings(settings);
    s.SetItems({Item(0, 0, 100)});
    FakeHost host;
    StreamFocus f = At(105, 0);
    Settle(s, host, &f, 1);
    ML_CHECK(host.alive.empty());          // outside the create distance
    f = At(99, 0);
    Settle(s, host, &f, 1);
    ML_CHECK_EQ(host.alive.size(), static_cast<size_t>(1));
    f = At(125, 0);
    Settle(s, host, &f, 1);
    ML_CHECK_EQ(host.alive.size(), static_cast<size_t>(1));   // inside the margin: no flicker at the border
    f = At(131, 0);
    Settle(s, host, &f, 1);
    ML_CHECK(host.alive.empty());
    ML_CHECK_EQ(host.createOrder.size(), static_cast<size_t>(1));
    ML_CHECK_EQ(host.destroyOrder.size(), static_cast<size_t>(1));
}

ML_TEST(streamer_distance_scale_applies_to_every_object) {
    Streamer s;
    StreamSettings settings;
    settings.distanceScale = 0.5f;
    settings.criticalDistance = 0.0f;
    s.SetSettings(settings);
    s.SetItems({Item(40, 0, 100), Item(60, 0, 100)});
    FakeHost host;
    const StreamFocus f = At(0, 0);
    Settle(s, host, &f, 1);
    ML_CHECK_EQ(host.alive, (std::set<uint32_t>{0}));
}

ML_TEST(streamer_what_the_player_could_touch_exists_whatever_its_type_says) {
    // A large land mass whose origin is far away, and a small prop next to it with a short distance.
    Streamer s;
    StreamSettings settings;
    settings.criticalDistance = 40.0f;
    s.SetSettings(settings);
    s.SetItems({Item(500, 0, 250, 1, 480.0f), Item(500, 0, 250, 1, 5.0f)});
    FakeHost host;
    const StreamFocus f = At(0, 0);
    Settle(s, host, &f, 1);
    ML_CHECK_EQ(host.alive, (std::set<uint32_t>{0}));   // 500 away, but it reaches to within 20
}

ML_TEST(streamer_urgent_objects_ignore_the_time_budget_and_come_first) {
    Streamer s;
    StreamSettings settings;
    settings.budgetUs = 1000;
    settings.criticalDistance = 40.0f;
    s.SetSettings(settings);
    std::vector<StreamItem> items;
    for (int i = 0; i < 50; ++i) items.push_back(Item(200.0f + static_cast<float>(i), 0, 400, 3));   // far, low priority
    for (int i = 0; i < 20; ++i) items.push_back(Item(static_cast<float>(i), 0, 400, 1));           // around the player
    items.push_back(Item(300, 0, 400, 6));                                                          // far, high priority
    s.SetItems(items);
    FakeHost host;
    host.createCostUs = 600;   // two creations use up the budget
    const StreamFocus f = At(0, 0);
    s.Tick(&f, 1, host);

    // All twenty objects next to the player exist after the very first tick, before anything else.
    ML_CHECK(host.createOrder.size() >= 20);
    for (size_t i = 0; i < 20 && i < host.createOrder.size(); ++i) ML_CHECK(host.createOrder[i] >= 50 && host.createOrder[i] < 70);
    // The budget lets only a few of the others through per tick ...
    ML_CHECK(host.createOrder.size() <= 23);
    const size_t afterFirstTick = host.createOrder.size();
    s.Tick(&f, 1, host);
    ML_CHECK(host.createOrder.size() > afterFirstTick);
    ML_CHECK(host.createOrder.size() <= afterFirstTick + 3);

    Settle(s, host, &f, 1);
    ML_CHECK_EQ(host.alive.size(), static_cast<size_t>(71));
    ML_CHECK_EQ(host.doubleCreates, 0);
    // ... and of those the more important type goes first, then the nearer one.
    ML_CHECK_EQ(host.createOrder[20], 70u);
    ML_CHECK_EQ(host.createOrder[21], 0u);
    ML_CHECK_EQ(host.createOrder[22], 1u);
}

ML_TEST(streamer_always_makes_progress_even_when_one_object_costs_more_than_the_budget) {
    Streamer s;
    StreamSettings settings;
    settings.budgetUs = 100;
    settings.criticalDistance = 0.0f;
    s.SetSettings(settings);
    s.SetItems({Item(100, 0, 400), Item(110, 0, 400), Item(120, 0, 400)});
    FakeHost host;
    host.createCostUs = 5000;
    const StreamFocus f = At(0, 0);
    s.Tick(&f, 1, host);
    ML_CHECK_EQ(host.alive.size(), static_cast<size_t>(1));
    s.Tick(&f, 1, host);
    ML_CHECK_EQ(host.alive.size(), static_cast<size_t>(2));
    s.Tick(&f, 1, host);
    ML_CHECK_EQ(host.alive.size(), static_cast<size_t>(3));
}

ML_TEST(streamer_too_many_objects_keeps_the_most_urgent) {
    Streamer s;
    StreamSettings settings;
    settings.maxActive = 5;
    settings.criticalDistance = 10.0f;
    s.SetSettings(settings);
    std::vector<StreamItem> items;
    for (int i = 0; i < 10; ++i) items.push_back(Item(100.0f + static_cast<float>(i), 0, 400, 3));   // 0..9
    items.push_back(Item(300, 0, 400, 6));                                                           // 10: important
    items.push_back(Item(2, 0, 400, 1));                                                             // 11: under the player
    s.SetItems(items);
    FakeHost host;
    const StreamFocus f = At(0, 0);
    Settle(s, host, &f, 1);
    ML_CHECK_EQ(host.alive, (std::set<uint32_t>{11, 10, 0, 1, 2}));
    ML_CHECK_EQ(s.droppedCount(), static_cast<size_t>(7));
    ML_CHECK_EQ(s.wantedCount(), static_cast<size_t>(5));

    // Moving on: the set changes, the limit holds at every moment.
    size_t most = 0;
    for (int step = 0; step < 40; ++step) {
        const StreamFocus g = At(static_cast<float>(step) * 5.0f, 0);
        host.nowUs += 300000;
        s.Tick(&g, 1, host);
        most = std::max(most, host.alive.size());
    }
    ML_CHECK(most <= 5);
    ML_CHECK_EQ(host.doubleCreates, 0);
    ML_CHECK_EQ(host.badDestroys, 0);
}

ML_TEST(streamer_an_object_that_cannot_be_made_now_is_tried_again_and_one_that_never_can_is_not) {
    Streamer s;
    s.SetItems({Item(10, 0, 100), Item(20, 0, 100), Item(30, 0, 100)});
    FakeHost host;
    host.later.insert(1);
    host.never.insert(2);
    const StreamFocus f = At(0, 0);
    for (int i = 0; i < 5; ++i) {
        host.nowUs += 300000;
        s.Tick(&f, 1, host);
    }
    ML_CHECK_EQ(host.alive, (std::set<uint32_t>{0}));
    ML_CHECK_EQ(s.activeCount(), static_cast<size_t>(1));

    host.later.clear();   // the pool has room again
    host.never.clear();   // ... but "never" was final
    for (int i = 0; i < 5; ++i) {
        host.nowUs += 300000;
        s.Tick(&f, 1, host);
    }
    ML_CHECK_EQ(host.alive, (std::set<uint32_t>{0, 1}));
}

ML_TEST(streamer_unusable_objects_are_never_created) {
    Streamer s;
    std::vector<StreamItem> items = {Item(10, 0, 100), Item(20, 0, 100)};
    items[1].usable = false;
    s.SetItems(items);
    FakeHost host;
    const StreamFocus f = At(0, 0);
    Settle(s, host, &f, 1);
    ML_CHECK_EQ(host.alive, (std::set<uint32_t>{0}));
    s.SetUsable(0, false);
    Settle(s, host, &f, 1);
    ML_CHECK(host.alive.empty());   // also taken away when it becomes unusable
    s.SetUsable(1, true);
    Settle(s, host, &f, 1);
    ML_CHECK_EQ(host.alive, (std::set<uint32_t>{1}));
    s.SetUsable(99, true);          // out of range: ignored
}

ML_TEST(streamer_two_focus_points) {
    Streamer s;
    s.SetItems({Item(0, 0, 100), Item(1000, 0, 100), Item(500, 0, 100)});
    FakeHost host;
    const StreamFocus both[2] = {At(0, 0), At(1000, 0)};
    Settle(s, host, both, 2);
    ML_CHECK_EQ(host.alive, (std::set<uint32_t>{0, 1}));
    Settle(s, host, both, 1);       // the camera has come back to the player
    ML_CHECK_EQ(host.alive, (std::set<uint32_t>{0}));
}

ML_TEST(streamer_scans_when_time_has_passed_or_the_player_has_moved) {
    Streamer s;
    StreamSettings settings;
    settings.rescanMs = 250;
    settings.rescanMove = 8.0f;
    settings.criticalDistance = 0.0f;
    s.SetSettings(settings);
    s.SetItems({Item(0, 0, 50), Item(200, 0, 50)});
    FakeHost host;
    StreamFocus f = At(0, 0);
    s.Tick(&f, 1, host);
    ML_CHECK_EQ(host.alive, (std::set<uint32_t>{0}));

    // A teleport is noticed on the very next tick, without waiting for the interval.
    f = At(200, 0);
    host.nowUs += 1000;
    s.Tick(&f, 1, host);
    ML_CHECK_EQ(host.alive, (std::set<uint32_t>{1}));

    // A small step is not worth a scan ...
    f = At(100, 0);   // 100 away from object 1: out of its range, margin included
    StreamFocus tiny = At(203, 0);
    host.nowUs += 1000;
    s.Tick(&tiny, 1, host);
    ML_CHECK_EQ(host.alive, (std::set<uint32_t>{1}));
    // ... until the interval is over.
    host.nowUs += 300000;
    s.Tick(&f, 1, host);
    ML_CHECK(host.alive.empty() || host.alive == (std::set<uint32_t>{1}));
    Settle(s, host, &f, 1);
    ML_CHECK(host.alive.empty());
}

ML_TEST(streamer_clear_forget_and_lost) {
    Streamer s;
    s.SetItems({Item(10, 0, 100), Item(20, 0, 100), Item(30, 0, 100)});
    FakeHost host;
    const StreamFocus f = At(0, 0);
    Settle(s, host, &f, 1);
    ML_CHECK_EQ(host.alive.size(), static_cast<size_t>(3));

    // One object vanished behind the streamer's back: it comes back.
    host.alive.erase(1);
    s.Lost(1);
    ML_CHECK_EQ(s.activeCount(), static_cast<size_t>(2));
    Settle(s, host, &f, 1);
    ML_CHECK_EQ(host.alive.size(), static_cast<size_t>(3));
    s.Lost(1000);   // out of range: ignored
    s.Lost(1);
    s.Lost(1);      // twice: counted once
    ML_CHECK_EQ(s.activeCount(), static_cast<size_t>(2));
    host.alive.erase(1);
    Settle(s, host, &f, 1);

    // Switching the map off destroys everything through the host.
    s.Clear(host);
    ML_CHECK(host.alive.empty());
    ML_CHECK_EQ(s.activeCount(), static_cast<size_t>(0));
    ML_CHECK_EQ(host.badDestroys, 0);

    // ... and it can come back.
    Settle(s, host, &f, 1);
    ML_CHECK_EQ(host.alive.size(), static_cast<size_t>(3));

    // The game threw its objects away itself: nothing is destroyed twice.
    const size_t destroysBefore = host.destroyOrder.size();
    host.alive.clear();
    s.Forget();
    ML_CHECK_EQ(s.activeCount(), static_cast<size_t>(0));
    ML_CHECK_EQ(host.destroyOrder.size(), destroysBefore);
    Settle(s, host, &f, 1);
    ML_CHECK_EQ(host.alive.size(), static_cast<size_t>(3));
    ML_CHECK_EQ(host.doubleCreates, 0);
}

ML_TEST(streamer_leaving_the_map_takes_everything_away_in_steps) {
    Streamer s;
    StreamSettings settings;
    settings.maxDestroyPerTick = 10;
    s.SetSettings(settings);
    std::vector<StreamItem> items;
    for (int i = 0; i < 35; ++i) items.push_back(Item(static_cast<float>(i), 0, 100));
    s.SetItems(items);
    FakeHost host;
    const StreamFocus f = At(0, 0);
    Settle(s, host, &f, 1);
    ML_CHECK_EQ(host.alive.size(), static_cast<size_t>(35));
    host.nowUs += 300000;
    s.Tick(nullptr, 0, host);
    ML_CHECK_EQ(host.alive.size(), static_cast<size_t>(25));
    s.Tick(nullptr, 0, host);
    s.Tick(nullptr, 0, host);
    s.Tick(nullptr, 0, host);
    ML_CHECK(host.alive.empty());
    ML_CHECK_EQ(host.badDestroys, 0);
}

ML_TEST(streamer_odd_settings_are_made_usable) {
    Streamer s;
    StreamSettings settings;
    settings.distanceScale = 0.0f;
    settings.keepMargin = -5.0f;
    settings.criticalDistance = -1.0f;
    settings.rescanMove = -1.0f;
    settings.maxDestroyPerTick = 0;
    s.SetSettings(settings);
    ML_CHECK_EQ(s.settings().distanceScale, 1.0f);
    ML_CHECK_EQ(s.settings().keepMargin, 0.0f);
    ML_CHECK_EQ(s.settings().criticalDistance, 0.0f);
    ML_CHECK_EQ(s.settings().maxDestroyPerTick, static_cast<size_t>(1));
    s.SetItems({Item(10, 0, 100)});
    FakeHost host;
    const StreamFocus f = At(0, 0);
    Settle(s, host, &f, 1);
    ML_CHECK_EQ(host.alive.size(), static_cast<size_t>(1));

    // Positions that are not numbers want nothing and break nothing.
    const StreamFocus nan = At(std::numeric_limits<float>::quiet_NaN(), 0);
    Settle(s, host, &nan, 1);
    ML_CHECK(host.alive.empty());
}
