#include <renderer/SnapshotRing.h>

#include <cassert>

struct DummySnapshot {
    uint64_t frame{ 0 };
};

int main()
{
    renderer::SnapshotRing<DummySnapshot, 3> ring{};

    // Warm-up contract: 1 publish is insufficient for strict N+1 write / N read.
    const auto w1 = ring.beginWrite();
    w1.snapshot->frame = 1;
    ring.publish(w1);
    const auto warmupRead0 = ring.beginReadStaged();
    assert(!warmupRead0.has_value());

    // After the second publish, reader sees prior snapshot (epoch 1/frame 1).
    const auto w2 = ring.beginWrite();
    w2.snapshot->frame = 2;
    ring.publish(w2);

    const auto r1 = ring.beginReadStaged();
    assert(r1.has_value());
    assert(r1->readEpoch == 1);
    assert(r1->snapshot->frame == 1);
    ring.endRead(*r1);

    // Third publish advances staged read to epoch 2/frame 2.
    const auto w3 = ring.beginWrite();
    w3.snapshot->frame = 3;
    ring.publish(w3);

    const auto r2 = ring.beginReadStaged();
    assert(r2.has_value());
    assert(r2->readEpoch == 2);
    assert(r2->snapshot->frame == 2);
    ring.endRead(*r2);

    // Reset returns ring to deterministic warm-up state.
    ring.reset();
    const auto rAfterReset = ring.beginReadStaged();
    assert(!rAfterReset.has_value());

    const auto wReset1 = ring.beginWrite();
    wReset1.snapshot->frame = 10;
    ring.publish(wReset1);
    assert(wReset1.writeEpoch == 1);

    const auto rAfterSinglePostResetPublish = ring.beginReadStaged();
    assert(!rAfterSinglePostResetPublish.has_value());

    const auto wReset2 = ring.beginWrite();
    wReset2.snapshot->frame = 11;
    ring.publish(wReset2);
    assert(wReset2.writeEpoch == 2);

    const auto rAfterResetWarmup = ring.beginReadStaged();
    assert(rAfterResetWarmup.has_value());
    assert(rAfterResetWarmup->readEpoch == 1);
    assert(rAfterResetWarmup->snapshot->frame == 10);
    ring.endRead(*rAfterResetWarmup);

    return 0;
}
