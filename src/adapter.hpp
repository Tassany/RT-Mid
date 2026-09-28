#pragma once

/**
 * @file adapter.hpp
 *
 * Wires a real ComponentBase instance into a Subtask::execute closure that
 * automatically moves data through ring buffers — the piece MCFlow builds
 * automatically as part of its dispatch mechanism (paper Section V-C: "it
 * copies its outputs to the input queues of its immediate downstream
 * subtasks") and that Dispatcher::process_subtask() does NOT do on its own
 * (it only sends a control signal via notify(), never data).
 *
 * Also implements MCFlow's adapters (Section IV-B): "an adapter is a C or
 * C++ function that takes the output of an upstream component and converts
 * it into the input of a downstream component," for edges where the
 * upstream's output type and downstream's input type differ. Identity
 * (same-type) and adapted (converting) readers/writers are distinct types
 * here rather than one type with a nullable conversion function, so a
 * missing-adapter-on-a-mismatched-edge mistake is a compile error, not a
 * runtime null-pointer call.
 *
 * Multi-supplier fan-in (MultiSupplierRingBuffer): there is no generic way
 * to merge K suppliers' values into one — MCFlow's own model (Fig. 7, "Data
 * Merge") gives the *consumer* all K fields and lets its own execute()
 * decide how to combine them. multi_reader<T,N,K> therefore hands back
 * std::array<T,K> (in supplier-index order); a component consuming a
 * multi-supplier edge must declare its input_type as exactly that.
 *
 * Per-job sequencing: each reader/writer below owns its OWN local counter
 * (starts at 0, increments once per call) instead of sharing one counter
 * across the whole pipeline. This is enough — no cross-edge coordination
 * needed — because every edge is a strict one-write-per-one-read relation
 * per round: a supplier's downstream is notified (and therefore reads)
 * exactly once for every time the supplier itself executes (see
 * Dispatcher::process_subtask()'s fan_in_mask gating), so a writer's local
 * counter and its matching reader's local counter always advance in
 * lockstep, round for round, even when they run on different Dispatcher
 * threads/cores — nothing needs to tell a reader which round a writer just
 * produced. RingBuffer's own consumer_pos_ backpressure (write() spins if
 * the ring is full) is what keeps a supplier from racing more than N rounds
 * ahead of a slower sibling in the multi-supplier case.
 *
 * This file is the only place with real logic — codegen (tools/codegen)
 * only ever instantiates wire_component<...> and the reader/writer
 * templates below with concrete types; it never contains this logic itself.
 */

#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <type_traits>
#include "component_registry.hpp"
#include "dispatcher.hpp"
#include "ring_buffer.hpp"

namespace rtmid {

// --- Compile-time detection of ConcreteType::input_type / output_type ---
// SourceComponent<O,C> has no input_type; SinkComponent<I,C> has no
// output_type (component.hpp) — wire_component uses these to skip the
// missing side via `if constexpr` rather than requiring every component to
// have both.
/**
 * @brief Trait: false unless @p T declares a nested `input_type`.
 * @tparam T Type to test for an `input_type` member typedef.
 */
template<typename, typename = void>
struct has_input_type : std::false_type {};
/** @brief Specialization: true when T::input_type exists. */
template<typename T>
struct has_input_type<T, std::void_t<typename T::input_type>> : std::true_type {};

/**
 * @brief Trait: false unless @p T declares a nested `output_type`.
 * @tparam T Type to test for an `output_type` member typedef.
 */
template<typename, typename = void>
struct has_output_type : std::false_type {};
/** @brief Specialization: true when T::output_type exists. */
template<typename T>
struct has_output_type<T, std::void_t<typename T::output_type>> : std::true_type {};

// Placeholder passed as the UpstreamReader argument for a source node (no
// upstream). Never actually invoked: wire_component only calls `upstream()`
// under `if constexpr (has_input_type<ConcreteType>::value)`, which is false
// for a SourceComponent.
/**
 * @brief Placeholder UpstreamReader for a source node with no upstream.
 *
 * Never actually invoked: wire_component() only calls `upstream()` under
 * `if constexpr (has_input_type<ConcreteType>::value)`, which is false
 * for a SourceComponent.
 */
struct no_upstream {
    /**
     * @brief No-op call operator; never invoked in practice.
     * @return void
     */
    void operator()() const {}
};

// --- SPSC edges (single upstream, single downstream) ---

/**
 * @brief Reads one value per call from a single-producer ring buffer.
 *
 * Owns a local, thread-confined sequence counter that advances once per
 * call, matched round-for-round with the paired writer.
 *
 * @tparam T Value type stored in the ring buffer.
 * @tparam N Ring buffer slot count.
 */
template<typename T, std::size_t N>
class spsc_reader {
public:
    /**
     * @brief Wraps a ring buffer to read from.
     * @param buf Ring buffer this reader consumes; must outlive the reader.
     */
    explicit spsc_reader(RingBuffer<T, N>& buf) : buf_(buf) {}
    /**
     * @brief Reads and releases the next value in sequence.
     * @return The value written by the paired writer for this round.
     */
    T operator()() const {
        std::size_t j = seq_++;
        T v = buf_.read(j);
        buf_.release(j);
        return v;
    }
private:
    RingBuffer<T, N>& buf_;
    // Thread-confined: only ever called from the one Dispatcher thread that
    // runs this reader's owning Subtask, one call per execution — never
    // concurrent, so a plain counter (not atomic) is enough. `mutable` so
    // operator() can stay const, matching how it's captured (by value, into
    // a non-mutable lambda) in wire_component().
    mutable std::size_t seq_ = 0;
};

// Identity writer: upstream output type and downstream input type are the
// same T.
/**
 * @brief Identity writer: upstream output type equals downstream input
 *        type T.
 * @tparam T Value type stored in the ring buffer.
 * @tparam N Ring buffer slot count.
 */
template<typename T, std::size_t N>
class spsc_writer {
public:
    /**
     * @brief Wraps a ring buffer to write into.
     * @param buf Ring buffer this writer feeds; must outlive the writer.
     */
    explicit spsc_writer(RingBuffer<T, N>& buf) : buf_(buf) {}
    /**
     * @brief Binds the producer subtask's terminating flag, so a write
     *        stuck on backpressure can abort instead of spinning forever.
     *        Called by wire_component() once the Subtask exists; unbound
     *        (nullptr) spins unconditionally, as before.
     * @param flag Producer's own Subtask::terminating flag.
     * @return void
     */
    void bind_shutdown_flag(const std::atomic<bool>& flag) { should_abort_ = &flag; }
    /**
     * @brief Writes the next value in sequence, blocking if the buffer
     *        is full.
     * @param v Value to write for this round.
     * @return void
     * @throws WriteAbortedOnShutdown see bind_shutdown_flag().
     */
    void operator()(const T& v) const {
        std::size_t j = seq_++;
        buf_.write(j, v, should_abort_);
    }
private:
    RingBuffer<T, N>& buf_;
    const std::atomic<bool>* should_abort_ = nullptr;
    mutable std::size_t seq_ = 0;
};

// Adapted writer: converts UpstreamT -> DownstreamT via a mandatory function
// pointer (MCFlow Section IV-B). No default/null adapter — an edge that
// needs one must always provide it, by construction, not by convention.
/**
 * @brief Writer that converts UpstreamT to DownstreamT via an adapter
 *        function (MCFlow Section IV-B).
 *
 * No default/null adapter: an edge whose types differ must always supply
 * one by construction, not by convention.
 *
 * @tparam UpstreamT Type produced by the upstream component.
 * @tparam DownstreamT Type expected by the downstream component.
 * @tparam N Ring buffer slot count.
 */
template<typename UpstreamT, typename DownstreamT, std::size_t N>
class spsc_adapted_writer {
public:
    /** @brief Function pointer converting UpstreamT to DownstreamT. */
    using AdaptFn = DownstreamT (*)(const UpstreamT&);
    /**
     * @brief Wraps a ring buffer and the adapter function to write with.
     * @param buf Ring buffer this writer feeds; must outlive the writer.
     * @param adapt Function converting an UpstreamT value to DownstreamT.
     */
    spsc_adapted_writer(RingBuffer<DownstreamT, N>& buf, AdaptFn adapt)
        : buf_(buf), adapt_(adapt) {}
    /** @brief See spsc_writer::bind_shutdown_flag(). */
    void bind_shutdown_flag(const std::atomic<bool>& flag) { should_abort_ = &flag; }
    /**
     * @brief Converts and writes the next value in sequence.
     * @param v Upstream value to convert and write for this round.
     * @return void
     * @throws WriteAbortedOnShutdown see bind_shutdown_flag().
     */
    void operator()(const UpstreamT& v) const {
        std::size_t j = seq_++;
        buf_.write(j, adapt_(v), should_abort_);
    }
private:
    RingBuffer<DownstreamT, N>& buf_;
    AdaptFn adapt_;
    const std::atomic<bool>* should_abort_ = nullptr;
    mutable std::size_t seq_ = 0;
};

// --- Multi-supplier edges (K upstreams into one downstream) ---

/**
 * @brief Reads the merged values of all K suppliers for a fan-in edge.
 *
 * MCFlow's model (Fig. 7, "Data Merge") gives the consumer all K fields
 * rather than merging them generically; this reader hands back an
 * std::array<T,K> in supplier-index order for the consumer's own
 * execute() to combine as it sees fit.
 *
 * @tparam T Value type each supplier writes.
 * @tparam N Ring buffer slot count.
 * @tparam K Number of suppliers feeding this edge.
 */
template<typename T, std::size_t N, std::size_t K>
class multi_reader {
public:
    /**
     * @brief Wraps a multi-supplier ring buffer to read from.
     * @param buf Buffer this reader consumes; must outlive the reader.
     */
    explicit multi_reader(MultiSupplierRingBuffer<T, N, K>& buf) : buf_(buf) {}
    // Precondition: buf_.ready(seq) — callers only read once the Dispatcher's
    // own fan_in_mask has already confirmed every supplier notified, so this
    // is expected to already hold by construction (see tests/adapter_test.cpp
    // for a direct check of that agreement).
    /**
     * @brief Reads and releases all K suppliers' values for this round.
     *
     * Precondition: the underlying slot is ready (every supplier has
     * written), which the Dispatcher's fan_in_mask gating guarantees
     * before this is called.
     *
     * @return Array of K values in supplier-index order.
     */
    std::array<T, K> operator()() const {
        std::size_t j = seq_++;
        std::array<T, K> out{};
        for (std::size_t i = 0; i < K; ++i) out[i] = buf_.read(j, i);
        buf_.release(j);
        return out;
    }
private:
    MultiSupplierRingBuffer<T, N, K>& buf_;
    mutable std::size_t seq_ = 0;
};

/**
 * @brief One supplier's identity writer into a fan-in edge.
 * @tparam T Value type this supplier writes.
 * @tparam N Ring buffer slot count.
 * @tparam K Number of suppliers feeding this edge.
 */
template<typename T, std::size_t N, std::size_t K>
class multi_writer {
public:
    /**
     * @brief Wraps a multi-supplier buffer and this supplier's index.
     * @param buf Buffer this writer feeds; must outlive the writer.
     * @param supplier_id 0-based index identifying this supplier.
     */
    multi_writer(MultiSupplierRingBuffer<T, N, K>& buf, std::size_t supplier_id)
        : buf_(buf), supplier_id_(supplier_id) {}
    /** @brief See spsc_writer::bind_shutdown_flag(). */
    void bind_shutdown_flag(const std::atomic<bool>& flag) { should_abort_ = &flag; }
    /**
     * @brief Writes this supplier's value for the next round.
     * @param v Value to write for this round.
     * @return void
     * @throws WriteAbortedOnShutdown see bind_shutdown_flag().
     */
    void operator()(const T& v) const {
        std::size_t j = seq_++;
        buf_.write(j, supplier_id_, v, should_abort_);
    }
private:
    MultiSupplierRingBuffer<T, N, K>& buf_;
    std::size_t supplier_id_;
    const std::atomic<bool>* should_abort_ = nullptr;
    mutable std::size_t seq_ = 0;
};

/**
 * @brief One supplier's type-converting writer into a fan-in edge.
 * @tparam UpstreamT Type produced by this supplier's component.
 * @tparam DownstreamT Type expected by the downstream component.
 * @tparam N Ring buffer slot count.
 * @tparam K Number of suppliers feeding this edge.
 */
template<typename UpstreamT, typename DownstreamT, std::size_t N, std::size_t K>
class multi_adapted_writer {
public:
    /** @brief Function pointer converting UpstreamT to DownstreamT. */
    using AdaptFn = DownstreamT (*)(const UpstreamT&);
    /**
     * @brief Wraps a multi-supplier buffer, supplier index, and adapter.
     * @param buf Buffer this writer feeds; must outlive the writer.
     * @param supplier_id 0-based index identifying this supplier.
     * @param adapt Function converting an UpstreamT value to DownstreamT.
     */
    multi_adapted_writer(MultiSupplierRingBuffer<DownstreamT, N, K>& buf, std::size_t supplier_id, AdaptFn adapt)
        : buf_(buf), supplier_id_(supplier_id), adapt_(adapt) {}
    /** @brief See spsc_writer::bind_shutdown_flag(). */
    void bind_shutdown_flag(const std::atomic<bool>& flag) { should_abort_ = &flag; }
    /**
     * @brief Converts and writes this supplier's value for the next
     *        round.
     * @param v Upstream value to convert and write for this round.
     * @return void
     * @throws WriteAbortedOnShutdown see bind_shutdown_flag().
     */
    void operator()(const UpstreamT& v) const {
        std::size_t j = seq_++;
        buf_.write(j, supplier_id_, adapt_(v), should_abort_);
    }
private:
    MultiSupplierRingBuffer<DownstreamT, N, K>& buf_;
    std::size_t supplier_id_;
    AdaptFn adapt_;
    const std::atomic<bool>* should_abort_ = nullptr;
    mutable std::size_t seq_ = 0;
};

// --- Component -> Subtask adapter ---

/**
 * @brief A constructed component instance bundled with its runnable
 *        Subtask.
 * @var WiredNode::instance Owns the ComponentBase* and its config.
 * @var WiredNode::subtask Owns the Subtask; TeamManager only borrows it.
 */
struct WiredNode {
    ComponentInstance        instance; // owns the ComponentBase* and its config
    std::unique_ptr<Subtask> subtask;  // owns the Subtask; TeamManager only borrows it
};

// Builds a Subtask whose execute() reads via `upstream` (skipped for a
// source: has_input_type<ConcreteType> is false), calls the real
// component's execute(), and fans output_ out to every `downstream` writer
// (skipped for a sink). `upstream`/`downstream...` are any callable matching
// the reader/writer shapes above (or a hand-written equivalent, e.g. in
// tests) — wire_component itself never touches a RingBuffer directly.
/**
 * @brief Builds a real component and wires it into a runnable Subtask.
 *
 * Constructs @p ConcreteType directly via its config_type alias,
 * bypassing ComponentRegistry (needed only by
 * JsonParser::validate_components, which resolves an
 * unknown-at-compile-time component_type string). Wires a Subtask whose
 * execute() reads @p upstream, runs the component, and fans output_ to
 * each @p downstream writer; source/sink steps are skipped via
 * has_input_type/has_output_type.
 *
 * @tparam ConcreteType Concrete component class to construct.
 * @tparam UpstreamReader Callable providing this component's input.
 * @tparam DownstreamWriters Callables receiving this component's output,
 *         one per edge.
 * @param id This subtask's id.
 * @param config JSON config parsed into ConcreteType::config_type.
 * @param upstream Supplies input_; ignored for a source.
 * @param downstream Receives output_; ignored for a sink.
 * @return WiredNode owning the component and its Subtask.
 */
template<typename ConcreteType, typename UpstreamReader, typename... DownstreamWriters>
WiredNode wire_component(int id, const nlohmann::json& config,
                          UpstreamReader upstream,
                          DownstreamWriters... downstream) {
    using ConfigType = typename ConcreteType::config_type;
    auto cfg = std::make_shared<ConfigType>(config.get<ConfigType>());
    auto component = std::make_unique<ConcreteType>(cfg.get());
    auto* concrete = component.get();

    WiredNode node;
    node.instance.config    = cfg;
    node.instance.component = std::move(component);

    // Subtask constructed BEFORE exec (not after, as previously) so each
    // downstream writer can be bound to THIS subtask's own `terminating`
    // flag before being captured into exec's closure — see
    // ring_buffer.hpp's WriteAbortedOnShutdown: a write stuck spinning on
    // backpressure aborts once its own producer subtask has been asked to
    // stop, rather than risking Dispatcher::stop()'s pthread_join blocking
    // forever on a consumer that may never drain again.
    node.subtask = std::make_unique<Subtask>();
    node.subtask->id = id;
    (downstream.bind_shutdown_flag(node.subtask->terminating), ...);

    auto exec = [concrete, upstream, downstream...]() {
        if constexpr (has_input_type<ConcreteType>::value)
            concrete->init_input(upstream());
        concrete->execute();
        if constexpr (has_output_type<ConcreteType>::value)
            (downstream(concrete->output_), ...);
    };
    node.subtask->execute = std::function<void()>(exec);

    return node;
}

} // namespace rtmid
