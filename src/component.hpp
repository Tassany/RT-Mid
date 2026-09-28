#ifndef COMPONENT_HPP
#define COMPONENT_HPP

#include <vector>
#include <string>

// ----------------------------------------------------------------
//  Component classification (paper Section IV)
// ----------------------------------------------------------------
/**
 * @brief Classifies a component's role in a pipeline (paper Section IV).
 *
 * SOURCE components produce output with no input, INTERMEDIATE components
 * consume input and produce output, and SINK components consume input with
 * no output.
 */
enum class ComponentKind { SOURCE, INTERMEDIATE, SINK };

// ----------------------------------------------------------------
//  ComponentBase — abstract interface for all components
// ----------------------------------------------------------------
/**
 * @brief Abstract, type-erased interface implemented by every component.
 *
 * DAG and the dispatcher operate on this base so heterogeneous
 * Component/SourceComponent/SinkComponent instantiations can be stored and
 * scheduled uniformly.
 */
class ComponentBase {
public:
    /**
     * @brief Optional pre-run setup hook.
     *
     * Called once before the real-time loop starts. Subclasses may
     * override to pre-allocate memory up front so execute() performs no
     * dynamic allocation. Default implementation does nothing.
     * @return void
     */
    virtual void preallocate() {}

    /**
     * @brief Runs one subjob of this component.
     *
     * Invoked by the dispatcher once per subjob; must be implemented by
     * every concrete component.
     * @return void
     */
    virtual void execute() = 0;

    /**
     * @brief Reports this component's role in the pipeline.
     * @return ComponentKind::INTERMEDIATE by default, so existing
     *         Component<I,O,C> subclasses compile without modification;
     *         SourceComponent/SinkComponent override it.
     */
    virtual ComponentKind kind() const { return ComponentKind::INTERMEDIATE; }

    /** @brief Virtual destructor for safe polymorphic destruction. */
    virtual ~ComponentBase() = default;
};

// ----------------------------------------------------------------
//  Component<I,O,C> — intermediate component (has both input and output)
// ----------------------------------------------------------------
/**
 * @brief Intermediate component: consumes one input and produces one output.
 *
 * @tparam InputType Type of the value consumed via init_input().
 * @tparam OutputType Type of the value produced and read back via output_.
 * @tparam ConfigType Type of the read-only configuration passed at
 *         construction.
 */
template<typename InputType, typename OutputType, typename ConfigType>
class Component : public ComponentBase {
public:
    using input_type  = InputType;
    using output_type = OutputType;
    using config_type = ConfigType;

    InputType  input_;
    OutputType output_;

    /**
     * @brief Constructs the component with a shared, read-only config.
     * @param config Non-owning pointer to this component's configuration.
     */
    explicit Component(const ConfigType* config) : config_(config) {}
    /** @brief Default destructor. */
    ~Component() override = default;

    /**
     * @brief Stores the next input value to be consumed by execute().
     * @param v Input value to copy into this component.
     * @return void
     */
    void init_input (const InputType&  v) { input_  = v; }
    /**
     * @brief Seeds the output value (e.g. for testing or preallocation).
     * @param v Output value to copy into this component.
     * @return void
     */
    void init_output(const OutputType& v) { output_ = v; }

    /** @brief Reports this component as ComponentKind::INTERMEDIATE. */
    ComponentKind kind() const override { return ComponentKind::INTERMEDIATE; }
    /** @brief Runs this component's logic; implemented by subclasses. */
    void execute() override = 0;

protected:
    const ConfigType* config_;
};

// ----------------------------------------------------------------
//  SourceComponent<O,C> — generates output, consumes no input
// ----------------------------------------------------------------
/**
 * @brief Source component: produces output and consumes no input.
 *
 * @tparam OutputType Type of the value produced and read back via output_.
 * @tparam ConfigType Type of the read-only configuration passed at
 *         construction.
 */
template<typename OutputType, typename ConfigType>
class SourceComponent : public ComponentBase {
public:
    using output_type = OutputType;
    using config_type = ConfigType;

    OutputType output_;

    /**
     * @brief Constructs the component with a shared, read-only config.
     * @param config Non-owning pointer to this component's configuration.
     */
    explicit SourceComponent(const ConfigType* config) : config_(config) {}
    /** @brief Default destructor. */
    ~SourceComponent() override = default;

    /**
     * @brief Seeds the output value (e.g. for testing or preallocation).
     * @param v Output value to copy into this component.
     * @return void
     */
    void init_output(const OutputType& v) { output_ = v; }

    /** @brief Reports this component as ComponentKind::SOURCE. */
    ComponentKind kind() const override { return ComponentKind::SOURCE; }
    /** @brief Runs this component's logic; implemented by subclasses. */
    void execute() override = 0;

protected:
    const ConfigType* config_;
};

// ----------------------------------------------------------------
//  SinkComponent<I,C> — consumes input, produces no output
// ----------------------------------------------------------------
/**
 * @brief Sink component: consumes input and produces no output.
 *
 * @tparam InputType Type of the value consumed via init_input().
 * @tparam ConfigType Type of the read-only configuration passed at
 *         construction.
 */
template<typename InputType, typename ConfigType>
class SinkComponent : public ComponentBase {
public:
    using input_type  = InputType;
    using config_type = ConfigType;

    InputType input_;

    /**
     * @brief Constructs the component with a shared, read-only config.
     * @param config Non-owning pointer to this component's configuration.
     */
    explicit SinkComponent(const ConfigType* config) : config_(config) {}
    /** @brief Default destructor. */
    ~SinkComponent() override = default;

    /**
     * @brief Stores the next input value to be consumed by execute().
     * @param v Input value to copy into this component.
     * @return void
     */
    void init_input(const InputType& v) { input_ = v; }

    /** @brief Reports this component as ComponentKind::SINK. */
    ComponentKind kind() const override { return ComponentKind::SINK; }
    /** @brief Runs this component's logic; implemented by subclasses. */
    void execute() override = 0;

protected:
    const ConfigType* config_;
};

#endif // COMPONENT_HPP
