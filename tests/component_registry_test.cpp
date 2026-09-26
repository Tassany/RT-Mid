// Standalone test: registers fake source/sink/intermediate components and
// verifies ComponentRegistry actually builds real ComponentBase instances
// whose kind() reflects what was registered — the piece parser_json.cpp's
// validate_dag() now cross-checks against DAG topology.
// Build: g++ -std=c++17 -Isrc -Iinclude tests/component_registry_test.cpp -o /tmp/registry_test

#include <cassert>
#include <iostream>
#include "component_registry.hpp"

struct SensorConfig { double rate_hz = 0.0; };
void from_json(const nlohmann::json& j, SensorConfig& c) {
    c.rate_hz = j.value("rate_hz", 0.0);
}

struct FilterConfig { int window = 0; };
void from_json(const nlohmann::json& j, FilterConfig& c) {
    c.window = j.value("window", 0);
}

struct ActuatorConfig { std::string channel; };
void from_json(const nlohmann::json& j, ActuatorConfig& c) {
    c.channel = j.value("channel", std::string{});
}

class FakeSensor : public SourceComponent<double, SensorConfig> {
public:
    using SourceComponent::SourceComponent;
    void execute() override { output_ = config_->rate_hz; }
};

class FakeFilter : public Component<double, double, FilterConfig> {
public:
    using Component::Component;
    void execute() override { output_ = input_ * config_->window; }
};

class FakeActuator : public SinkComponent<double, ActuatorConfig> {
public:
    using SinkComponent::SinkComponent;
    void execute() override { /* would write to config_->channel */ }
};

RT_MID_REGISTER_COMPONENT("fake_sensor",   FakeSensor,   SensorConfig)
RT_MID_REGISTER_COMPONENT("fake_filter",   FakeFilter,   FilterConfig)
RT_MID_REGISTER_COMPONENT("fake_actuator", FakeActuator, ActuatorConfig)

static void expect(bool cond, const char* what) {
    if (!cond) { std::cerr << "FAIL: " << what << "\n"; std::exit(1); }
    std::cerr << "ok:   " << what << "\n";
}

int main() {
    auto& reg = ComponentRegistry::instance();

    expect(reg.has("fake_sensor") && reg.has("fake_filter") && reg.has("fake_actuator"),
           "all three registered types are visible");
    expect(!reg.has("does_not_exist"), "unregistered type is not visible");

    {
        ComponentInstance inst = reg.create("fake_sensor", nlohmann::json{{"rate_hz", 100.0}});
        expect(inst.component->kind() == ComponentKind::SOURCE,
               "fake_sensor builds as SOURCE via kind()");
        inst.component->execute(); // exercises the config pointer post-construction
    }
    {
        ComponentInstance inst = reg.create("fake_filter", nlohmann::json{{"window", 4}});
        expect(inst.component->kind() == ComponentKind::INTERMEDIATE,
               "fake_filter builds as INTERMEDIATE via kind()");
    }
    {
        ComponentInstance inst = reg.create("fake_actuator", nlohmann::json{{"channel", "gpio0"}});
        expect(inst.component->kind() == ComponentKind::SINK,
               "fake_actuator builds as SINK via kind()");
    }

    bool threw = false;
    try { reg.create("does_not_exist", nlohmann::json::object()); }
    catch (const std::runtime_error&) { threw = true; }
    expect(threw, "create() throws on an unregistered component_type");

    std::cerr << "\nAll checks passed.\n";
    return 0;
}
