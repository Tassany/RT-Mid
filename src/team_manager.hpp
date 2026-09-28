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
 *   - Set period_ns on each subtask from SubtaskInfo's period_us (converted
 *     to nanoseconds — Dispatcher compares it against monotonic_ns()).
 *   - Wrap each execute() with exception handling that triggers emergency stop.
 *   - Manage the state machine: CREATED → INITIALIZED → RUNNING →
 *     TERMINATING → TERMINATED.
 *   - On shutdown, terminate every subtask directly (MCFlow Section V-A
 *     protocol: stop accepting new input, propagate to successors,
 *     acknowledge back to TeamManager) and wait for every subtask to
 *     confirm before stopping any Dispatcher thread. This is what makes
 *     Dispatcher stop order safe: once every subtask has acknowledged, no
 *     Dispatcher can still be notify()'d by another, so the Dispatchers
 *     (each now owning its own idle thread — see dispatcher.hpp) can be
 *     torn down in any order without racing on a dispatcher's
 *     queue_mutex_/efd_ after they've been destroyed/closed.
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
#include "ring_buffer.hpp"

/**
 * @brief Lifecycle manager for the subtasks running on one host.
 *
 * Groups subtasks by (core, priority) into shared Dispatcher threads,
 * wires downstream connections and fan-in from the DAG, wraps execute()
 * calls with fault handling, and drives the CREATED → INITIALIZED →
 * RUNNING → TERMINATING → TERMINATED state machine, including the
 * MCFlow-style orderly shutdown protocol.
 */
class TeamManager {
public:
    /** @brief Lifecycle states of a TeamManager instance. */
    enum class State { CREATED, INITIALIZED, RUNNING, TERMINATING, TERMINATED };

    // One entry per subtask: the SubtaskInfo carries scheduling metadata
    // (id, core, priority, period_us); the Subtask carries the execute function.
    // TeamManager does NOT own the Subtask objects.
    /**
     * @brief One subtask's scheduling metadata plus its executable object.
     *
     * TeamManager does not own the Subtask pointed to here.
     * @var SubtaskEntry::info Scheduling metadata (id, core, priority,
     *      period_us, ...).
     * @var SubtaskEntry::subtask Non-owning pointer to the executable
     *      subtask.
     */
    struct SubtaskEntry {
        SubtaskInfo info;
        Subtask*    subtask;
    };

    /** @brief Constructs a TeamManager in state CREATED. */
    TeamManager();
    /** @brief Destructor; does not implicitly stop a running TeamManager. */
    ~TeamManager();

    // Group subtasks by (core, priority), build one Dispatcher per unique pair,
    // derive downstream connections and fan_in_total from the DAG, and wrap
    // each execute() with exception handling.
    // Precondition: state == CREATED.
    /**
     * @brief Wires subtasks into shared dispatchers and derives topology.
     *
     * Groups subtasks by (core, priority), builds one Dispatcher per
     * unique pair (each Dispatcher owns its own idle thread — see
     * dispatcher.hpp), derives downstream connections and fan_in_total
     * from @p dag, and wraps each execute() with exception handling.
     * Precondition: state() == CREATED.
     *
     * @param entries Subtasks to wire, with their scheduling metadata.
     * @param dag Dependency graph used to derive connections and fan-in.
     * @return void
     */
    void initialize(const std::vector<SubtaskEntry>& entries, const DAG& dag);

    // Start all dispatcher threads (one per unique (core, priority) pair).
    // Precondition: state == INITIALIZED.
    /**
     * @brief Starts all dispatcher and idle-controller threads.
     * Precondition: state() == INITIALIZED.
     * @return void
     */
    void start();

    // Stop all dispatchers in reverse creation order (sinks first), then
    // transition to TERMINATED. Idempotent — safe to call multiple times.
    /**
     * @brief Runs the orderly shutdown protocol and stops all threads.
     *
     * Terminates every subtask, waits for all termination acknowledgments,
     * then stops all dispatchers and idle controllers and transitions to
     * TERMINATED. Idempotent — safe to call multiple times.
     * @return void
     */
    void stop();

    // Deliver one activation to the subtask identified by subtask_id.
    // Precondition: state == RUNNING.
    /**
     * @brief Delivers one activation to a subtask.
     * Precondition: state() == RUNNING.
     * @param subtask_id Id of the subtask to activate.
     * @return void
     */
    void notify(int subtask_id);

    // Called when a subtask's execute() throws. Transitions to TERMINATING so
    // the caller can detect the fault and finish cleanup with stop().
    // Thread-safe; may be called from a dispatcher thread.
    /**
     * @brief Reports a fault from a subtask's execute().
     *
     * Transitions to TERMINATING so the caller can detect the fault and
     * finish cleanup with stop(). Thread-safe; may be called from a
     * dispatcher thread.
     * @param subtask_id Id of the subtask whose execute() threw.
     * @return void
     */
    void on_subtask_exception(int subtask_id);

    // Called by a Subtask's on_stopped closure once it has confirmed
    // termination (see Dispatcher::process_subtask). Thread-safe; called
    // from whichever dispatcher thread owns that subtask.
    /**
     * @brief Records that a subtask has confirmed termination.
     *
     * Called by a Subtask's on_stopped closure (see
     * Dispatcher::process_subtask). Thread-safe; called from whichever
     * dispatcher thread owns that subtask.
     * @param subtask_id Id of the subtask that confirmed termination.
     * @return void
     */
    void on_subtask_stopped(int subtask_id);

    /**
     * @brief Reads the current lifecycle state.
     * @return Current State.
     */
    State state() const;

    // Returns the number of Dispatcher instances created (≤ number of subtasks).
    // Useful for verifying that sharing works correctly in tests.
    /**
     * @brief Reports how many Dispatcher instances were created.
     * @return Number of Dispatcher instances (≤ number of subtasks); useful
     *         for verifying that (core, priority) sharing works in tests.
     */
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
    // If upstream period_us == 0 (aperiodic), returns pipeline_depth + 2.
    // Returns 0 if the edge does not exist in the DAG.
    //
    // RingBuffer<T, N> has a compile-time N; use this value at code-generation
    // time (or with a template helper) to size the buffer correctly.
    /**
     * @brief Computes the recommended ring buffer slot count for an edge.
     *
     * Formula (paper Section IV + plan): N = max(2,
     * ceil(deadline_downstream / period_upstream) + pipeline_depth). The
     * ceil term bounds in-flight upstream jobs at peak load, pipeline_depth
     * bounds worst-case simultaneous occupancy across stages, and the
     * floor of 2 guarantees double-buffering. If the upstream period is 0
     * (aperiodic), returns pipeline_depth + 2. Use this value at
     * code-generation time to size a RingBuffer<T, N>'s compile-time N.
     *
     * @param upstream_id Id of the producing subtask.
     * @param downstream_id Id of the consuming subtask.
     * @return Recommended slot count, or 0 if the edge does not exist in
     *         the DAG.
     */
    std::size_t ring_buffer_size(int upstream_id, int downstream_id) const;

private:
    /** @brief Shared shutdown-protocol implementation used by stop(). */
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
