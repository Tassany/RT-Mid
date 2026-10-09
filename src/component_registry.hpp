#pragma once

/**
 * @file component_registry.hpp
 *
 * Resolves a deployment plan's component_type string — always one of
 * "source", "intermediate", or "sink" (see CONTEXT.md) — plus its config
 * JSON blob into a real ComponentBase*. Used only by
 * JsonParser::validate_components, the one place that receives a
 * component_type from JSON without knowing the concrete C++ type at
 * compile time; runtime wiring (adapter.hpp's wire_component) already
 * knows the concrete type and constructs it directly, bypassing this
 * registry.
 *
 * Application code registers its concrete class per role with:
 *
 *   struct SensorConfig { double sample_rate_hz; };
 *   void from_json(const nlohmann::json& j, SensorConfig& c) {
 *       c.sample_rate_hz = j.at("sample_rate_hz");
 *   }
 *   class MySource : public SourceComponent<Reading, SensorConfig> {
 *       ...
 *   };
 *   RT_MID_REGISTER_COMPONENT("source", MySource, SensorConfig)
 *
 * from_json is nlohmann::json's standard extension point — required so
 * the registry can parse each component's own config type generically.
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
/**
 * @brief Owns a constructed component together with its backing config.
 *
 * Component<I,O,C>::config_ is a non-owning const ConfigType*, so
 * something must keep the ConfigType alive for at least as long as the
 * component. Declaration order matters here: C++ destroys members in
 * reverse declaration order, so `component` (declared second) is
 * destroyed before `config` (declared first), keeping config_ valid for
 * the component's whole lifetime, including its own destructor.
 *
 * @var ComponentInstance::config Type-erased owner of the component's
 *      config object.
 * @var ComponentInstance::component Owning pointer to the constructed
 *      component.
 */
struct ComponentInstance {
    std::shared_ptr<void>          config;
    std::unique_ptr<ComponentBase> component;
};

/**
 * @brief Global registry mapping component_type strings to factories.
 *
 * Maps the component_type strings used in deployment plans to factory
 * functions that construct the corresponding concrete ComponentBase and
 * its config from a JSON blob. Populated at static-init time by
 * RT_MID_REGISTER_COMPONENT.
 */
class ComponentRegistry {
public:
    /** @brief Function that builds a ComponentInstance from a config JSON blob. */
    using Factory = std::function<ComponentInstance(const nlohmann::json&)>;

    /**
     * @brief Accesses the process-wide registry singleton.
     * @return Reference to the single ComponentRegistry instance.
     */
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
    /**
     * @brief Registers a factory under a component_type name, idempotently.
     *
     * A no-op if @p name is already registered (see comment below on why
     * this is safe under repeated static-initializer registration across
     * translation units).
     *
     * @param name component_type string this factory is registered under.
     * @param factory Function constructing a ComponentInstance from config
     *        JSON.
     * @return void
     */
    void register_type(const std::string& name, Factory factory) {
        if (factories_.count(name)) return;
        factories_[name] = std::move(factory);
    }

    /**
     * @brief Checks whether a component_type is registered.
     * @param name component_type string to look up.
     * @return true if a factory is registered under @p name.
     */
    bool has(const std::string& name) const {
        return factories_.count(name) != 0;
    }

    /**
     * @brief Builds a component instance for a registered component_type.
     * @param name component_type string identifying which factory to use.
     * @param config JSON blob passed to the factory to build the
     *        component's config object.
     * @return Newly constructed ComponentInstance.
     * @throws std::runtime_error if @p name is not registered.
     */
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

/**
 * @def RT_MID_REGISTER_COMPONENT(name, ConcreteType, ConfigType)
 * @brief Registers a concrete component type under a component_type name.
 *
 * Expands to a file-local struct whose constructor runs at static-init
 * time and registers, under the registry singleton, a factory that parses
 * a ConfigType from JSON (via that type's from_json) and constructs a
 * ConcreteType with it. Declared inside an anonymous namespace so
 * including the same component header from multiple translation units is
 * safe (see ComponentRegistry::register_type).
 *
 * @param name String literal component_type key, e.g. "temperature_sensor".
 * @param ConcreteType Concrete ComponentBase subclass to construct.
 * @param ConfigType Config type passed to ConcreteType's constructor.
 */
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
