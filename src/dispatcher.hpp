#pragma once

/**
 * @file dispatcher.hpp
 *
 * Implements the MCFlow dispatching subsystem (paper Section V-B, V-C).
 *
 * Key additions over the previous version
 * ----------------------------------------
 * - Subtask carries period_ns / next_release_ns for periodic scheduling.
 * - Subtask carries an atomic in_processing flag (required by leader/followers).
 * - Subtask carries fan_in_mask / fan_in_mask_full for multi-supplier fan-in:
 *   each predecessor is assigned a fixed bit (see SubtaskConn::supplier_bit),
 *   so notify() ORs that bit in rather than incrementing a blind counter.
 *   A plain counter cannot tell "two different suppliers signalled once
 *   each" apart from "one supplier signalled twice", which a bitmask can —
 *   this matters once a node has 2+ real predecessors racing on different
 *   cores, not just the source nodes a linear chain ever had.
 * - Subtask carries a downstream list so the dispatcher can automatically
 *   notify successors after execution (no manual wiring in execute()).
 * - Dispatcher owns a min-heap timer_queue_ for deferred periodic subtasks,
 *   AND its own dedicated idle thread that drains it (SCHED_FIFO priority
 *   1, only runs when nothing higher-priority on its core is ready) — one
 *   idle thread per Dispatcher, not shared across the Dispatchers pinned
 *   to the same physical core. Reverted from an earlier version that tried
 *   a single shared-per-core idle thread (CoreIdleController, MCFlow
 *   Section V-C's literal reading): under real contention that design's
 *   coarser ~10ms polling and one-dispatcher-serviced-per-wake behavior
 *   let a deferred subtask's own producers pile up against ring-buffer
 *   backpressure faster than the shared idle thread could rescue them,
 *   measured directly on tests/performance_test.cpp (Table I workload)
 *   before this revert. Each Dispatcher's own idle thread here arms its
 *   timerfd_ synchronously, at the exact moment a subtask is deferred (see
 *   process_subtask()), rather than polling — no cross-dispatcher wake
 *   contention.
 * - notify() enforces the fan-in condition before enqueuing.
 * - The 6-step release-guard protocol (Section V-C) is implemented in
 *   process_subtask(), which is also exposed via Demultiplexer::process().
 * - terminate() implements the MCFlow Section V-A termination protocol
 *   (adapted to a single host, no network acks): a subtask stops accepting
 *   new releases, propagates the request to its successors, and
 *   acknowledges TeamManager, all executed at its own real-time priority.
 * - stop() bounds pthread_join to 300ms (pthread_timedjoin_np, a GNU/Linux
 *   extension — hence _GNU_SOURCE below, defined ahead of every system
 *   header in this TU since it's a feature-test macro, not a runtime
 *   flag): a dispatch thread stuck past that is detached rather than
 *   blocking stop() (and every dispatcher after it) forever. Last-resort
 *   safety net, not the primary shutdown path — see
 *   ring_buffer.hpp's WriteAbortedOnShutdown for what's meant to unblock
 *   a stuck thread well before this timeout is ever reached.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <queue>
#include <vector>
#include <functional>
#include <atomic>
#include <iostream>
#include <pthread.h>
#include <sched.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <time.h>

// Abstract notification interface — implemented by both Dispatcher and
// PreemptiveDispatcher so SubtaskConn can hold either without casting.
/**
 * @brief Abstract notification interface implemented by Dispatcher (and
 *        PreemptiveDispatcher), so SubtaskConn can hold either without
 *        casting.
 */
class IDispatcher {
public:
    // supplier_bit identifies which predecessor is signalling (see
    // SubtaskConn::supplier_bit); default of 1 (bit 0) is correct for the
    // overwhelming majority of calls, where the target has a single
    // supplier: sources ticked directly by TeamManager::notify, and any
    // hand-built Subtask that never sets fan_in_mask_full beyond its default.
    /**
     * @brief Signals that one predecessor's precondition is met for @p s.
     * @param s Subtask being notified.
     * @param supplier_bit Bit identifying which predecessor is signalling
     *        (see SubtaskConn::supplier_bit); default (bit 0) is correct
     *        for a single-supplier target.
     * @return void
     */
    virtual void notify(struct Subtask* s, uint64_t supplier_bit = 1) = 0;
    /**
     * @brief Enqueues a termination request for @p s.
     * @param s Subtask to terminate.
     * @return void
     */
    virtual void terminate(struct Subtask* s) = 0;
    /** @brief Virtual destructor for safe polymorphic destruction. */
    virtual ~IDispatcher() = default;
};

// -----------------------------------------------------------------------
//  Downstream connection descriptor
// -----------------------------------------------------------------------
/**
 * @brief One downstream edge from a subtask to a successor it notifies.
 * @var SubtaskConn::dispatcher Dispatcher owning the target subtask.
 * @var SubtaskConn::subtask Target subtask to notify.
 * @var SubtaskConn::supplier_bit Bit of the target's fan_in_mask this edge
 *      sets; distinct predecessors of the same subtask must use distinct
 *      bits (1, 2, 4, ...), assigned by TeamManager::initialize from the
 *      DAG.
 */
struct SubtaskConn {
    IDispatcher*    dispatcher;
    struct Subtask* subtask;
    // Which bit of the target's fan_in_mask this edge sets. Distinct
    // predecessors of the same subtask must use distinct bits (1, 2, 4, ...)
    // — see TeamManager::initialize, which assigns them from the DAG.
    uint64_t        supplier_bit = 1;
};

// -----------------------------------------------------------------------
//  Subtask — unit of work with real-time scheduling metadata
// -----------------------------------------------------------------------
/**
 * @brief One unit of scheduled work plus its real-time metadata.
 *
 * @var Subtask::id Unique subtask identifier.
 * @var Subtask::execute Function running this subtask's work for one job.
 * @var Subtask::period_ns Period in nanoseconds; 0 means aperiodic
 *      (execute immediately every time).
 * @var Subtask::next_release_ns Earliest absolute CLOCK_MONOTONIC time
 *      for the next execution; 0 means not yet initialized (execute
 *      immediately on first notification).
 * @var Subtask::in_processing Guards against concurrent execution under
 *      the leader/followers pattern.
 * @var Subtask::fan_in_mask_full Bitmask of predecessor bits that must
 *      all be set before dispatch; default is a single supplier at bit 0.
 * @var Subtask::fan_in_mask Bits accumulated so far from notify() calls.
 * @var Subtask::downstream Successors notified automatically after this
 *      subtask finishes.
 * @var Subtask::terminating Set once shutdown has started for this
 *      subtask.
 * @var Subtask::stop_acked Guards the propagate+acknowledge block so it
 *      runs exactly once regardless of how many times terminate() fires.
 * @var Subtask::on_stopped Callback invoked once termination is
 *      acknowledged; wired by TeamManager.
 */
struct Subtask {
    int                   id;
    std::function<void()> execute;

    // Periodic scheduling: 0 = aperiodic (execute immediately every time)
    uint64_t period_ns       = 0;
    // Earliest absolute time (CLOCK_MONOTONIC ns) for next execution.
    // 0 = not yet initialised → execute immediately on first notification.
    uint64_t next_release_ns = 0;

    // Prevents concurrent execution when the leader/followers pattern is used.
    std::atomic<bool> in_processing{false};

    // Fan-in: bit i set in fan_in_mask_full means predecessor i must notify
    // (with supplier_bit = 1 << i) before we dispatch. Default: single
    // supplier at bit 0, so a plain notify(s) with no bit argument works.
    uint64_t              fan_in_mask_full = 1;
    std::atomic<uint64_t> fan_in_mask{0};

    // Successors to notify automatically after this subtask finishes.
    std::vector<SubtaskConn> downstream;

    // Termination (MCFlow-style, Section V-A): set once by whoever starts
    // shutdown (directly by TeamManager, and/or by a predecessor that
    // already stopped). stop_acked guards the propagate+ack block below so
    // it runs exactly once no matter how many times terminate() fires for
    // this subtask. on_stopped is wired by TeamManager (same pattern as
    // execute's exception-handling wrapper) so Dispatcher never needs to
    // know about TeamManager's type.
    std::atomic<bool>     terminating{false};
    std::atomic<bool>     stop_acked{false};
    std::function<void()> on_stopped;

    /** @brief Constructs an empty, unconfigured Subtask. */
    Subtask() = default;
    /**
     * @brief Constructs a Subtask with an id and its execute function.
     * @param i Unique subtask identifier.
     * @param fn Function to run for one job of this subtask.
     */
    Subtask(int i, std::function<void()> fn)
        : id(i), execute(std::move(fn)) {}

    // Non-copyable: atomic members cannot be copied.
    Subtask(const Subtask&)            = delete;
    Subtask& operator=(const Subtask&) = delete;
};

// -----------------------------------------------------------------------
//  Timer queue support
// -----------------------------------------------------------------------
/**
 * @brief A deferred periodic subtask waiting for its release time.
 * @var TimerEntry::release_ns Absolute CLOCK_MONOTONIC time this entry
 *      becomes due.
 * @var TimerEntry::subtask Subtask to dispatch once due.
 */
struct TimerEntry {
    uint64_t release_ns;
    Subtask* subtask;
    /**
     * @brief Orders entries so the priority_queue's top is the earliest.
     * @param o Entry to compare against.
     * @return true if this entry's release_ns is later than @p o's.
     */
    bool operator>(const TimerEntry& o) const { return release_ns > o.release_ns; }
};

/** @brief Min-heap of TimerEntry ordered by earliest release_ns first. */
using TimerQueue = std::priority_queue<TimerEntry,
                                       std::vector<TimerEntry>,
                                       std::greater<TimerEntry>>;

// -----------------------------------------------------------------------
//  Dispatcher
// -----------------------------------------------------------------------
/**
 * @brief One (core, priority) scheduling class's dispatch thread.
 *
 * Owns a queue of ready subtasks, a min-heap timer_queue_ of deferred
 * periodic subtasks, and its own dedicated idle thread (lowest SCHED_FIFO
 * priority) that drains that queue, and runs the MCFlow release-guard/
 * dispatch protocol (paper Section V-B, V-C) on a dedicated real-time
 * thread pinned to one core.
 */
class Dispatcher : public IDispatcher {
public:
    /**
     * @brief Constructs a dispatcher for one (core, priority) pair.
     * @param core CPU core this dispatcher's thread will be pinned to.
     * @param priority SCHED_FIFO priority applied to this dispatcher's
     *        thread.
     */
    Dispatcher(int core, int priority)
        : core_(core), priority_(priority),
          efd_(-1), epfd_(-1), idle_efd_(-1), timerfd_(-1), running_(false) {}

    /** @brief Stops the dispatch thread if running. */
    ~Dispatcher() { stop(); }

    /**
     * @brief Registers a subtask as scheduled by this dispatcher.
     * @param s Subtask to register; not owned by this dispatcher.
     * @return void
     */
    void register_subtask(Subtask* s) { subtasks_.push_back(s); }

    /**
     * @brief Reads this dispatcher's SCHED_FIFO priority.
     * @return Configured priority value.
     */
    int priority() const { return priority_; }

    /**
     * @brief Enqueues a termination request for @p s at its own priority.
     *
     * Processed like any other job — i.e. at s's own real-time priority,
     * in FIFO order behind whatever genuine work is already queued ahead
     * of it (paper Section V-A: "a termination protocol for each task is
     * executed at the task's real-time priority"). Unlike notify(), this
     * does not wait on the fan-in mask: termination must reach @p s
     * regardless of whether its data preconditions are currently met.
     *
     * @param s Subtask to terminate.
     * @return void
     */
    void terminate(Subtask* s) override {
        s->terminating.store(true, std::memory_order_release);
        pthread_mutex_lock(&queue_mutex_);
        queue_.push(s);
        pthread_mutex_unlock(&queue_mutex_);
        uint64_t sig = 1;
        ::write(efd_, &sig, sizeof(sig));
    }

    /**
     * @brief Signals one predecessor's precondition met; dispatches when
     *        all are.
     *
     * Enqueues @p s only once every bit of fan_in_mask_full has been set.
     * A bitmask (not a counter) is what makes this safe with 2+ real
     * predecessors: OR-ing the same bit twice before the target fires is
     * a no-op, so a supplier that signals twice for one job cannot be
     * mistaken for two distinct suppliers each signalling once, the way a
     * plain increment would. Thread-safe; may be called from any thread.
     *
     * @param s Subtask being notified.
     * @param supplier_bit Bit identifying which predecessor is signalling.
     * @return void
     */
    void notify(Subtask* s, uint64_t supplier_bit = 1) override {
        uint64_t mask = s->fan_in_mask.fetch_or(supplier_bit, std::memory_order_acq_rel)
                       | supplier_bit;
        if ((mask & s->fan_in_mask_full) != s->fan_in_mask_full) return; // still waiting
        s->fan_in_mask.fetch_and(~s->fan_in_mask_full, std::memory_order_acq_rel);

        pthread_mutex_lock(&queue_mutex_);
        queue_.push(s);
        pthread_mutex_unlock(&queue_mutex_);

        uint64_t sig = 1;
        ::write(efd_, &sig, sizeof(sig));
    }

    /**
     * @brief Creates the wake eventfd/epoll/timerfd and starts both the
     *        dispatch thread and this dispatcher's own idle thread.
     * @return void
     */
    void start() {
        efd_      = eventfd(0, EFD_SEMAPHORE);
        epfd_     = epoll_create1(0);
        idle_efd_ = eventfd(0, EFD_SEMAPHORE);
        timerfd_  = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);

        struct epoll_event ev{};
        ev.events  = EPOLLIN;
        ev.data.fd = efd_;
        epoll_ctl(epfd_, EPOLL_CTL_ADD, efd_, &ev);

        pthread_mutex_init(&queue_mutex_, nullptr);
        pthread_mutex_init(&timer_mutex_, nullptr);

        running_ = true;
        pthread_create(&thread_,      nullptr, static_loop,      this);
        pthread_create(&idle_thread_, nullptr, static_idle_loop, this);
    }

    /**
     * @brief Signals both threads to exit and joins them, bounded.
     *
     * Waits up to 300ms (pthread_timedjoin_np) per thread. TeamManager::
     * do_stop() already waited up to 2s for every subtask's graceful
     * termination ack before calling this, and a thread stuck in
     * process_subtask() should now unblock almost immediately via
     * WriteAbortedOnShutdown (ring_buffer.hpp) once its own subtask's
     * `terminating` flag is set — so 300ms here is a last-resort net for
     * whatever that doesn't cover, not the primary mechanism. Either
     * thread timing out gets detached instead of blocking stop() (and
     * every dispatcher stopped after this one) forever; in that case
     * efd_/epfd_/idle_efd_/timerfd_/the mutexes are deliberately left
     * open/alive rather than destroyed, since an orphaned thread may
     * still reference them if it ever does unblock — a small,
     * intentional, documented leak in that one case, traded for not
     * risking undefined behavior in a thread we can no longer account for.
     * @return void
     */
    void stop() {
        if (!running_) return;
        running_ = false;

        uint64_t wake = 1;
        ::write(efd_,      &wake, sizeof(wake));
        ::write(idle_efd_, &wake, sizeof(wake));

        // Both calls must run regardless of the first result — do not
        // short-circuit with ||, or a timed-out dispatch thread would skip
        // ever attempting to join the idle thread.
        const bool dispatch_joined = timed_join(thread_,      "dispatch");
        const bool idle_joined     = timed_join(idle_thread_, "idle");
        if (!dispatch_joined || !idle_joined)
            return; // at least one timed out and was detached — leak resources on purpose, see doc comment above

        close(efd_);      efd_      = -1;
        close(epfd_);     epfd_     = -1;
        close(idle_efd_); idle_efd_ = -1;
        close(timerfd_);  timerfd_  = -1;

        pthread_mutex_destroy(&queue_mutex_);
        pthread_mutex_destroy(&timer_mutex_);
    }

    // Returns current time in nanoseconds (CLOCK_MONOTONIC).
    /**
     * @brief Reads the current CLOCK_MONOTONIC time.
     * @return Current monotonic time, in nanoseconds.
     */
    static uint64_t monotonic_ns() {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
               static_cast<uint64_t>(ts.tv_nsec);
    }

    // Exposed so Demultiplexer and the idle thread can call it.
    // Implements the 6-step release-guard protocol (paper Section V-C).
    /**
     * @brief Runs the 6-step release-guard/dispatch protocol for one
     *        subtask.
     *
     * Guards against concurrent execution, handles termination
     * (propagating and acknowledging on first arrival), defers a
     * not-yet-due periodic subtask into timer_queue_, otherwise advances
     * next_release_ns, runs s->execute(), and notifies every downstream
     * connection. Exposed so Demultiplexer and the idle thread can call
     * it directly.
     *
     * @param s Subtask to process.
     * @return void
     */
    void process_subtask(Subtask* s) {
        // Step 2: skip if already executing (leader/followers guard)
        if (s->in_processing.exchange(true)) return;

        // Termination (MCFlow Section V-A), checked ahead of the periodic
        // release-time logic below so a terminating subtask is never
        // deferred into the timer queue waiting for a release that will
        // never matter. stop_acked makes this idempotent: TeamManager
        // terminates every subtask directly, and a subtask may also receive
        // this via propagation from an already-stopped predecessor: only
        // the first arrival propagates downstream and acknowledges.
        if (s->terminating.load(std::memory_order_acquire)) {
            if (!s->stop_acked.exchange(true, std::memory_order_acq_rel)) {
                for (auto& conn : s->downstream)
                    conn.dispatcher->terminate(conn.subtask);
                if (s->on_stopped) s->on_stopped();
            }
            s->in_processing.store(false);
            return;
        }

        uint64_t now = monotonic_ns();

        // Steps 3 & 4a: check if release time has arrived
        if (s->period_ns > 0 && s->next_release_ns > 0 && now < s->next_release_ns) {
            // Defer: push into timer queue; arm this dispatcher's OWN
            // timerfd_ at the earliest pending release time, synchronously,
            // right here — no cross-thread callback/polling round-trip.
            s->in_processing.store(false);

            pthread_mutex_lock(&timer_mutex_);
            timer_queue_.push({s->next_release_ns, s});
            uint64_t earliest = timer_queue_.top().release_ns;
            pthread_mutex_unlock(&timer_mutex_);

            struct itimerspec its{};
            its.it_value.tv_sec  = earliest / 1'000'000'000ULL;
            its.it_value.tv_nsec = earliest % 1'000'000'000ULL;
            timerfd_settime(timerfd_, TFD_TIMER_ABSTIME, &its, nullptr);
            return;
        }

        // Step 4b: advance next_release_ns for strict periodicity
        if (s->period_ns > 0) {
            s->next_release_ns = (s->next_release_ns == 0)
                ? now + s->period_ns
                : s->next_release_ns + s->period_ns;
        }

        // Execute the subtask
        s->execute();

        // Step 5: propagate to downstream subtasks
        for (auto& conn : s->downstream)
            conn.dispatcher->notify(conn.subtask, conn.supplier_bit);

        // Step 6: clear in_processing
        s->in_processing.store(false);
    }

    // Called by this dispatcher's own idle thread: dispatch earliest timer
    // entry if past due.
    /**
     * @brief Moves every past-due timer entry into the ready queue.
     *
     * Called by this dispatcher's own idle thread (idle_loop()): pops
     * every entry from timer_queue_ whose release_ns is now due and
     * enqueues its subtask for normal dispatch.
     * @return void
     */
    void dispatch_expired_timers() {
        uint64_t now = monotonic_ns();

        pthread_mutex_lock(&timer_mutex_);
        while (!timer_queue_.empty() && timer_queue_.top().release_ns <= now) {
            Subtask* s = timer_queue_.top().subtask;
            timer_queue_.pop();
            pthread_mutex_unlock(&timer_mutex_);

            pthread_mutex_lock(&queue_mutex_);
            queue_.push(s);
            pthread_mutex_unlock(&queue_mutex_);

            uint64_t sig = 1;
            ::write(efd_, &sig, sizeof(sig));

            pthread_mutex_lock(&timer_mutex_);
        }
        pthread_mutex_unlock(&timer_mutex_);
    }

private:
    /**
     * @brief Dispatch thread body: pins/prioritizes, then services the
     *        queue.
     *
     * Pins the calling thread to core_, applies SCHED_FIFO priority_,
     * then repeatedly epoll_waits on efd_ (20ms timeout) and, on wake,
     * dequeues and process_subtask()s one ready subtask followed by
     * draining any remaining ready subtasks, until running_ is cleared
     * by stop().
     * @return void
     */
    void loop() {
        // Pin to designated core
        cpu_set_t mask;
        CPU_ZERO(&mask);
        CPU_SET(core_, &mask);
        pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask);

        // Apply real-time priority (requires CAP_SYS_NICE / root)
        struct sched_param param{};
        param.sched_priority = priority_;
        if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0)
            std::cerr << "[Dispatcher core=" << core_
                      << "] warning: RT priority not applied (run with sudo)\n";

        // std::cerr << "[Dispatcher] core=" << core_
        //           << " priority=" << priority_ << " started\n";

        struct epoll_event events[1];

        while (running_) {
            int n = epoll_wait(epfd_, events, 1, /*timeout_ms=*/20);
            if (n <= 0) continue;

            uint64_t val;
            ::read(efd_, &val, sizeof(val));

            // Step 1: dequeue one subtask
            pthread_mutex_lock(&queue_mutex_);
            if (queue_.empty()) { pthread_mutex_unlock(&queue_mutex_); continue; }
            Subtask* next = queue_.front();
            queue_.pop();
            pthread_mutex_unlock(&queue_mutex_);

            // Steps 2–6
            process_subtask(next);

            // Paper step 5 continuation: drain remaining ready subtasks.
            // Bounded by running_ so stop() doesn't have to wait out an
            // overloaded dispatcher's whole backlog before pthread_join
            // returns — it finishes the in-flight subtask and leaves the
            // rest of the queue unprocessed, same as any other shutdown.
            while (running_) {
                pthread_mutex_lock(&queue_mutex_);
                if (queue_.empty()) { pthread_mutex_unlock(&queue_mutex_); break; }
                next = queue_.front();
                queue_.pop();
                pthread_mutex_unlock(&queue_mutex_);
                process_subtask(next);
            }
        }

        // std::cerr << "[Dispatcher] core=" << core_ << " stopped\n";
    }

    /**
     * @brief Idle thread body: pins to the core at the lowest SCHED_FIFO
     *        priority, then drains timer_queue_ as entries become due.
     *
     * Waits on timerfd_ (armed exactly at the earliest pending release,
     * by process_subtask()) and idle_efd_ (shutdown signal); a 10ms
     * epoll timeout is a fallback for edge-case races only, not the
     * primary wake mechanism.
     * @return void
     */
    void idle_loop() {
        cpu_set_t mask;
        CPU_ZERO(&mask);
        CPU_SET(core_, &mask);
        pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask);

        struct sched_param param{};
        param.sched_priority = 1; // minimum SCHED_FIFO priority
        pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);

        int idle_epfd = epoll_create1(0);
        struct epoll_event ev{};
        ev.events  = EPOLLIN;
        ev.data.fd = idle_efd_;
        epoll_ctl(idle_epfd, EPOLL_CTL_ADD, idle_efd_, &ev);
        ev.data.fd = timerfd_;
        epoll_ctl(idle_epfd, EPOLL_CTL_ADD, timerfd_, &ev);

        struct epoll_event events[2];
        while (running_) {
            int n = epoll_wait(idle_epfd, events, 2, /*timeout_ms=*/10);
            for (int i = 0; i < n; ++i) {
                uint64_t val;
                ::read(events[i].data.fd, &val, sizeof(val)); // drain token
            }
            dispatch_expired_timers();
        }

        close(idle_epfd);
    }

    /**
     * @brief pthread trampoline that invokes loop() on the dispatcher.
     * @param arg Dispatcher* to run the loop on.
     * @return Always nullptr.
     */
    static void* static_loop(void* arg) {
        static_cast<Dispatcher*>(arg)->loop(); return nullptr;
    }
    /**
     * @brief pthread trampoline that invokes idle_loop() on the dispatcher.
     * @param arg Dispatcher* to run the idle loop on.
     * @return Always nullptr.
     */
    static void* static_idle_loop(void* arg) {
        static_cast<Dispatcher*>(arg)->idle_loop(); return nullptr;
    }

    /**
     * @brief Joins @p t with a 300ms bound; detaches and warns on timeout.
     * @param t Thread to join.
     * @param label Short name used in the timeout warning ("dispatch"/"idle").
     * @return true if joined within the bound; false if timed out (and
     *         the thread was detached instead).
     */
    bool timed_join(pthread_t t, const char* label) {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline); // pthread_timedjoin_np uses CLOCK_REALTIME, not MONOTONIC
        deadline.tv_nsec += 300'000'000L;
        if (deadline.tv_nsec >= 1'000'000'000L) {
            deadline.tv_sec  += 1;
            deadline.tv_nsec -= 1'000'000'000L;
        }
        if (pthread_timedjoin_np(t, nullptr, &deadline) != 0) {
            std::cerr << "[Dispatcher core=" << core_ << "] warning: " << label
                      << " thread did not stop within 300ms; abandoning it (detached, still running)\n";
            pthread_detach(t);
            return false;
        }
        return true;
    }

    int core_;
    int priority_;
    int efd_;
    int epfd_;
    int idle_efd_;
    int timerfd_;

    std::atomic<bool>    running_;
    std::queue<Subtask*> queue_;
    pthread_mutex_t      queue_mutex_;
    TimerQueue           timer_queue_;
    pthread_mutex_t      timer_mutex_;

    pthread_t thread_;
    pthread_t idle_thread_;

    std::vector<Subtask*> subtasks_;
};
