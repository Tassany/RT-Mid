#pragma once

/**
 * @file core_idle_controller.hpp
 *
 * One idle thread per physical core, shared by every Dispatcher pinned to
 * that core — matches MCFlow's design (Huang et al. 2012, Section V-C),
 * replacing RT-Mid's earlier per-(core,priority) idle thread.
 *
 * Each Dispatcher on a core still owns its own timer_queue_ (deferred
 * periodic releases waiting for next_release_ns). This controller never
 * touches that queue's contents directly; it only asks each registered
 * Dispatcher "do you have an expired entry?" (peek_earliest_timer) and, if
 * so, tells that Dispatcher to drain it (dispatch_expired_timers()) — that
 * call re-enters the Dispatcher's own queue_/efd_, resources this
 * controller never owns or destroys itself.
 *
 * Priority ordering: dispatchers_ is sorted highest-priority-first at
 * start(). Each wake only ever services the highest-priority dispatcher
 * that has an expired entry, matching MCFlow's "idle notification [goes]
 * to the highest priority dispatcher that has a nonzero timer queue size."
 *
 * Shutdown ordering: TeamManager stops every CoreIdleController before
 * stopping any Dispatcher (see team_manager.cpp). That is the invariant
 * that keeps this controller from ever touching a Dispatcher's queue_mutex_
 * or efd_ after Dispatcher::stop() has destroyed/closed them.
 */

#include <pthread.h>
#include <sched.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <vector>
#include "dispatcher.hpp"

class CoreIdleController {
public:
    explicit CoreIdleController(int core) : core_(core), running_(false) {}
    ~CoreIdleController() { stop(); }

    // Must be called for every Dispatcher pinned to this core, before start().
    void register_dispatcher(Dispatcher* d) {
        dispatchers_.push_back(d);
        d->set_on_timer_deferred([this]() { on_timer_queue_changed(); });
    }

    void start() {
        std::sort(dispatchers_.begin(), dispatchers_.end(),
                   [](Dispatcher* a, Dispatcher* b) { return a->priority() > b->priority(); });

        wake_efd_ = eventfd(0, EFD_SEMAPHORE);
        timerfd_  = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        epfd_     = epoll_create1(0);

        struct epoll_event ev{};
        ev.events  = EPOLLIN;
        ev.data.fd = wake_efd_;
        epoll_ctl(epfd_, EPOLL_CTL_ADD, wake_efd_, &ev);
        ev.data.fd = timerfd_;
        epoll_ctl(epfd_, EPOLL_CTL_ADD, timerfd_, &ev);

        running_ = true;
        pthread_create(&thread_, nullptr, static_loop, this);
    }

    void stop() {
        if (!running_) return;
        running_ = false;
        uint64_t wake = 1;
        ::write(wake_efd_, &wake, sizeof(wake));
        pthread_join(thread_, nullptr);
        close(wake_efd_);
        close(timerfd_);
        close(epfd_);
    }

    // Called by any registered Dispatcher (from its own thread) whenever it
    // pushes an entry into its timer_queue_. Cheap: just nudges the idle
    // thread to re-scan and re-arm timerfd_; the scan itself happens on the
    // controller's own thread in rearm_and_dispatch().
    void on_timer_queue_changed() {
        if (!running_) return;
        uint64_t sig = 1;
        ::write(wake_efd_, &sig, sizeof(sig));
    }

private:
    void rearm_and_dispatch() {
        uint64_t now = Dispatcher::monotonic_ns();
        uint64_t earliest = std::numeric_limits<uint64_t>::max();

        for (Dispatcher* d : dispatchers_) {
            uint64_t t;
            if (!d->peek_earliest_timer(t)) continue;
            if (t <= now) {
                // Service only the highest-priority ready dispatcher this
                // pass; the next wake (triggered by this dispatch itself,
                // or by the next epoll tick) picks up whatever is left.
                d->dispatch_expired_timers();
                return;
            }
            earliest = std::min(earliest, t);
        }

        struct itimerspec its{};
        if (earliest != std::numeric_limits<uint64_t>::max()) {
            its.it_value.tv_sec  = earliest / 1'000'000'000ULL;
            its.it_value.tv_nsec = earliest % 1'000'000'000ULL;
        } // else: all-zero itimerspec disarms the timer — nothing pending
        timerfd_settime(timerfd_, TFD_TIMER_ABSTIME, &its, nullptr);
    }

    void loop() {
        cpu_set_t mask;
        CPU_ZERO(&mask);
        CPU_SET(core_, &mask);
        pthread_setaffinity_np(pthread_self(), sizeof(mask), &mask);

        struct sched_param param{};
        param.sched_priority = 1; // minimum SCHED_FIFO priority, shared core-wide
        pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);

        struct epoll_event events[2];
        while (running_) {
            int n = epoll_wait(epfd_, events, 2, /*timeout_ms=*/10);
            for (int i = 0; i < n; ++i) {
                uint64_t val;
                ::read(events[i].data.fd, &val, sizeof(val)); // drain token
            }
            rearm_and_dispatch();
        }
    }

    static void* static_loop(void* arg) {
        static_cast<CoreIdleController*>(arg)->loop();
        return nullptr;
    }

    int core_;
    std::atomic<bool> running_;
    int wake_efd_ = -1;
    int timerfd_  = -1;
    int epfd_     = -1;
    pthread_t thread_;

    // Only mutated during registration (before start(), single-threaded from
    // TeamManager's side) and only read afterwards from this controller's
    // own thread — no concurrent access, so no lock is needed around it.
    std::vector<Dispatcher*> dispatchers_;
};
