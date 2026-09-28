#ifndef RING_BUFFER_HPP
#define RING_BUFFER_HPP

/**
 * @file ring_buffer.hpp
 *
 * Provides two lock-free ring buffer types for inter-subtask communication,
 * following the MCFlow paper (Huang et al., 2012).
 *
 * Key design points
 * -----------------
 * - Each slot is aligned to and padded up to a full cache-line boundary
 *   (alignas(64)) to eliminate false sharing between adjacent slots.
 * - The consumer position counter lives on its own cache line, separate from
 *   the slots array, to avoid false sharing between the producer (reading
 *   consumer_pos_ for backpressure) and the consumer (writing it).
 * - Backpressure: write() spins until the target slot is free, i.e. until
 *   the consumer has called release() for the oldest outstanding job.
 *   N must therefore be larger than the maximum pipeline depth so that
 *   blocking is rare under normal conditions.
 * - Shutdown: that spin has no bound of its own — if the consumer subtask
 *   never runs again (e.g. it's stuck waiting on a shared, low-priority
 *   idle thread under heavy load), the producer would spin forever, and
 *   Dispatcher::stop()'s pthread_join on that producer's thread would
 *   block forever with it. write() takes an optional pointer to the
 *   PRODUCER's own Subtask::terminating flag (bound by
 *   adapter.hpp's wire_component); once that's set — which
 *   Dispatcher::terminate() does immediately, before any ack-waiting —
 *   the spin throws WriteAbortedOnShutdown instead of continuing to wait
 *   on a consumer that may never drain again. nullptr (the default)
 *   preserves the old unbounded-spin behavior for any caller that isn't
 *   wired to a Subtask's lifecycle.
 *
 * RingBuffer<T, N>
 *   Single-producer / single-consumer (SPSC).
 *
 * MultiSupplierRingBuffer<T, N, NumSuppliers>
 *   Multiple producers / single consumer (fan-in).
 *   Each slot has one field per supplier; the slot is ready for consumption
 *   only when every supplier has written (tracked via an atomic bitmask).
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>

static constexpr size_t CACHE_LINE_SIZE = 64; // Specific for x86-64

/**
 * @brief Thrown by RingBuffer/MultiSupplierRingBuffer::write() when the
 *        producer's own subtask is terminating while the write is still
 *        spinning on backpressure.
 *
 * Expected during shutdown, not a fault — TeamManager::initialize()'s
 * per-subtask execute() wrapper (team_manager.cpp) catches this
 * separately from a genuine exception and does not treat it as one.
 */
struct WriteAbortedOnShutdown : std::exception {
    const char* what() const noexcept override {
        return "RingBuffer::write aborted: producer subtask is terminating";
    }
};

// ============================================================
//  Ring buffer sizing formula  (paper Section IV)
//
//  N = next_pow2(max(2, ceil(deadline_downstream / period_upstream)
//                         + pipeline_depth))
//
//  next_pow2 is required because RingBuffer uses a power-of-2 mask
//  to wrap indices without a modulo operation.
//
//  Use ring_buffer_n() wherever you need the compile-time N:
//    - TeamManager::ring_buffer_size()  — advisory, shows required N
//    - tools/codegen                    — emits RingBuffer<T, N> declarations
// ============================================================

/**
 * @brief Rounds a value up to the next power of two.
 *
 * Required because RingBuffer/MultiSupplierRingBuffer use a power-of-2
 * mask to wrap indices without a modulo operation.
 *
 * @param n Value to round up; 0 is treated as requiring a result of 1.
 * @return Smallest power of two that is >= @p n (1 if n == 0).
 */
inline constexpr std::size_t ring_buffer_next_pow2(std::size_t n) noexcept {
    if (n == 0) return 1;
    std::size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

// period_up == 0 means aperiodic upstream: uses depth + 2 as the base.
/**
 * @brief Computes the required compile-time slot count N for a connection.
 *
 * Formula (paper Section IV): N = next_pow2(max(2,
 * ceil(deadline_down_ns / period_up_ns) + pipeline_depth)). A pure ratio,
 * so it's unit-agnostic as long as both arguments use the same unit —
 * callers currently pass SubtaskInfo's period_us/deadline_us directly.
 * Used by TeamManager::ring_buffer_size() (advisory) and tools/codegen (to
 * emit RingBuffer<T, N> declarations).
 *
 * @param period_up_ns Upstream subtask's period; 0 means an aperiodic
 *        upstream, in which case the base is pipeline_depth + 2 instead of
 *        the ceil-division formula. Any time unit works, as long as
 *        deadline_down_ns uses the same one.
 * @param deadline_down_ns Downstream subtask's relative deadline, in the
 *        same unit as period_up_ns.
 * @param pipeline_depth Worst-case simultaneous occupancy across all
 *        pipeline stages (DAG::pipeline_depth()).
 * @return Smallest power-of-two slot count satisfying the sizing formula.
 */
inline constexpr std::size_t ring_buffer_n(
    std::uint64_t period_up_ns,
    std::uint64_t deadline_down_ns,
    int           pipeline_depth) noexcept
{
    std::size_t base = 0;
    if (period_up_ns == 0) {
        base = static_cast<std::size_t>(pipeline_depth) + 2;
    } else {
        std::size_t jif = (deadline_down_ns + period_up_ns - 1) / period_up_ns;
        base = (jif + static_cast<std::size_t>(pipeline_depth));
        if (base < 2) base = 2;
    }
    return ring_buffer_next_pow2(base);
}

// ============================================================
//  RingBuffer<T, N> sequence-number addressed
// ============================================================

/**
 * @brief Single-producer/single-consumer lock-free ring buffer.
 *
 * Slots are addressed by monotonically increasing sequence number modulo
 * N (a power of 2). Each slot is cache-line aligned and padded to avoid
 * false sharing; the consumer position lives on its own cache line,
 * separate from the slot array.
 *
 * @tparam T Type of value stored per slot.
 * @tparam N Number of slots; must be a power of 2, at least 2. Should be
 *         sized via ring_buffer_n() to exceed the maximum pipeline depth
 *         so write() rarely blocks.
 */
template<typename T, size_t N>
class RingBuffer {
    static_assert((N & (N - 1)) == 0, "N must be a power of 2");
    static_assert(N >= 2,             "N must be at least 2");

    // alignas(64) forces sizeof(Slot) to be a multiple of 64.
    struct alignas(CACHE_LINE_SIZE) Slot { T data; };
    static_assert(sizeof(Slot) % CACHE_LINE_SIZE == 0,
                  "Slot must be a multiple of the cache-line size");

    Slot slots_[N];

    // On its own cache line: written by consumer, read by producer.
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> consumer_pos_{0};

public:
    /**
     * @brief Writes a value for a job, blocking until the slot is free.
     *
     * Spins (backpressure) until the target slot is free, i.e. until the
     * consumer has released the oldest outstanding job. Must be called
     * before notifying the consumer dispatcher.
     *
     * @param seq_num Monotonically increasing sequence number of the job.
     * @param value Value to store into the slot for @p seq_num.
     * @param should_abort Optional pointer to the producer's own
     *        Subtask::terminating flag; once it's set, the spin throws
     *        WriteAbortedOnShutdown instead of continuing to wait.
     *        nullptr (default) spins unconditionally, as before.
     * @return void
     * @throws WriteAbortedOnShutdown if @p should_abort becomes set while
     *         still spinning.
     */
    void write(size_t seq_num, const T& value, const std::atomic<bool>* should_abort = nullptr) {
        while (seq_num >= consumer_pos_.load(std::memory_order_acquire) + N) {
            // spin: producer waits until consumer releases an old slot
            if (should_abort && should_abort->load(std::memory_order_acquire))
                throw WriteAbortedOnShutdown{};
        }
        slots_[seq_num & (N - 1)].data = value;
    }

    /**
     * @brief Accesses the slot for a job by sequence number.
     * @param seq_num Sequence number of the job to read.
     * @return Reference to the slot's data; valid until release() is
     *         called for this @p seq_num.
     */
    T& read(size_t seq_num) {
        return slots_[seq_num & (N - 1)].data;
    }

    /**
     * @brief Marks a slot as consumed so the producer can reuse it.
     *
     * Must be called after the consumer is done with the data; allows the
     * producer to reuse the slot for job seq_num + N.
     *
     * @param seq_num Sequence number of the job whose slot is released.
     * @return void
     */
    void release(size_t seq_num) {
        consumer_pos_.store(seq_num + 1, std::memory_order_release);
    }

    // Slot size in bytes — useful for tests and static_assert verification.
    /**
     * @brief Reports the padded size of one slot, in bytes.
     * @return sizeof(Slot); useful for tests and static_assert checks.
     */
    static constexpr size_t slot_size() { return sizeof(Slot); }
};


// ============================================================
//  MultiSupplierRingBuffer<T, N, NumSuppliers>  —  fan-in
// ============================================================

/**
 * @brief Lock-free multi-producer/single-consumer ring buffer (fan-in).
 *
 * Each slot has one data field per supplier plus an atomic ready-bit
 * mask; a slot is ready for consumption only once every supplier has
 * written its value.
 *
 * @tparam T Type of value stored per supplier per slot.
 * @tparam N Number of slots; must be a power of 2, at least 2.
 * @tparam NumSuppliers Number of producers feeding this buffer; must be
 *         in [1, 64].
 */
template<typename T, size_t N, size_t NumSuppliers>
class MultiSupplierRingBuffer {
    static_assert((N & (N - 1)) == 0, "N must be a power of 2");
    static_assert(N >= 2,             "N must be at least 2");
    static_assert(NumSuppliers >= 1 && NumSuppliers <= 64,
                  "NumSuppliers must be in [1, 64]");

    static constexpr uint64_t FULL_MASK =
        (NumSuppliers == 64) ? ~uint64_t(0)
                             : ((uint64_t(1) << NumSuppliers) - 1);

    // Each slot stores one data field per supplier plus an atomic bitmask.
    struct alignas(CACHE_LINE_SIZE) Slot {
        T data[NumSuppliers];
        std::atomic<uint64_t> ready_mask{0};
    };
    static_assert(sizeof(Slot) % CACHE_LINE_SIZE == 0,
                  "Slot must be a multiple of the cache-line size");

    Slot slots_[N];
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> consumer_pos_{0};

public:
    /**
     * @brief One supplier writes its value for a job, blocking if full.
     *
     * Sets the corresponding bit in the slot's ready_mask. Spins
     * (backpressure) until the slot is free.
     *
     * @param seq_num Sequence number of the job being supplied.
     * @param supplier_id 0-based index of the writing supplier.
     * @param value Value to store for this supplier and @p seq_num.
     * @param should_abort Optional pointer to the producer's own
     *        Subtask::terminating flag; see RingBuffer::write().
     * @return void
     * @throws WriteAbortedOnShutdown if @p should_abort becomes set while
     *         still spinning.
     */
    void write(size_t seq_num, size_t supplier_id, const T& value,
               const std::atomic<bool>* should_abort = nullptr) {
        while (seq_num >= consumer_pos_.load(std::memory_order_acquire) + N) {
            if (should_abort && should_abort->load(std::memory_order_acquire))
                throw WriteAbortedOnShutdown{};
        }
        Slot& slot = slots_[seq_num & (N - 1)];
        slot.data[supplier_id] = value;
        slot.ready_mask.fetch_or(uint64_t(1) << supplier_id,
                                 std::memory_order_release);
    }

    /**
     * @brief Checks whether every supplier has written for a job.
     * @param seq_num Sequence number of the job to check.
     * @return true once all NumSuppliers have written their value for
     *         @p seq_num.
     */
    bool ready(size_t seq_num) const {
        return slots_[seq_num & (N - 1)].ready_mask.load(
                   std::memory_order_acquire) == FULL_MASK;
    }

    /**
     * @brief Reads the value written by one supplier for a job.
     *
     * Call only after ready() returns true for @p seq_num.
     *
     * @param seq_num Sequence number of the job to read.
     * @param supplier_id 0-based index of the supplier whose value to read.
     * @return Const reference to that supplier's stored value.
     */
    const T& read(size_t seq_num, size_t supplier_id) const {
        return slots_[seq_num & (N - 1)].data[supplier_id];
    }

    /**
     * @brief Marks a slot as consumed so suppliers can reuse it.
     *
     * Resets the slot's ready bitmask and advances consumer_pos_ so the
     * suppliers can reuse the slot for job seq_num + N.
     *
     * @param seq_num Sequence number of the job whose slot is released.
     * @return void
     */
    void release(size_t seq_num) {
        slots_[seq_num & (N - 1)].ready_mask.store(0,
            std::memory_order_release);
        consumer_pos_.store(seq_num + 1, std::memory_order_release);
    }

    /**
     * @brief Reports the padded size of one slot, in bytes.
     * @return sizeof(Slot); useful for tests and static_assert checks.
     */
    static constexpr size_t slot_size() { return sizeof(Slot); }
};

#endif // RING_BUFFER_HPP
