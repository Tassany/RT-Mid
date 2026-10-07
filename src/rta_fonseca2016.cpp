#include "rta_fonseca2016.hpp"
#include "dag.hpp"
#include "ied.hpp"
#include "rrc.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <queue>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

namespace rta_fonseca2016 {

namespace detail {

namespace {

// BFS reachability from `start` following `neighbors_of(node)` (either
// successors, for descendants, or predecessors, for ancestors) -- same
// small, duplicated-per-precedent pattern as rrc.cpp's own
// `reachable_via` (two independent call sites beat a shared header for a
// 15-line BFS -- this codebase's established convention).
template <typename NeighborsFn>
std::unordered_set<int> reachable_via(int start, const std::unordered_map<int, const DAG::Node*>& node_by_id,
                                       NeighborsFn neighbors_of) {
    std::unordered_set<int> visited;
    std::queue<int> frontier;
    frontier.push(start);
    while (!frontier.empty()) {
        int cur = frontier.front();
        frontier.pop();
        auto it = node_by_id.find(cur);
        if (it == node_by_id.end())
            continue;
        for (int next : neighbors_of(it->second))
            if (visited.insert(next).second)
                frontier.push(next);
    }
    return visited;
}

} // namespace

/** @copydoc group_execution_regions */
std::vector<ExecutionRegion> group_execution_regions(
    const std::vector<int>& path, const std::vector<SubtaskInfo>& task_subtasks) {

    std::unordered_map<int, int> core_by_id;
    std::unordered_map<int, uint64_t> wcet_by_id;
    for (const auto& st : task_subtasks) {
        core_by_id[st.id] = st.core;
        wcet_by_id[st.id] = st.wcet_us;
    }

    std::vector<ExecutionRegion> regions;
    for (int id : path) {
        int core = core_by_id.at(id);
        double wcet = static_cast<double>(wcet_by_id.at(id));
        if (!regions.empty() && regions.back().core == core) {
            regions.back().subtask_ids.push_back(id);
            regions.back().wcet += wcet;
        } else {
            ExecutionRegion region;
            region.core = core;
            region.subtask_ids.push_back(id);
            region.wcet = wcet;
            regions.push_back(std::move(region));
        }
    }
    return regions;
}

/** @copydoc p_workload */
double p_workload(const std::vector<SubtaskInfo>& task_subtasks, int core) {
    double sum = 0.0;
    for (const auto& st : task_subtasks)
        if (st.core == core)
            sum += static_cast<double>(st.wcet_us);
    return sum;
}

/** @copydoc self_interference_set */
std::vector<int> self_interference_set(
    const std::vector<int>& path,
    const std::vector<SubtaskInfo>& task_subtasks,
    const std::vector<ConnectionInfo>& reduced_connections) {

    DAG dag;
    for (const auto& st : task_subtasks)
        dag.add_node(st.id);
    for (const auto& c : reduced_connections)
        dag.add_edge(c.upstream, c.downstream);

    std::unordered_map<int, const DAG::Node*> node_by_id;
    for (const auto& node : dag.nodes())
        node_by_id[node.id] = &node;

    std::unordered_map<int, int> core_by_id;
    for (const auto& st : task_subtasks)
        core_by_id[st.id] = st.core;

    std::unordered_set<int> path_set(path.begin(), path.end());

    // proc(path): distinct cores the path touches, and per core, the
    // first/last path subtask on it (path is already in source-to-sink
    // order, so a single linear scan finds both).
    std::vector<int> proc_cores;
    std::unordered_map<int, int> first_on_core, last_on_core;
    for (int id : path) {
        int core = core_by_id.at(id);
        if (!first_on_core.count(core)) {
            first_on_core[core] = id;
            proc_cores.push_back(core);
        }
        last_on_core[core] = id;
    }

    // Theta: for each core p, ancestors of v_a^p restricted to p, union
    // descendants of v_z^p restricted to p.
    std::unordered_set<int> theta;
    for (int p : proc_cores) {
        int v_a = first_on_core.at(p);
        int v_z = last_on_core.at(p);
        auto ancestors = reachable_via(v_a, node_by_id,
            [](const DAG::Node* n) -> const std::vector<int>& { return n->predecessors; });
        auto descendants = reachable_via(v_z, node_by_id,
            [](const DAG::Node* n) -> const std::vector<int>& { return n->successors; });
        for (int a : ancestors)
            if (core_by_id.at(a) == p)
                theta.insert(a);
        for (int d : descendants)
            if (core_by_id.at(d) == p)
                theta.insert(d);
    }

    std::unordered_set<int> proc_set(proc_cores.begin(), proc_cores.end());
    std::vector<int> result;
    for (const auto& st : task_subtasks) {
        if (!proc_set.count(st.core))
            continue;
        if (path_set.count(st.id))
            continue;
        if (theta.count(st.id))
            continue;
        result.push_back(st.id);
    }
    std::sort(result.begin(), result.end());
    return result;
}

/** @copydoc virtual_tasks_for_core */
std::vector<VirtualTask> virtual_tasks_for_core(const TaskInfo& hp_task, int core, double r_hp_task) {
    std::vector<VirtualTask> result;
    for (const auto& st : hp_task.subtasks) {
        if (st.core != core)
            continue;
        VirtualTask vt;
        vt.c = static_cast<double>(st.wcet_us);
        vt.t = static_cast<double>(st.period_us);
        vt.j = r_hp_task - vt.c;
        result.push_back(vt);
    }
    return result;
}

namespace {

constexpr int kMaxIterations = 10000; // same convention as rrc::detail::response_time_of_path
constexpr double kEpsilon = 1e-9;

// Eq. 7/8's shared fixed-point shape: R = base + sum over virtual tasks
// of ceil((R+J)/T)*C. `base` already includes everything that doesn't
// depend on R (execution-region WCET, S^{p,ub}, self-interference).
double solve_fixed_point(double base, const std::vector<VirtualTask>& virtual_tasks, double deadline_us) {
    double r = base;
    for (int iter = 0; iter < kMaxIterations; ++iter) {
        double interference = 0.0;
        for (const auto& vt : virtual_tasks)
            interference += std::ceil((r + vt.j) / vt.t) * vt.c;
        double new_r = base + interference;
        if (std::fabs(new_r - r) < kEpsilon)
            return new_r;
        if (new_r > deadline_us)
            return new_r;
        r = new_r;
    }
    throw std::runtime_error(
        "rta_fonseca2016: fixed-point iteration did not converge within " +
        std::to_string(kMaxIterations) + " iterations");
}

struct RegionResult {
    double r;
    double s_ub;
};

// Keyed by (first,last) REGION-INDEX RANGE -- matching the paper's own
// RTs matrix (indexed by subtask pairs), not by core alone. Required,
// not a stylistic choice: a core can legitimately be resolved more than
// once over DIFFERENT ranges within one path (e.g. an alternating
// c0,c1,c0,c1 pattern resolves c0 over one range and c1 over another,
// each needing the OTHER's resolution as its own "remote" suspension
// source -- keying by core alone makes these collide). See plan.md's
// Risks table.
using Memo = std::map<std::pair<int, int>, RegionResult>;

// Everything path_analysis needs that doesn't change across the
// recursion, bundled to keep its own signature manageable.
struct Context {
    const DeploymentPlan* plan;
    const TaskInfo* task;
    std::unordered_map<int, int> core_by_id;   // task's own subtasks
    std::unordered_map<int, uint64_t> wcet_by_id; // task's own subtasks
    std::vector<int> self_lambda;              // self(path) at the top level
    double deadline_us;
};

double self_wcet_on_core(const Context& ctx, int core) {
    double sum = 0.0;
    for (int id : ctx.self_lambda)
        if (ctx.core_by_id.at(id) == core)
            sum += static_cast<double>(ctx.wcet_by_id.at(id));
    return sum;
}

// Every higher-priority task with >=1 subtask on `core`, expanded into
// virtual tasks (Theorem 3) -- hp_task's own WCRT is computed on demand
// via this same paper's compute_wcrt, consistent with rrc.cpp's own
// recompute-per-call (no cross-call caching) convention.
std::vector<VirtualTask> collect_virtual_tasks(const Context& ctx, int core);

// First/last index of `core` within regions[lo..hi] (inclusive) -- the
// range a *nested* resolution of that core was computed over, which is
// not necessarily the same as its first/last index in the whole path.
std::pair<int, int> local_first_last(const std::vector<ExecutionRegion>& regions, int lo, int hi, int core) {
    int first = -1, last = -1;
    for (int i = lo; i <= hi; ++i) {
        if (regions[i].core != core)
            continue;
        if (first < 0)
            first = i;
        last = i;
    }
    return {first, last};
}

// Populates memo[(first,last)] (and, recursively, every core's own
// resolution within regions[first..last]) -- the caller
// (response_time_of_path) reads memo directly afterward (Theorem 2), so
// this has no return value; see plan.md's Technical Approach for why
// R(lambda) isn't assembled from any single recursive call's own result.
void path_analysis(const std::vector<ExecutionRegion>& regions, int first, int last,
                    const Context& ctx, Memo& memo) {
    int core = regions[first].core;

    if (first == last) {
        double self_sum = self_wcet_on_core(ctx, core);
        auto virtual_tasks = collect_virtual_tasks(ctx, core);
        double r = solve_fixed_point(regions[first].wcet + self_sum, virtual_tasks, ctx.deadline_us);
        memo[{first, last}] = RegionResult{r, 0.0};
        return;
    }

    int last_core = regions[last].core;
    if (core == last_core) {
        double s_ub = 0.0;
        double c_h = regions[first].wcet + regions[last].wcet;
        if (first + 1 <= last - 1) {
            path_analysis(regions, first + 1, last - 1, ctx, memo);
            std::vector<int> middle_cores;
            for (int i = first + 1; i <= last - 1; ++i)
                if (std::find(middle_cores.begin(), middle_cores.end(), regions[i].core) == middle_cores.end())
                    middle_cores.push_back(regions[i].core);
            for (int o : middle_cores) {
                auto range = local_first_last(regions, first + 1, last - 1, o);
                const RegionResult& remote = memo.at(range);
                s_ub += remote.r - remote.s_ub;
            }
        }

        double self_sum = self_wcet_on_core(ctx, core);
        auto virtual_tasks = collect_virtual_tasks(ctx, core);
        double r = solve_fixed_point(c_h + s_ub + self_sum, virtual_tasks, ctx.deadline_us);
        memo[{first, last}] = RegionResult{r, s_ub};
        return;
    }

    // Different cores (Algorithm 1 lines 18-28): isolate the suffix
    // starting at the first occurrence of last_core, resolving its full
    // extent; then recurse on everything except the very last region.
    int idx = first;
    while (regions[idx].core != last_core)
        ++idx;
    path_analysis(regions, idx, last, ctx, memo);
    path_analysis(regions, first, last - 1, ctx, memo);
}

std::vector<VirtualTask> collect_virtual_tasks(const Context& ctx, int core) {
    std::vector<VirtualTask> result;
    for (const auto& t : ctx.plan->tasks) {
        if (t.id == ctx.task->id || t.priority >= ctx.task->priority)
            continue;
        bool on_core = false;
        for (const auto& st : t.subtasks)
            if (st.core == core) { on_core = true; break; }
        if (!on_core)
            continue;
        double r_hp = compute_wcrt(*ctx.plan, t);
        auto vts = virtual_tasks_for_core(t, core, r_hp);
        result.insert(result.end(), vts.begin(), vts.end());
    }
    return result;
}

} // namespace

/** @copydoc response_time_of_path */
double response_time_of_path(
    const DeploymentPlan& plan,
    const TaskInfo& task,
    const std::vector<int>& path,
    const std::vector<SubtaskInfo>& task_subtasks,
    const std::vector<ConnectionInfo>& reduced_connections) {

    if (plan.tasks.size() > 1)
        for (const auto& t : plan.tasks)
            if (t.priority == TASK_PRIORITY_UNSET)
                throw std::runtime_error(
                    "rta_fonseca2016::response_time_of_path: task " + std::to_string(t.id) +
                    " has no priority set, but the plan has " + std::to_string(plan.tasks.size()) +
                    " tasks -- hp(tau_i) is undefined without a total priority order");

    Context ctx;
    ctx.plan = &plan;
    ctx.task = &task;
    for (const auto& st : task_subtasks) {
        ctx.core_by_id[st.id] = st.core;
        ctx.wcet_by_id[st.id] = st.wcet_us;
    }
    ctx.self_lambda = self_interference_set(path, task_subtasks, reduced_connections);
    ctx.deadline_us = static_cast<double>(task_subtasks.front().deadline_us);

    auto regions = group_execution_regions(path, task_subtasks);
    int last_index = static_cast<int>(regions.size()) - 1;

    Memo memo;
    path_analysis(regions, 0, last_index, ctx, memo);

    // Theorem 2 (Eq. 5), applied explicitly -- see this file's header
    // comment and plan.md's Technical Approach. Each core's own
    // contribution is looked up by its TRUE first/last occurrence over
    // the WHOLE path (not just any entry that happens to exist for it --
    // see local_first_last's own comment on why range-keying matters).
    std::vector<int> proc_lambda;
    for (const auto& region : regions)
        if (std::find(proc_lambda.begin(), proc_lambda.end(), region.core) == proc_lambda.end())
            proc_lambda.push_back(region.core);

    double r_lambda = 0.0;
    for (int p : proc_lambda) {
        auto range = local_first_last(regions, 0, last_index, p);
        const RegionResult& res = memo.at(range);
        r_lambda += res.r - res.s_ub;
    }
    return r_lambda;
}

} // namespace detail

/** @copydoc compute_wcrt */
double compute_wcrt(const DeploymentPlan& plan, const TaskInfo& task) {
    auto reduced = ied::remove_invalid_edges(task.subtasks, ied::task_connections(plan, task));
    auto paths = rrc::detail::enumerate_complete_paths(task.subtasks, reduced);

    double max_r = 0.0;
    for (const auto& path : paths) {
        double r = detail::response_time_of_path(plan, task, path, task.subtasks, reduced);
        max_r = std::max(max_r, r);
    }
    return max_r;
}

/** @copydoc check_schedulability */
std::vector<int> check_schedulability(const DeploymentPlan& plan) {
    std::vector<int> unschedulable;
    for (const auto& task : plan.tasks) {
        double r = compute_wcrt(plan, task);
        double deadline_us = static_cast<double>(task.subtasks.front().deadline_us);
        if (r > deadline_us)
            unschedulable.push_back(task.id);
    }
    return unschedulable;
}

} // namespace rta_fonseca2016
