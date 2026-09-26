// Unit test for src/adapter.hpp: proves wire_component() plus the SPSC and
// multi-supplier reader/writer templates actually move (and, where an
// adapter is used, convert) data correctly through real
// RingBuffer/MultiSupplierRingBuffer instances — no DAG, Dispatcher, or
// TeamManager involved, just the wiring layer itself, driven by calling the
// produced Subtask::execute() closures directly in order.
// Build: g++ -std=c++17 -Isrc -Iinclude tests/adapter_test.cpp -o /tmp/adapter_test

#include <array>
#include <cstdlib>
#include <iostream>
#include "adapter.hpp"

struct EmptyConfig {};
inline void from_json(const nlohmann::json&, EmptyConfig&) {}

static void expect(bool cond, const char* what) {
    if (!cond) { std::cerr << "FAIL: " << what << "\n"; std::exit(1); }
    std::cerr << "ok:   " << what << "\n";
}

// --- (a) SPSC identity path ---
class SrcDouble : public SourceComponent<double, EmptyConfig> {
public:
    using SourceComponent::SourceComponent;
    void execute() override { output_ = 7.0; }
};
class SinkDouble : public SinkComponent<double, EmptyConfig> {
public:
    using SinkComponent::SinkComponent;
    void execute() override {}
};
RT_MID_REGISTER_COMPONENT("adapter_test_src_double",  SrcDouble,  EmptyConfig)
RT_MID_REGISTER_COMPONENT("adapter_test_sink_double", SinkDouble, EmptyConfig)

// --- (b) Multi-supplier identity path ---
class SrcA : public SourceComponent<double, EmptyConfig> {
public:
    using SourceComponent::SourceComponent;
    void execute() override { output_ = 3.0; }
};
class SrcB : public SourceComponent<double, EmptyConfig> {
public:
    using SourceComponent::SourceComponent;
    void execute() override { output_ = 4.0; }
};
class SinkMulti2 : public SinkComponent<std::array<double, 2>, EmptyConfig> {
public:
    using SinkComponent::SinkComponent;
    void execute() override {}
};
RT_MID_REGISTER_COMPONENT("adapter_test_src_a",       SrcA,       EmptyConfig)
RT_MID_REGISTER_COMPONENT("adapter_test_src_b",       SrcB,       EmptyConfig)
RT_MID_REGISTER_COMPONENT("adapter_test_sink_multi2", SinkMulti2, EmptyConfig)

// --- (c) Adapted path: int -> double ---
class SrcInt : public SourceComponent<int, EmptyConfig> {
public:
    using SourceComponent::SourceComponent;
    void execute() override { output_ = 5; }
};
RT_MID_REGISTER_COMPONENT("adapter_test_src_int", SrcInt, EmptyConfig)

double int_to_double(const int& v) { return static_cast<double>(v) * 2.0; }

// --- (d) Multiple rounds: each reader/writer's own local counter must
// actually advance. This is exactly what was silently broken before: a
// single round_seq_ shared across the whole pipeline never advanced, so
// every round reused ring buffer slot 0 — harmless only by accident for a
// demo where no two jobs ever overlapped.
class CountingSrc : public SourceComponent<int, EmptyConfig> {
public:
    using SourceComponent::SourceComponent;
    int calls = 0;
    void execute() override { output_ = calls++; }
};
class LastValueSink : public SinkComponent<int, EmptyConfig> {
public:
    using SinkComponent::SinkComponent;
    void execute() override {}
};
RT_MID_REGISTER_COMPONENT("adapter_test_counting_src",    CountingSrc,   EmptyConfig)
RT_MID_REGISTER_COMPONENT("adapter_test_last_value_sink", LastValueSink, EmptyConfig)

int main() {
    // (a) SPSC identity: one producer, one consumer, same type throughout.
    {
        RingBuffer<double, 4> buf;
        auto src  = rtmid::wire_component<SrcDouble>(1, "adapter_test_src_double", {},
                        rtmid::no_upstream{}, rtmid::spsc_writer<double, 4>(buf));
        auto sink = rtmid::wire_component<SinkDouble>(2, "adapter_test_sink_double", {},
                        rtmid::spsc_reader<double, 4>(buf));

        src.subtask->execute();
        sink.subtask->execute();

        auto* sink_c = static_cast<SinkDouble*>(sink.instance.component.get());
        expect(sink_c->input_ == 7.0, "(a) SPSC identity: sink received the exact value the source produced");
    }

    // (b) Multi-supplier fan-in identity: two producers, one consumer,
    // consumer's input_type is std::array<double,2> (one slot per supplier).
    {
        MultiSupplierRingBuffer<double, 4, 2> buf;
        auto a = rtmid::wire_component<SrcA>(1, "adapter_test_src_a", {},
                     rtmid::no_upstream{}, rtmid::multi_writer<double, 4, 2>(buf, /*supplier_id=*/0));
        auto b = rtmid::wire_component<SrcB>(2, "adapter_test_src_b", {},
                     rtmid::no_upstream{}, rtmid::multi_writer<double, 4, 2>(buf, /*supplier_id=*/1));

        expect(!buf.ready(0), "(b) buffer not ready before either supplier has written");
        a.subtask->execute();
        expect(!buf.ready(0), "(b) buffer still not ready after only one of two suppliers has written");
        b.subtask->execute();
        expect(buf.ready(0), "(b) buffer ready once both suppliers have written — matches what a real "
                              "Dispatcher fan_in_mask join would gate on before running the consumer");

        auto sink = rtmid::wire_component<SinkMulti2>(3, "adapter_test_sink_multi2", {},
                        rtmid::multi_reader<double, 4, 2>(buf));
        sink.subtask->execute();

        auto* sink_c = static_cast<SinkMulti2*>(sink.instance.component.get());
        expect(sink_c->input_[0] == 3.0 && sink_c->input_[1] == 4.0,
               "(b) sink received both suppliers' exact values, in supplier-index order");
    }

    // (c) Adapted path: SrcInt (output_type=int) -> SinkDouble (input_type=double)
    // via int_to_double — proves the adapter actually converts, not just
    // plumbs a value through unchanged.
    {
        RingBuffer<double, 4> buf;
        auto src  = rtmid::wire_component<SrcInt>(1, "adapter_test_src_int", {},
                        rtmid::no_upstream{},
                        rtmid::spsc_adapted_writer<int, double, 4>(buf, &int_to_double));
        auto sink = rtmid::wire_component<SinkDouble>(2, "adapter_test_sink_double", {},
                        rtmid::spsc_reader<double, 4>(buf));

        src.subtask->execute();
        sink.subtask->execute();

        auto* sink_c = static_cast<SinkDouble*>(sink.instance.component.get());
        expect(sink_c->input_ == 10.0,
               "(c) adapted path: int_to_double(5) == 10.0 arrived at the sink — the adapter converted the value");
    }

    // (d) Six rounds in a row must each carry their own distinct value.
    {
        RingBuffer<int, 4> buf;
        auto src  = rtmid::wire_component<CountingSrc>(1, "adapter_test_counting_src", {},
                        rtmid::no_upstream{}, rtmid::spsc_writer<int, 4>(buf));
        auto sink = rtmid::wire_component<LastValueSink>(2, "adapter_test_last_value_sink", {},
                        rtmid::spsc_reader<int, 4>(buf));
        auto* sink_c = static_cast<LastValueSink*>(sink.instance.component.get());

        bool all_correct = true;
        for (int round = 0; round < 6; ++round) {
            src.subtask->execute();
            sink.subtask->execute();
            all_correct = all_correct && (sink_c->input_ == round);
        }
        expect(all_correct, "(d) six rounds in a row each carried their own distinct value (0..5), "
                             "not a single reused slot");
    }

    std::cerr << "\nAll checks passed.\n";
    return 0;
}
