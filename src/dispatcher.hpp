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
 *   but not the idle thread that drains it: that's shared, one per physical
 *   core, across every Dispatcher pinned there (see core_idle_controller.hpp
 *   — MCFlow Section V-C describes one idle thread per core, not per
 *   dispatcher, which is what CoreIdleController implements).
 * - notify() enforces the fan-in condition before enqueuing.
 * - The 6-step release-guard protocol (Section V-C) is implemented in
 *   process_subtask(), which is also exposed via Demultiplexer::process().
 * - terminate() implements the MCFlow Section V-A termination protocol
 *   (adapted to a single host, no network acks): a subtask stops accepting
 *   new releases, propagates the request to its successors, and
 *   acknowledges TeamManager, all executed at its own real-time priority.
 */

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
class IDispatcher {
public:
    // supplier_bit identifies which predecessor is signalling (see
    // SubtaskConn::supplier_bit); default of 1 (bit 0) is correct for the
    // overwhelming majority of calls, where the target has a single
    // supplier: sources ticked directly by TeamManager::notify, and any
    // hand-built Subtask that never sets fan_in_mask_full beyond its default.
    virtual void notify(struct Subtask* s, uint64_t supplier_bit = 1) = 0;
    virtual void terminate(struct Subtask* s) = 0;
    virtual ~IDispatcher() = default;
};

// -----------------------------------------------------------------------
//  Downstream connection descriptor
// -----------------------------------------------------------------------
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

    Subtask() = default;
    Subtask(int i, std::function<void()> fn)
        : id(i), execute(std::move(fn)) {}

    // Non-copyable: atomic members cannot be copied.
    Subtask(const Subtask&)            = delete;
    Subtask& operator=(const Subtask&) = delete;
};

// -----------------------------------------------------------------------
//  Timer queue support
// -----------------------------------------------------------------------
struct TimerEntry {
    uint64_t release_ns;
    Subtask* subtask;
    bool operator>(const TimerEntry& o) const { return release_ns > o.release_ns; }
};

using TimerQueue = std::priority_queue<TimerEntry,
                                       std::vector<TimerEntry>,
                                       std::greater<TimerEntry>>;

// -----------------------------------------------------------------------
//  Dispatcher
// -----------------------------------------------------------------------
class Dispatcher : public IDispatcher {
public:
    Dispatcher(int core, int priority)
        : core_(core), priority_(priority),
          efd_(-1), epfd_(-1), running_(false) {}

    ~Dispatcher() { stop(); }

    void register_subtask(Subtask* s) { subtasks_.push_back(s); }

    int priority() const { return priority_; }

    // Called by CoreIdleController: pushes a callback that fires whenever
    // process_subtask() defers a subtask into timer_queue_ (see below).
    void set_on_timer_deferred(std::function<void()> cb) {
        on_timer_deferred_ = std::move(cb);
    }

    // Thread-safe peek at this dispatcher's nearest pending release time,
    // without popping it. Used by CoreIdleController to decide, across all
    // dispatchers sharing a core, which one (if any) is ready and which one
    // is the highest-priority dispatcher among the ready ones.
    bool peek_earliest_timer(uint64_t& out) {
        pthread_mutex_lock(&timer_mutex_);
        bool has = !timer_queue_.empty();
        if (has) out = timer_queue_.top().release_ns;
        pthread_mutex_unlock(&timer_mutex_);
        return has;
    }

    /**
     * Enqueue a termination request for subtask s, to be processed like any
     * other job — i.e. at s's own real-time priority, in FIFO order behind
     * whatever genuine work is already queued ahead of it (paper Section
     * V-A: "a termination protocol for each task is executed at the task's
     * real-time priority"). Unlike notify(), this does not wait on the
     * fan-in mask: termination must reach s regardless of whether its data
     * preconditions are currently met.
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
     * Signal that supplier_bit's precondition for subtask s is met for one
     * job. Enqueues s only once every bit of fan_in_mask_full has been set.
     *
     * A bitmask (not a counter) is what makes this safe with 2+ real
     * predecessors: OR-ing the same bit twice before the target fires is a
     * no-op, so a supplier that (for whatever reason) signals twice for one
     * job cannot be mistaken for two distinct suppliers each signalling
     * once, the way a plain increment would.
     * Thread-safe; may be called from any thread.
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

    void start() {
        efd_  = eventfd(0, EFD_SEMAPHORE);
        epfd_ = epoll_create1(0);

        struct epoll_event ev{};
        ev.events  = EPOLLIN;
        ev.data.fd = efd_;
        epoll_ctl(epfd_, EPOLL_CTL_ADD, efd_, &ev);

        pthread_mutex_init(&queue_mutex_, nullptr);
        pthread_mutex_init(&timer_mutex_, nullptr);

        running_ = true;
        pthread_create(&thread_, nullptr, static_loop, this);
    }

    void stop() {
        if (!running_) return;
        running_ = false;

        uint64_t wake = 1;
        ::write(efd_, &wake, sizeof(wake));

        pthread_join(thread_, nullptr);

        close(efd_);  efd_  = -1;
        close(epfd_); epfd_ = -1;

        pthread_mutex_destroy(&queue_mutex_);
        pthread_mutex_destroy(&timer_mutex_);
    }

    // Returns current time in nanoseconds (CLOCK_MONOTONIC).
    static uint64_t monotonic_ns() {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
               static_cast<uint64_t>(ts.tv_nsec);
    }

    // Exposed so Demultiplexer and the idle thread can call it.
    // Implements the 6-step release-guard protocol (paper Section V-C).
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
            // Defer: push into timer queue; let the core's shared idle
            // controller know so it can re-arm its timer (see
            // CoreIdleController::on_timer_queue_changed).
            s->in_processing.store(false);

            pthread_mutex_lock(&timer_mutex_);
            timer_queue_.push({s->next_release_ns, s});
            pthread_mutex_unlock(&timer_mutex_);

            if (on_timer_deferred_) on_timer_deferred_();
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

    // Called by the idle thread: dispatch earliest timer entry if past due.
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

    static void* static_loop(void* arg) {
        static_cast<Dispatcher*>(arg)->loop(); return nullptr;
    }

    int core_;
    int priority_;
    int efd_;
    int epfd_;

    std::atomic<bool>    running_;
    std::queue<Subtask*> queue_;
    pthread_mutex_t      queue_mutex_;
    TimerQueue           timer_queue_;
    pthread_mutex_t      timer_mutex_;

    // Fires whenever process_subtask() defers a subtask into timer_queue_.
    // Wired by CoreIdleController::register_dispatcher(); nullptr (a no-op)
    // if this Dispatcher was never registered with one.
    std::function<void()> on_timer_deferred_;

    pthread_t thread_;

    std::vector<Subtask*> subtasks_;
};
