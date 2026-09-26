// codegen_main.cpp — reads a deployment plan and emits C++ that wires real
// components together via ring buffers (src/adapter.hpp does the actual
// wiring logic; this program only emits calls into it with concrete types).
//
// This is a pure text generator: it never #includes a component header
// itself, only strings from the plan (cpp_class/header/input_type/
// output_type on each subtask, adapter/adapter_header on each connection —
// see src/deployment_plan.hpp). Only the generated .cpp this program writes
// ever gets compiled against real component headers, by the normal build.
//
// Multi-supplier buffer typing rule (not fully specified in the original
// plan, decided here): a downstream node with fan_in > 1 gets ONE shared
// MultiSupplierRingBuffer<T,N,K>, not one per incoming edge. T is the
// *first* supplier's output_type, in plan.connections file order (which is
// also the order TeamManager::initialize() derives predecessor/supplier
// indices from, so supplier_id assignment here matches it by construction).
// Any other supplier whose own output_type differs from T must declare an
// adapter converting to T on its edge — same rule as the SPSC case, just
// anchored to the first supplier's type instead of a single fixed
// downstream input_type (a multi-supplier node's input_type in the plan,
// e.g. "std::array<double,4>", is documentation for humans only; this tool
// does not parse it structurally).
//
// Sequencing: each generated reader/writer call owns its own local round
// counter (see adapter.hpp's file comment for why that's sufficient without
// any shared/global counter) — codegen does not emit anything for
// sequencing itself, just the reader/writer instantiations.
//
// Usage: codegen_main [plan_path] [out_dir]
//   plan_path defaults to plans/deployment_plan.json
//   out_dir   defaults to generated

#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include "dag.hpp"
#include "parser_json.hpp"
#include "ring_buffer.hpp"

namespace {

const SubtaskInfo& find_subtask(const DeploymentPlan& plan, int id) {
    for (const auto& t : plan.tasks)
        for (const auto& s : t.subtasks)
            if (s.id == id) return s;
    throw std::runtime_error("codegen: subtask id " + std::to_string(id) + " not found in plan");
}

std::string spsc_buf_name(int up, int down) {
    return "buf_" + std::to_string(up) + "_" + std::to_string(down);
}
std::string join_buf_name(int down) {
    return "buf_join_" + std::to_string(down);
}

// Describes one outgoing edge as codegen has resolved it: whether it needs
// an adapter, and (for the multi-supplier case) its position among the
// downstream node's suppliers.
struct ResolvedEdge {
    int upstream, downstream;
    std::string upstream_type;   // this edge's upstream output_type
    std::string buffer_type;     // element type actually stored in the buffer
    bool needs_adapter;
    std::string adapter, adapter_header;
    std::size_t supplier_id = 0; // only meaningful when downstream fan_in > 1
};

} // namespace

int main(int argc, char** argv) {
    const std::string plan_path = argc > 1 ? argv[1] : "plans/deployment_plan.json";
    const std::string out_dir   = argc > 2 ? argv[2] : "generated";

    DeploymentPlan plan;
    try {
        // parse_for_codegen(), not parse(): this tool never #includes a
        // component header (see file comment), so it has no real components
        // to run parse()'s strong component_type-vs-topology check with.
        // That check still runs normally for every other caller (tests, the
        // real application) that has the actual components registered.
        plan = JsonParser{}.parse_for_codegen(plan_path);
    } catch (const std::exception& e) {
        std::cerr << "codegen: failed to parse " << plan_path << ": " << e.what() << "\n";
        return 1;
    }

    // Shape-only DAG, built the same way JsonParser::validate_dag() already
    // does, purely to query fan_in/fan_out/pipeline_depth.
    DAG dag;
    for (const auto& t : plan.tasks)
        for (const auto& s : t.subtasks)
            dag.add_node(s.id, nullptr);
    for (const auto& c : plan.connections)
        dag.add_edge(c.upstream, c.downstream);
    const int depth = dag.pipeline_depth();

    // Connections grouped by downstream id, preserving plan.connections file
    // order — this is what fixes supplier_id assignment to agree with
    // TeamManager::initialize()'s independently-derived predecessor order.
    std::map<int, std::vector<ConnectionInfo>> incoming;
    for (const auto& c : plan.connections)
        incoming[c.downstream].push_back(c);

    // Resolve every edge: adapter requirement, buffer element type, size,
    // supplier index.
    std::vector<ResolvedEdge> edges;
    std::map<int, std::string> join_buf_type; // downstream id -> shared buffer type (multi-supplier only)
    std::map<int, std::size_t> join_buf_n;    // downstream id -> shared buffer size (max over suppliers)

    for (const auto& [down_id, conns] : incoming) {
        const bool is_multi = dag.fan_in_count(down_id) > 1;
        const SubtaskInfo& down_info = find_subtask(plan, down_id);

        for (std::size_t i = 0; i < conns.size(); ++i) {
            const ConnectionInfo& c = conns[i];
            const SubtaskInfo& up_info = find_subtask(plan, c.upstream);

            ResolvedEdge e;
            e.upstream = c.upstream;
            e.downstream = down_id;
            e.upstream_type = up_info.output_type;
            e.adapter = c.adapter;
            e.adapter_header = c.adapter_header;

            const std::string target_type = is_multi
                ? (i == 0 ? up_info.output_type : join_buf_type.at(down_id))
                : down_info.input_type;

            e.needs_adapter = (e.upstream_type != target_type);
            if (e.needs_adapter && (e.adapter.empty() || e.adapter_header.empty()))
                throw std::runtime_error(
                    "codegen: edge " + std::to_string(c.upstream) + "->" + std::to_string(down_id) +
                    " has mismatched types (" + e.upstream_type + " -> " + target_type +
                    ") but no adapter/adapter_header set");
            if (!e.needs_adapter && !e.adapter.empty())
                throw std::runtime_error(
                    "codegen: edge " + std::to_string(c.upstream) + "->" + std::to_string(down_id) +
                    " declares an adapter but its types already match (" + e.upstream_type + ")");

            e.buffer_type = target_type;

            const std::size_t n = ring_buffer_n(up_info.period_ns, down_info.deadline_ns, depth);
            if (is_multi) {
                e.supplier_id = i;
                if (i == 0) { join_buf_type[down_id] = target_type; join_buf_n[down_id] = n; }
                else        { join_buf_n[down_id] = std::max(join_buf_n[down_id], n); }
            }

            edges.push_back(std::move(e));
        }
    }

    // --- collect headers to #include ---
    std::set<std::string> headers;
    for (const auto& t : plan.tasks)
        for (const auto& s : t.subtasks)
            if (!s.header.empty()) headers.insert(s.header);
    for (const auto& e : edges)
        if (e.needs_adapter) headers.insert(e.adapter_header);

    // ---------------------------------------------------------------
    // Emit pipeline_generated.hpp
    // ---------------------------------------------------------------
    std::ofstream hpp(out_dir + "/pipeline_generated.hpp");
    hpp << "// AUTO-GENERATED by tools/codegen/codegen_main.cpp — do not edit.\n"
        << "// Source plan: " << plan_path << "\n"
        << "#pragma once\n"
        << "#include <vector>\n"
        << "#include \"adapter.hpp\"\n"
        << "#include \"dag.hpp\"\n"
        << "#include \"deployment_plan.hpp\"\n"
        << "#include \"team_manager.hpp\"\n\n"
        << "struct GeneratedPipeline {\n"
        << "    DAG dag;\n"
        << "    std::vector<rtmid::WiredNode> nodes;\n"
        << "    std::vector<TeamManager::SubtaskEntry> entries;\n"
        << "};\n\n"
        << "GeneratedPipeline build_pipeline(const DeploymentPlan& plan);\n";
    hpp.close();

    // ---------------------------------------------------------------
    // Emit pipeline_generated.cpp
    // ---------------------------------------------------------------
    std::ofstream cpp(out_dir + "/pipeline_generated.cpp");
    cpp << "// AUTO-GENERATED by tools/codegen/codegen_main.cpp — do not edit.\n"
        << "// Source plan: " << plan_path << "\n"
        << "#include \"pipeline_generated.hpp\"\n";
    for (const auto& h : headers) cpp << "#include \"" << h << "\"\n";
    cpp << "\nnamespace {\n";

    // Buffer declarations: one per SPSC edge, one per multi-supplier node.
    for (const auto& e : edges) {
        const bool is_multi = dag.fan_in_count(e.downstream) > 1;
        if (is_multi) continue; // emitted once below, not per edge
        cpp << "static RingBuffer<" << e.buffer_type << ", " << ring_buffer_n(
                   find_subtask(plan, e.upstream).period_ns,
                   find_subtask(plan, e.downstream).deadline_ns, depth)
            << "> " << spsc_buf_name(e.upstream, e.downstream) << ";\n";
    }
    for (const auto& [down_id, type] : join_buf_type) {
        cpp << "static MultiSupplierRingBuffer<" << type << ", " << join_buf_n.at(down_id)
            << ", " << dag.fan_in_count(down_id) << "> " << join_buf_name(down_id) << ";\n";
    }
    cpp << "} // namespace\n\n";

    cpp << "static const SubtaskInfo& info_for(const DeploymentPlan& plan, int id) {\n"
        << "    for (auto& t : plan.tasks) for (auto& s : t.subtasks) if (s.id == id) return s;\n"
        << "    throw std::runtime_error(\"generated pipeline: subtask id not found\");\n"
        << "}\n\n";

    cpp << "GeneratedPipeline build_pipeline(const DeploymentPlan& plan) {\n"
        << "    GeneratedPipeline gp;\n\n";

    for (const auto& t : plan.tasks) {
        for (const auto& s : t.subtasks) {
            // Upstream reader (or no_upstream{} for a source).
            std::ostringstream upstream;
            const int fan_in = dag.fan_in_count(s.id);
            if (fan_in == 0) {
                upstream << "rtmid::no_upstream{}";
            } else if (fan_in == 1) {
                const auto& e = *std::find_if(edges.begin(), edges.end(),
                    [&](const ResolvedEdge& x) { return x.downstream == s.id; });
                upstream << "rtmid::spsc_reader<" << e.buffer_type << ", " << ring_buffer_n(
                                find_subtask(plan, e.upstream).period_ns, s.deadline_ns, depth)
                          << ">(" << spsc_buf_name(e.upstream, s.id) << ")";
            } else {
                upstream << "rtmid::multi_reader<" << join_buf_type.at(s.id) << ", "
                          << join_buf_n.at(s.id) << ", " << fan_in << ">("
                          << join_buf_name(s.id) << ")";
            }

            // Downstream writers, in plan.connections order among this node's
            // outgoing edges.
            std::vector<std::string> writers;
            for (const auto& e : edges) {
                if (e.upstream != s.id) continue;
                const bool down_is_multi = dag.fan_in_count(e.downstream) > 1;
                std::ostringstream w;
                if (!down_is_multi) {
                    const std::size_t n = ring_buffer_n(s.period_ns, find_subtask(plan, e.downstream).deadline_ns, depth);
                    if (!e.needs_adapter) {
                        w << "rtmid::spsc_writer<" << e.buffer_type << ", " << n << ">("
                          << spsc_buf_name(s.id, e.downstream) << ")";
                    } else {
                        w << "rtmid::spsc_adapted_writer<" << e.upstream_type << ", " << e.buffer_type
                          << ", " << n << ">(" << spsc_buf_name(s.id, e.downstream) << ", &"
                          << e.adapter << ")";
                    }
                } else {
                    const std::size_t n = join_buf_n.at(e.downstream);
                    const int k = dag.fan_in_count(e.downstream);
                    if (!e.needs_adapter) {
                        w << "rtmid::multi_writer<" << e.buffer_type << ", " << n << ", " << k << ">("
                          << join_buf_name(e.downstream) << ", " << e.supplier_id << ")";
                    } else {
                        w << "rtmid::multi_adapted_writer<" << e.upstream_type << ", " << e.buffer_type
                          << ", " << n << ", " << k << ">(" << join_buf_name(e.downstream) << ", "
                          << e.supplier_id << ", &" << e.adapter << ")";
                    }
                }
                writers.push_back(w.str());
            }

            cpp << "    auto n" << s.id << " = rtmid::wire_component<" << s.cpp_class << ">("
                << s.id << ", \"" << s.component_type << "\", info_for(plan, " << s.id << ").config,\n"
                << "        " << upstream.str();
            for (const auto& w : writers) cpp << ",\n        " << w;
            cpp << ");\n"
                << "    gp.dag.add_node(" << s.id << ", n" << s.id << ".instance.component.get());\n"
                << "    gp.entries.push_back({info_for(plan, " << s.id << "), n" << s.id << ".subtask.get()});\n"
                << "    gp.nodes.push_back(std::move(n" << s.id << "));\n\n";
        }
    }

    for (const auto& c : plan.connections)
        cpp << "    gp.dag.add_edge(" << c.upstream << ", " << c.downstream << ");\n";

    cpp << "\n    return gp;\n}\n";
    cpp.close();

    std::cerr << "codegen: wrote " << out_dir << "/pipeline_generated.hpp and .cpp\n";
    return 0;
}
