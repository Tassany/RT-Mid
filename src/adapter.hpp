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
template<typename, typename = void>
struct has_input_type : std::false_type {};
template<typename T>
struct has_input_type<T, std::void_t<typename T::input_type>> : std::true_type {};

template<typename, typename = void>
struct has_output_type : std::false_type {};
template<typename T>
struct has_output_type<T, std::void_t<typename T::output_type>> : std::true_type {};

// Placeholder passed as the UpstreamReader argument for a source node (no
// upstream). Never actually invoked: wire_component only calls `upstream()`
// under `if constexpr (has_input_type<ConcreteType>::value)`, which is false
// for a SourceComponent.
struct no_upstream {
    void operator()() const {}
};

// --- SPSC edges (single upstream, single downstream) ---

template<typename T, std::size_t N>
class spsc_reader {
public:
    explicit spsc_reader(RingBuffer<T, N>& buf) : buf_(buf) {}
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
template<typename T, std::size_t N>
class spsc_writer {
public:
    explicit spsc_writer(RingBuffer<T, N>& buf) : buf_(buf) {}
    void operator()(const T& v) const {
        std::size_t j = seq_++;
        buf_.write(j, v);
    }
private:
    RingBuffer<T, N>& buf_;
    mutable std::size_t seq_ = 0;
};

// Adapted writer: converts UpstreamT -> DownstreamT via a mandatory function
// pointer (MCFlow Section IV-B). No default/null adapter — an edge that
// needs one must always provide it, by construction, not by convention.
template<typename UpstreamT, typename DownstreamT, std::size_t N>
class spsc_adapted_writer {
public:
    using AdaptFn = DownstreamT (*)(const UpstreamT&);
    spsc_adapted_writer(RingBuffer<DownstreamT, N>& buf, AdaptFn adapt)
        : buf_(buf), adapt_(adapt) {}
    void operator()(const UpstreamT& v) const {
        std::size_t j = seq_++;
        buf_.write(j, adapt_(v));
    }
private:
    RingBuffer<DownstreamT, N>& buf_;
    AdaptFn adapt_;
    mutable std::size_t seq_ = 0;
};

// --- Multi-supplier edges (K upstreams into one downstream) ---

template<typename T, std::size_t N, std::size_t K>
class multi_reader {
public:
    explicit multi_reader(MultiSupplierRingBuffer<T, N, K>& buf) : buf_(buf) {}
    // Precondition: buf_.ready(seq) — callers only read once the Dispatcher's
    // own fan_in_mask has already confirmed every supplier notified, so this
    // is expected to already hold by construction (see tests/adapter_test.cpp
    // for a direct check of that agreement).
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

template<typename T, std::size_t N, std::size_t K>
class multi_writer {
public:
    multi_writer(MultiSupplierRingBuffer<T, N, K>& buf, std::size_t supplier_id)
        : buf_(buf), supplier_id_(supplier_id) {}
    void operator()(const T& v) const {
        std::size_t j = seq_++;
        buf_.write(j, supplier_id_, v);
    }
private:
    MultiSupplierRingBuffer<T, N, K>& buf_;
    std::size_t supplier_id_;
    mutable std::size_t seq_ = 0;
};

template<typename UpstreamT, typename DownstreamT, std::size_t N, std::size_t K>
class multi_adapted_writer {
public:
    using AdaptFn = DownstreamT (*)(const UpstreamT&);
    multi_adapted_writer(MultiSupplierRingBuffer<DownstreamT, N, K>& buf, std::size_t supplier_id, AdaptFn adapt)
        : buf_(buf), supplier_id_(supplier_id), adapt_(adapt) {}
    void operator()(const UpstreamT& v) const {
        std::size_t j = seq_++;
        buf_.write(j, supplier_id_, adapt_(v));
    }
private:
    MultiSupplierRingBuffer<DownstreamT, N, K>& buf_;
    std::size_t supplier_id_;
    AdaptFn adapt_;
    mutable std::size_t seq_ = 0;
};

// --- Component -> Subtask adapter ---

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
template<typename ConcreteType, typename UpstreamReader, typename... DownstreamWriters>
WiredNode wire_component(int id, const std::string& component_type,
                          const nlohmann::json& config,
                          UpstreamReader upstream,
                          DownstreamWriters... downstream) {
    WiredNode node;
    node.instance = ComponentRegistry::instance().create(component_type, config);
    auto* concrete = static_cast<ConcreteType*>(node.instance.component.get());

    auto exec = [concrete, upstream, downstream...]() {
        if constexpr (has_input_type<ConcreteType>::value)
            concrete->init_input(upstream());
        concrete->execute();
        if constexpr (has_output_type<ConcreteType>::value)
            (downstream(concrete->output_), ...);
    };

    node.subtask = std::make_unique<Subtask>(id, std::function<void()>(exec));
    return node;
}

} // namespace rtmid
