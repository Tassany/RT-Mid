// Integration test: exercises Scheduling + Orchestration + the ring-buffer
// data plane together — DAG -> TeamManager -> Dispatcher -> CoreIdleController
// -> fan-in join, with real payloads moving through RingBuffer /
// MultiSupplierRingBuffer between subtasks — not just "does everything
// compile side by side", and not just control-flow counts. Topology:
//
//   S(1) --> A(2) --> T(4)
//   S(1) --> B(3) --> T(4)
//
// Data plane: S writes into two SPSC RingBuffer<double,4> (one per outgoing
// edge — matches ring_buffer.hpp's own model: one ring buffer per port, not
// per node). A and B each write their result into a shared
// MultiSupplierRingBuffer<double,4,2> (T has two suppliers); T only reads
// once ready() reports both have written. That data-plane readiness is a
// second, independent synchronization mechanism from the Dispatcher's own
// fan_in_mask (control plane) — this test checks that the two agree: by the
// time the Dispatcher lets T run, the ring buffer must already be ready().
//
// Control plane, same as before: all four subtasks share one (core,
// priority) pair (Dispatcher-sharing, paper Section V-B, for 4 subtasks on
// one thread). A is periodic; its second release is deliberately triggered
// before its period elapses, forcing a defer into timer_queue_ that only the
// shared CoreIdleController can drain. Ends with a full TeamManager::stop(),
// exercising the MCFlow-style termination protocol (a hang here means
// do_stop() deadlocked).
//
// Not covered here: parser_json.cpp / JsonParser (needs allocator.hpp,
// which doesn't exist yet — the allocator is being rewritten separately) and
// component.hpp/ComponentRegistry (already covered end-to-end by
// component_registry_test.cpp; there is no codegen/wiring layer yet that
// connects a real Component's input_/output_ to ring buffers automatically,
// so this test wires RingBuffers by hand instead of inventing that layer).
//
// Build: g++ -std=c++17 -Isrc -Iinclude tests/integration_pipeline_test.cpp src/dag.cpp src/team_manager.cpp -o /tmp/integration_test

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>
#include "dag.hpp"
#include "team_manager.hpp"
#include "ring_buffer.hpp"

static void expect(bool cond, const char* what) {
    if (!cond) { std::cerr << "FAIL: " << what << "\n"; std::exit(1); }
    std::cerr << "ok:   " << what << "\n";
}

int main() {
    DAG dag;
    for (int id : {1, 2, 3, 4}) dag.add_node(id, nullptr);
    dag.add_edge(1, 2);
    dag.add_edge(1, 3);
    dag.add_edge(2, 4);
    dag.add_edge(3, 4);

    std::atomic<int> exec_count[5] = {}; // indexed by subtask id, [0] unused

    // Data plane: one SPSC buffer per S->{A,B} edge, one shared
    // MultiSupplierRingBuffer for A,B->T's join. job_seq publishes which
    // sequence number the current round's S execution used, so A/B/T (which
    // run strictly after S within the same round, on the same dispatcher
    // thread) know which slot to read.
    RingBuffer<double, 4>            buf_S_to_A;
    RingBuffer<double, 4>            buf_S_to_B;
    MultiSupplierRingBuffer<double, 4, 2> buf_to_T;
    std::atomic<size_t> next_job{0};
    std::atomic<size_t> job_seq{0};
    std::atomic<bool>   last_round_data_ok{false};

    auto exec_S = [&]() {
        size_t j = next_job.fetch_add(1, std::memory_order_relaxed);
        double v = 10.0 * static_cast<double>(j + 1);
        buf_S_to_A.write(j, v);
        buf_S_to_B.write(j, v);
        job_seq.store(j, std::memory_order_release);
        exec_count[1].fetch_add(1);
    };
    auto exec_A = [&]() {
        size_t j = job_seq.load(std::memory_order_acquire);
        double v = buf_S_to_A.read(j);
        buf_to_T.write(j, /*supplier_id=*/0, v * 2.0);
        buf_S_to_A.release(j);
        exec_count[2].fetch_add(1);
    };
    auto exec_B = [&]() {
        size_t j = job_seq.load(std::memory_order_acquire);
        double v = buf_S_to_B.read(j);
        buf_to_T.write(j, /*supplier_id=*/1, v * 3.0);
        buf_S_to_B.release(j);
        exec_count[3].fetch_add(1);
    };
    auto exec_T = [&]() {
        size_t j = job_seq.load(std::memory_order_acquire);
        // Data-plane vs. control-plane agreement: the Dispatcher only runs T
        // once its fan_in_mask says both A and B notified it, which must
        // imply the ring buffer's own independent bitmask already agrees.
        bool ready = buf_to_T.ready(j);
        double expected = 10.0 * static_cast<double>(j + 1) * 5.0; // v*2 + v*3
        double got = ready ? (buf_to_T.read(j, 0) + buf_to_T.read(j, 1)) : -1.0;
        buf_to_T.release(j);
        last_round_data_ok.store(ready && got == expected, std::memory_order_release);
        exec_count[4].fetch_add(1);
    };

    Subtask s1(1, exec_S);
    Subtask s2(2, exec_A);
    Subtask s3(3, exec_B);
    Subtask s4(4, exec_T);

    constexpr uint64_t period_ms = 50;

    TeamManager::SubtaskEntry e1{{}, &s1};
    TeamManager::SubtaskEntry e2{{}, &s2};
    TeamManager::SubtaskEntry e3{{}, &s3};
    TeamManager::SubtaskEntry e4{{}, &s4};
    e1.info.id = 1; e1.info.core = 0; e1.info.priority = 10;
    e2.info.id = 2; e2.info.core = 0; e2.info.priority = 10;
    e2.info.period_ns = period_ms * 1'000'000ULL; // set on the SubtaskInfo:
    // TeamManager::initialize() copies info.period_ns onto the Subtask,
    // overwriting anything set directly on the Subtask beforehand.
    e3.info.id = 3; e3.info.core = 0; e3.info.priority = 10;
    e4.info.id = 4; e4.info.core = 0; e4.info.priority = 10;

    TeamManager tm;
    tm.initialize({e1, e2, e3, e4}, dag);
    expect(tm.state() == TeamManager::State::INITIALIZED, "TeamManager reaches INITIALIZED");
    expect(tm.dispatcher_count() == 1,
           "all four subtasks share one Dispatcher (same core/priority)");

    tm.start();
    expect(tm.state() == TeamManager::State::RUNNING, "TeamManager reaches RUNNING");

    // Round 1: full fan-out/fan-in join. The whole chain runs on one thread
    // and is driven purely by eventfd wakeups, so it settles in well under a
    // millisecond in practice; 15ms leaves comfortable margin without eating
    // into A's 50ms period before round 2 fires.
    tm.notify(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    expect(exec_count[1] == 1, "S ran once");
    expect(exec_count[2] == 1, "A's first release ran immediately (next_release_ns was unset)");
    expect(exec_count[3] == 1, "B ran once");
    expect(exec_count[4] == 1, "T ran once — fan-in from both A and B was required and met");
    expect(last_round_data_ok.load(),
           "round 1: MultiSupplierRingBuffer was ready() when T ran, and carried the exact "
           "values A and B wrote — data plane agrees with the Dispatcher's control-plane join");

    // Round 2, fired at ~15ms since A's first release: still well inside its
    // 50ms period, so process_subtask() must defer A into timer_queue_
    // instead of running it.
    tm.notify(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    expect(exec_count[2] == 1, "A's second release is deferred, not run yet");
    expect(exec_count[4] == 1, "T has not fired a second time (still missing A's side of the join)");

    // Wait past the period (now ~25ms elapsed since round 2; period is
    // 50ms from A's first release, so this clears it with margin): only
    // CoreIdleController drains timer_queue_ from here.
    std::this_thread::sleep_for(std::chrono::milliseconds(period_ms));
    expect(exec_count[2] == 2,
           "CoreIdleController drained A's deferred release once its period elapsed");
    expect(exec_count[4] == 2, "T fired a second time once A's deferred release completed the join");
    expect(last_round_data_ok.load(),
           "round 2: ring buffer data was still correct after a deferred/timer-driven release, "
           "not just after an immediate one");

    tm.stop();
    expect(tm.state() == TeamManager::State::TERMINATED,
           "TeamManager reaches TERMINATED — do_stop() did not deadlock");

    std::cerr << "\nAll checks passed.\n";
    return 0;
}
