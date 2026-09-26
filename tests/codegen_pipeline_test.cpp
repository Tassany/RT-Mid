// End-to-end test for the codegen tool: parses the real deployment plan,
// #includes the generated files, and drives the pipeline through a REAL
// TeamManager — zero manual RingBuffer .write()/.read()/.release() calls
// anywhere in this file, in direct contrast to
// tests/integration_pipeline_test.cpp's hand-wiring. Also captures
// execution order to prove the generated pipeline actually runs source
// before every intermediate before the sink on the real dispatched
// pipeline, not just that DAG::topological_sort() says it should.
//
// Run via `make build/codegen_pipeline_test && ./build/codegen_pipeline_test`
// (or `make test`) — the Makefile runs the codegen tool first as an
// order-only prerequisite. Running this file's g++ command directly without
// that step will fail to find generated/pipeline_generated.hpp.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>
#include "parser_json.hpp"
#include "pipeline_generated.hpp"
#include "demo_components.hpp"

static void expect(bool cond, const char* what) {
    if (!cond) { std::cerr << "FAIL: " << what << "\n"; std::exit(1); }
    std::cerr << "ok:   " << what << "\n";
}

int main() {
    JsonParser parser;
    DeploymentPlan plan = parser.parse("plans/deployment_plan.json");

    GeneratedPipeline gp = build_pipeline(plan);
    expect(gp.nodes.size() == 6, "build_pipeline() wired all six subtasks");

    // Wrap each node's execute() to record execution order, BEFORE
    // TeamManager::initialize() wraps it again for exception handling. That
    // wrap captures whatever execute() currently is as its inner closure, so
    // this one still runs (innermost) every time the subtask executes.
    std::mutex order_mutex;
    std::vector<int> exec_order;
    SinkDemo* sink_ptr = nullptr;

    for (auto& node : gp.nodes) {
        int id = node.subtask->id;
        if (id == 6) sink_ptr = static_cast<SinkDemo*>(node.instance.component.get());

        auto original = node.subtask->execute;
        node.subtask->execute = [id, original, &order_mutex, &exec_order]() {
            {
                std::lock_guard<std::mutex> lk(order_mutex);
                exec_order.push_back(id);
            }
            original();
        };
    }
    expect(sink_ptr != nullptr, "found the sink component (id 6) among the wired nodes");

    TeamManager tm;
    tm.initialize(gp.entries, gp.dag);
    tm.start();
    tm.notify(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    // --- Data correctness: read the sink's own input_ directly. Gains in
    // plans/deployment_plan.json are 2.0/3.0/4.0/5.0 for T0..T3; Ts outputs
    // 1.0; IntermediateDemo::execute() does output_ = input_ * gain. Not a
    // single .write()/.read()/.release() call appears in this test file —
    // every one of those happened inside generated code + adapter.hpp.
    const std::array<double, 4> expected = {2.0, 3.0, 4.0, 5.0};
    expect(sink_ptr->input_ == expected,
           "sink's input_ holds exactly what each intermediate produced, moved there entirely "
           "by generated code — no manual ring buffer calls in this test");

    // --- Execution order: source before every intermediate, every
    // intermediate before the sink, on the real dispatched pipeline.
    {
        std::lock_guard<std::mutex> lk(order_mutex);
        expect(exec_order.size() == 6, "all six subtasks executed exactly once");

        auto pos = [&](int id) {
            return std::distance(exec_order.begin(),
                                  std::find(exec_order.begin(), exec_order.end(), id));
        };
        const long ts_pos = pos(1), tm_pos = pos(6);
        bool order_ok = true;
        for (int mid : {2, 3, 4, 5}) {
            const long p = pos(mid);
            order_ok = order_ok && (ts_pos < p) && (p < tm_pos);
        }
        expect(order_ok, "execution order is topologically valid: source ran before every "
                          "intermediate, and every intermediate ran before the sink");
    }

    tm.stop();
    expect(tm.state() == TeamManager::State::TERMINATED, "TeamManager reaches TERMINATED cleanly");

    std::cerr << "\nAll checks passed.\n";
    return 0;
}
