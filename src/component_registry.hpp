#pragma once

/**
 * @file component_registry.hpp
 *
 * Turns a deployment plan's component_type string + config JSON blob into a
 * real ComponentBase* — the piece that was missing between component.hpp
 * (paper Section IV: components are C++ classes with a kind()) and the
 * deployment plan (paper Section IV-C: "for each subtask the type of
 * component used, the values for each field in the config_type").
 *
 * Application code registers its own concrete component types (they're
 * application-specific — RT-Mid itself can't know them in advance) with:
 *
 *   struct SensorConfig { double sample_rate_hz; };
 *   void from_json(const nlohmann::json& j, SensorConfig& c) {
 *       c.sample_rate_hz = j.at("sample_rate_hz");
 *   }
 *   class TemperatureSensor : public SourceComponent<Reading, SensorConfig> {
 *       ...
 *   };
 *   RT_MID_REGISTER_COMPONENT("temperature_sensor", TemperatureSensor, SensorConfig)
 *
 * from_json is nlohmann::json's standard extension point — required so the
 * registry can parse each component's own config type generically.
 */

#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <nlohmann/json.hpp>
#include "component.hpp"

// Owns both the constructed component and the config object it points to
// (Component<I,O,C>::config_ is a non-owning const ConfigType*, so something
// has to keep the ConfigType alive for at least as long as the component).
// Declaration order matters: C++ destroys members in reverse declaration
// order, so `component` (declared second) is destroyed before `config`
// (declared first) — config_ stays valid for the component's whole lifetime,
// including during its own destructor.
struct ComponentInstance {
    std::shared_ptr<void>          config;
    std::unique_ptr<ComponentBase> component;
};

class ComponentRegistry {
public:
    using Factory = std::function<ComponentInstance(const nlohmann::json&)>;

    static ComponentRegistry& instance() {
        static ComponentRegistry reg;
        return reg;
    }

    // Idempotent by design, not just tolerant of it: RT_MID_REGISTER_COMPONENT
    // runs its registration from a static initializer inside an anonymous
    // namespace, so including the same component header from more than one
    // translation unit of the same binary — which happens routinely once
    // codegen-generated code and a test/application both need the same
    // concrete types — runs this constructor once per TU, all registering
    // the same name. That's expected, not a collision: only re-registering a
    // *different* name than what's already recorded would indicate a real
    // clash (two unrelated components picking the same component_type
    // string), which nothing here currently distinguishes from the benign
    // case — acceptable for now since RT_MID_REGISTER_COMPONENT always
    // derives `name` and the factory together from the same macro
    // invocation, so a rebound name and a rebound factory happen together.
    void register_type(const std::string& name, Factory factory) {
        if (factories_.count(name)) return;
        factories_[name] = std::move(factory);
    }

    bool has(const std::string& name) const {
        return factories_.count(name) != 0;
    }

    ComponentInstance create(const std::string& name, const nlohmann::json& config) const {
        auto it = factories_.find(name);
        if (it == factories_.end())
            throw std::runtime_error(
                "ComponentRegistry: unknown component_type \"" + name + "\"");
        return it->second(config);
    }

private:
    std::map<std::string, Factory> factories_;
};

#define RT_MID_REGISTER_COMPONENT(name, ConcreteType, ConfigType)                  \
    namespace {                                                                    \
        struct ConcreteType##_Registrar {                                          \
            ConcreteType##_Registrar() {                                           \
                ComponentRegistry::instance().register_type(name,                  \
                    [](const nlohmann::json& j) -> ComponentInstance {             \
                        auto cfg = std::make_shared<ConfigType>(j.get<ConfigType>()); \
                        ComponentInstance inst;                                    \
                        inst.config    = cfg;                                      \
                        inst.component = std::make_unique<ConcreteType>(cfg.get());\
                        return inst;                                               \
                    });                                                            \
            }                                                                      \
        } ConcreteType##_registrar_;                                               \
    }
