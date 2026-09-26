#pragma once
#ifndef TEAM_MANAGER_HPP
#define TEAM_MANAGER_HPP

/**
 * @file team_manager.hpp
 *
 * TeamManager — lifecycle manager for a set of subtasks running on one host.
 *
 * Dispatcher sharing (paper Section V-B, partitioned fixed-priority scheduling):
 *   The paper maps one Dispatcher thread per (core, priority) pair, not per
 *   subtask. Multiple subtasks with the same core and priority share a single
 *   Dispatcher — one thread, one queue, one eventfd. Creating a Dispatcher per
 *   subtask would spawn unnecessary threads and violate the scheduling model.
 *
 *   TeamManager groups subtasks by (core, priority) during initialize() and
 *   creates exactly one Dispatcher per unique pair.
 *
 * Other responsibilities (paper Section V-A):
 *   - Derive downstream connections and fan_in_total automatically from the DAG.
 *   - Set period_ns on each subtask from SubtaskInfo.
 *   - Wrap each execute() with exception handling that triggers emergency stop.
 *   - Manage the state machine: CREATED → INITIALIZED → RUNNING →
 *     TERMINATING → TERMINATED.
 *   - On shutdown, terminate every subtask directly (MCFlow Section V-A
 *     protocol: stop accepting new input, propagate to successors,
 *     acknowledge back to TeamManager) and wait for every subtask to
 *     confirm before stopping any Dispatcher thread. This is what makes
 *     Dispatcher stop order safe: once every subtask has acknowledged, no
 *     Dispatcher can still be notify()'d by another, so the CoreIdleControllers
 *     and Dispatchers can be torn down in any order without racing on a
 *     dispatcher's queue_mutex_/efd_ after they've been destroyed/closed.
 *
 * Typical usage:
 *   TeamManager tm;
 *   tm.initialize(entries, dag);   // group and wire everything
 *   tm.start();                    // launch one thread per (core, priority)
 *   tm.notify(source_id);          // fire source subtasks from main thread
 *   ...
 *   tm.stop();                     // orderly shutdown
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>
#include <stdexcept>
#include <iostream>
#include "dag.hpp"
#include "deployment_plan.hpp"
#include "dispatcher.hpp"
#include "core_idle_controller.hpp"
#include "ring_buffer.hpp"

class TeamManager {
public:
    enum class State { CREATED, INITIALIZED, RUNNING, TERMINATING, TERMINATED };

    // One entry per subtask: the SubtaskInfo carries scheduling metadata
    // (id, core, priority, period_ns); the Subtask carries the execute function.
    // TeamManager does NOT own the Subtask objects.
    struct SubtaskEntry {
        SubtaskInfo info;
        Subtask*    subtask;
    };

    TeamManager();
    ~TeamManager();

    // Group subtasks by (core, priority), build one Dispatcher per unique pair,
    // derive downstream connections and fan_in_total from the DAG, and wrap
    // each execute() with exception handling.
    // Precondition: state == CREATED.
    void initialize(const std::vector<SubtaskEntry>& entries, const DAG& dag);

    // Start all dispatcher threads (one per unique (core, priority) pair).
    // Precondition: state == INITIALIZED.
    void start();

    // Stop all dispatchers in reverse creation order (sinks first), then
    // transition to TERMINATED. Idempotent — safe to call multiple times.
    void stop();

    // Deliver one activation to the subtask identified by subtask_id.
    // Precondition: state == RUNNING.
    void notify(int subtask_id);

    // Called when a subtask's execute() throws. Transitions to TERMINATING so
    // the caller can detect the fault and finish cleanup with stop().
    // Thread-safe; may be called from a dispatcher thread.
    void on_subtask_exception(int subtask_id);

    // Called by a Subtask's on_stopped closure once it has confirmed
    // termination (see Dispatcher::process_subtask). Thread-safe; called
    // from whichever dispatcher thread owns that subtask.
    void on_subtask_stopped(int subtask_id);

    State state() const;

    // Returns the number of Dispatcher instances created (≤ number of subtasks).
    // Useful for verifying that sharing works correctly in tests.
    std::size_t dispatcher_count() const;

    // Returns the recommended ring buffer slot count for the connection
    // upstream_id → downstream_id.
    //
    // Formula (paper Section IV + plan):
    //   N = max(2, ceil(deadline_downstream / period_upstream) + pipeline_depth)
    //
    // Rationale:
    //   - ceil(D/T): how many upstream jobs can be released before the
    //     downstream's deadline expires (jobs in-flight at peak load).
    //   - pipeline_depth: worst-case simultaneous occupancy across all stages.
    //   - min 2: double-buffering floor so producer never blocks consumer.
    //
    // If upstream period_ns == 0 (aperiodic), returns pipeline_depth + 2.
    // Returns 0 if the edge does not exist in the DAG.
    //
    // RingBuffer<T, N> has a compile-time N; use this value at code-generation
    // time (or with a template helper) to size the buffer correctly.
    std::size_t ring_buffer_size(int upstream_id, int downstream_id) const;

private:
    void do_stop();

    // (core, priority) → owned Dispatcher
    using CorePrio = std::pair<int, int>;
    using Edge     = std::pair<int, int>;   // (upstream_id, downstream_id)

    mutable std::mutex                              state_mutex_;
    State                                           state_;
    std::vector<int>                                topo_order_;

    // Dispatchers keyed by (core, priority); one per unique scheduling class.
    std::map<CorePrio, std::unique_ptr<Dispatcher>> dispatchers_;

    // Dispatcher creation order (follows topological order of first subtask
    // assigned to each pair). Used to start dispatchers in a stable order;
    // no longer load-bearing for stop() correctness (see do_stop()), kept
    // for symmetry with creation.
    std::vector<CorePrio>                           dispatcher_order_;

    // One shared idle thread controller per physical core (MCFlow Section
    // V-C), keyed by core id. Populated alongside dispatchers_ in initialize().
    std::map<int, std::unique_ptr<CoreIdleController>> idle_controllers_;

    // Shutdown quiescence: how many subtasks have not yet acknowledged
    // termination. do_stop() blocks on stop_ack_cv_ until this reaches 0
    // (or a timeout), before touching any Dispatcher.
    std::atomic<int>       pending_stop_acks_{0};
    std::mutex              stop_ack_mutex_;
    std::condition_variable stop_ack_cv_;

    // subtask_id → its Dispatcher* (raw pointer into dispatchers_)
    std::map<int, Dispatcher*>                      subtask_dispatcher_;

    std::map<int, Subtask*>                         subtasks_;

    // Precomputed ring buffer sizes: (upstream_id, downstream_id) → N
    std::map<Edge, std::size_t>                     ring_buffer_sizes_;
};

#endif // TEAM_MANAGER_HPP
