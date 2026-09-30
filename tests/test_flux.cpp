// "main" for a minimal scenario: hand-wires N1 -> {N2, N3} -> N4
// (tests/plans/deployment_plan_test.json) through the REAL scheduler —
// real FluxSource/FluxIntermediate/FluxSink components (component.hpp via
// flux_components.hpp), real RingBuffers, a real TeamManager driving real
// Dispatcher threads, each with its own idle thread (dispatcher.hpp). No
// codegen: it was deleted to be rebuilt later (see project memory on
// that) — everything below is written by hand using the exact primitives
// codegen used to emit (adapter.hpp's wire_component()/spsc_*/multi_*).
//
// Build: g++ -std=c++17 -Isrc -Iinclude -Itests tests/test_flux.cpp src/dag.cpp src/parser_json.cpp src/team_manager.cpp -o /tmp/test_flux

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#include "parser_json.hpp"
#include "flux_components.hpp"
#include "dag.hpp"
#include "adapter.hpp"
#include "team_manager.hpp"

static void expect(bool cond, const char* what) {
    if (!cond) { std::cerr << "FAIL: " << what << "\n"; std::exit(1); }
    std::cerr << "ok:   " << what << "\n";
}

// Finds a subtask's plan metadata (config, period, ...) by id — same
// lookup the old codegen_main.cpp generated per plan.
static const SubtaskInfo& info_for(const DeploymentPlan& plan, int id) {
    for (const auto& t : plan.tasks)
        for (const auto& s : t.subtasks)
            if (s.id == id) return s;
    throw std::runtime_error("test_flux: subtask id " + std::to_string(id) + " not found");
}

int main() {
    // --- 1. Parse the plan ------------------------------------------------
    // Same as before: builds+discards two internal DAGs to validate cycle
    // freedom and component_type-vs-topology agreement.
    JsonParser parser;
    DeploymentPlan plan;
    try {
        plan = parser.parse("tests/plans/deployment_plan_test.json");
    } catch (const std::exception& e) {
        std::cerr << "FAIL: parse() threw: " << e.what() << "\n";
        return 1;
    }
    std::cerr << "ok:   parse() succeeded without throwing\n";


    // --- 2. Build the DAG (bridge step, same as before) -------------------
    // TeamManager::initialize() needs this directly — it derives dispatcher
    // grouping order and each subtask's fan_in_mask/downstream wiring from
    // it (see team_manager.cpp).
    DAG dag;
    for (const auto& st : plan.tasks[0].subtasks) dag.add_node(st.id);
    for (const auto& c : plan.connections) dag.add_edge(c.upstream, c.downstream);
    const int depth = dag.pipeline_depth(); // N1->N2->N4 or N1->N3->N4: 3 nodes
    // Guards the hardcoded 3 passed to ring_buffer_n() below — codegen used
    // to compute this itself at generation time; by hand, this is what
    // catches N silently going stale if the topology ever changes.
    expect(depth == 3, "pipeline_depth matches what N below assumes");


    // --- 3. Ring buffers: one per edge -------------------------------------
    // N sized via the SAME formula codegen used to emit (ring_buffer_n(),
    // ring_buffer.hpp): N = next_pow2(max(2, ceil(deadline/period) + depth)).
    // Every subtask in this plan shares period_us=5000/deadline_us=5000, so
    // every edge gets the same N=4 here — hardcoded because there's no
    // codegen anymore to derive it from the plan automatically (flagged as
    // a follow-up in project memory).
    //
    // Declaration order matters: these must be declared BEFORE the wired
    // nodes/TeamManager below (C++ destroys locals in reverse declaration
    // order, so this keeps the buffers alive for as long as Dispatcher
    // threads might still be touching them through the reader/writer
    // closures captured into each Subtask::execute).
    static constexpr std::size_t N = ring_buffer_n(5000, 5000, 3);
    RingBuffer<double, N>                 buf_1_2;      // N1 -> N2
    RingBuffer<double, N>                 buf_1_3;      // N1 -> N3
    MultiSupplierRingBuffer<double, N, 2> buf_join_4;    // N2, N3 -> N4 (fan-in 2)


    // --- 4. Wire each real component to its buffers ------------------------
    // wire_component<ConcreteType>(id, config, upstream_reader, downstream_writers...)
    // constructs FluxSource/FluxIntermediate/FluxSink DIRECTLY (bypassing
    // ComponentRegistry — same as codegen's generated code did, since the
    // concrete type is already known here at compile time) and returns a
    // WiredNode owning the component + a runnable Subtask whose execute()
    // reads upstream, calls the component's real execute(), and fans
    // output_ out to every downstream writer.
    auto n1 = rtmid::wire_component<FluxSource>(1, info_for(plan, 1).config,
        rtmid::no_upstream{},
        rtmid::spsc_writer<double, N>(buf_1_2),
        rtmid::spsc_writer<double, N>(buf_1_3));

    auto n2 = rtmid::wire_component<FluxIntermediate>(2, info_for(plan, 2).config,
        rtmid::spsc_reader<double, N>(buf_1_2),
        rtmid::multi_writer<double, N, 2>(buf_join_4, /*supplier_id=*/0));

    auto n3 = rtmid::wire_component<FluxIntermediate>(3, info_for(plan, 3).config,
        rtmid::spsc_reader<double, N>(buf_1_3),
        rtmid::multi_writer<double, N, 2>(buf_join_4, /*supplier_id=*/1));

    auto n4 = rtmid::wire_component<FluxSink>(4, info_for(plan, 4).config,
        rtmid::multi_reader<double, N, 2>(buf_join_4));
    // n4 has no downstream writers — it's the sink.


    // --- 5. Record execution order + timing ---------------------------------
    // Wrap each node's subtask->execute BEFORE TeamManager::initialize()
    // wraps it again (it adds its own exception-handling wrapper) — this
    // outer wrap still runs (innermost) on every real execution.
    //
    // Timestamp taken AFTER original() (not before): exec_ns[id] records the
    // instant a subtask FINISHED, same convention as
    // tools/eval/latency_eval_main.cpp's finish_ns — and the same clock,
    // Dispatcher::monotonic_ns() (CLOCK_MONOTONIC), used everywhere real
    // timing decisions happen in this project (Dispatcher's own release-time
    // checks, src/main.cpp's periodic drivers). Deliberately NOT
    // std::chrono::steady_clock, even though it's typically the same clock
    // under the hood on Linux — using the project's own accessor keeps every
    // timestamp comparable against Dispatcher-internal times without relying
    // on that being true.
    std::mutex order_mutex;
    std::vector<int> exec_order;
    std::map<int, uint64_t> exec_ns;
    rtmid::WiredNode* nodes[] = { &n1, &n2, &n3, &n4 };
    for (auto* node : nodes) {
        int id = node->subtask->id;
        auto original = node->subtask->execute;
        node->subtask->execute = [id, original, &order_mutex, &exec_order, &exec_ns]() {
            original();
            uint64_t now = Dispatcher::monotonic_ns();
            std::lock_guard<std::mutex> lk(order_mutex);
            exec_order.push_back(id);
            exec_ns[id] = now;
        };
    }


    // --- 6. Drive the real pipeline through TeamManager ---------------------
    // SubtaskEntry pairs each subtask's plan metadata (core/priority/period,
    // used for Dispatcher grouping) with the real Subtask* wire_component
    // built above.
    TeamManager::SubtaskEntry e1{info_for(plan, 1), n1.subtask.get()};
    TeamManager::SubtaskEntry e2{info_for(plan, 2), n2.subtask.get()};
    TeamManager::SubtaskEntry e3{info_for(plan, 3), n3.subtask.get()};
    TeamManager::SubtaskEntry e4{info_for(plan, 4), n4.subtask.get()};

    TeamManager tm;
    tm.initialize({e1, e2, e3, e4}, dag);
    expect(tm.state() == TeamManager::State::INITIALIZED, "TeamManager reaches INITIALIZED");
    // Every subtask in this plan is explicitly pinned to core 0 (and
    // shares priority 10) in tests/plans/deployment_plan_test.json, so all
    // four share the same (core, priority) pair, hence one Dispatcher.
    // Pinned deliberately, not auto-allocated: this test exercises
    // TeamManager/Dispatcher, not the allocator (see allocator_test.cpp for
    // that) — and this plan's total utilization (1.08) exceeds one core's
    // capacity anyway, so real WF+DRU auto-allocation couldn't place it on
    // a single core even if asked to.
    expect(tm.dispatcher_count() == 1, "all four subtasks share one Dispatcher");

    tm.start();
    expect(tm.state() == TeamManager::State::RUNNING, "TeamManager reaches RUNNING");

    // release_ns: the instant N1's job was released — same CLOCK_MONOTONIC
    // source as exec_ns above, taken immediately before notify() so
    // dispatch latency counts against the deadline below, exactly like
    // latency_eval_main.cpp's release_ns (its file comment: "not the moment
    // the Dispatcher actually got around to running it").
    const uint64_t release_ns = Dispatcher::monotonic_ns();
    tm.notify(1); // fires N1, the only source (fan_in_count == 0)
    std::this_thread::sleep_for(std::chrono::milliseconds(30));


    // --- 7. Assert: execution order + the values that actually flowed ------
    {
        std::lock_guard<std::mutex> lk(order_mutex);
        expect(exec_order.size() == 4, "all four subtasks executed exactly once");

        auto pos = [&](int id) {
            return std::distance(exec_order.begin(),
                                  std::find(exec_order.begin(), exec_order.end(), id));
        };
        expect(pos(1) < pos(2), "N1 ran before N2");
        expect(pos(1) < pos(3), "N1 ran before N3");
        expect(pos(2) < pos(4), "N2 ran before N4");
        expect(pos(3) < pos(4), "N3 ran before N4");

        // --- Timing: end-to-end response time vs. N4's deadline ----------
        // Same definition tools/eval/latency_eval_main.cpp uses for the
        // paper's own metrics (Section 5): response time = sink's finish
        // time - source's release time. The deadline compared against is
        // N4's deadline_us — the SINK's, not each intermediate subtask's
        // own deadline_us. N2/N3's deadline_us fields aren't individually
        // enforced anywhere in this codebase today; they're only read by
        // ring_buffer_n() for buffer sizing, never compared against an
        // actual execution timestamp the way N4's is being compared here.
        for (int id : {1, 2, 3, 4}) {
            std::cerr << "  N" << id << " finished at +"
                      << ((exec_ns.at(id) - release_ns) / 1000) << " us\n";
        }

        const uint64_t finish_ns   = exec_ns.at(4);
        const uint64_t response_ns = finish_ns - release_ns;
        const uint64_t deadline_ns = info_for(plan, 4).deadline_us * 1000;
        std::cerr << "  response time: " << (response_ns / 1000) << " us (deadline "
                  << (deadline_ns / 1000) << " us)\n";
        expect(response_ns <= deadline_ns,
               "end-to-end response time (N1 release -> N4 finish) meets N4's deadline");
    }

    // FluxSource always outputs 1.0; FluxIntermediate does
    // output_ = input_ * config_->gain (gains 2.0/3.0 from the plan's
    // "config" for N2/N3); FluxSink's input_ is std::array<double,2> in
    // supplier order (supplier_id 0 = N2, 1 = N3), so it should hold
    // {2.0, 3.0} — and FluxSink::execute() sums them into result_.
    auto* sink = static_cast<FluxSink*>(n4.instance.component.get());
    expect(sink->input_[0] == 2.0, "N4 received N2's output (1.0 * gain 2.0)");
    expect(sink->input_[1] == 3.0, "N4 received N3's output (1.0 * gain 3.0)");
    expect(sink->result_ == 5.0, "N4's merge (2.0 + 3.0) is exactly 5.0");


    // --- 8. Clean shutdown ---------------------------------------------------
    tm.stop();
    expect(tm.state() == TeamManager::State::TERMINATED, "TeamManager reaches TERMINATED cleanly");

    std::cerr << "\nAll checks passed.\n";
    return 0;
}
