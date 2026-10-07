CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -Isrc -Iinclude
BUILD    := build
GENDIR      := generated
CODEGEN_BIN := $(BUILD)/codegen_main
PLAN        := plans/deployment_plan.json

# allocator.cpp's dispatcher references every strategy unconditionally
# (dru::apply_wf_dru_allocation, eru::apply_eru_allocation,
# tdta::apply_tdta_allocation, ...), so any binary linking allocator.cpp
# needs every strategy's own .cpp linked too, regardless of which strategy
# that binary actually uses at runtime. Add a new strategy's .cpp here
# (and nowhere else) when one is added. tdta.cpp itself needs ied.cpp
# (Algorithm 1), so that's included here too.
ALLOCATOR_SRCS := src/allocator.cpp src/dru.cpp src/eru.cpp src/ied.cpp src/tdta.cpp

# Each test only links the src/*.cpp files it actually needs — linking every
# src/*.cpp into every test indiscriminately would make each test's build
# depend on every other part of the codebase compiling. Add a line here when
# a new test needs extra .cpp files to link against.
EXTRA_SRCS.dag_arbitrary_shapes_test := src/dag.cpp
EXTRA_SRCS.component_registry_test   :=
EXTRA_SRCS.integration_pipeline_test := src/dag.cpp src/team_manager.cpp
EXTRA_SRCS.parser_json_test          := src/dag.cpp src/parser_json.cpp
EXTRA_SRCS.deployment_plan_fields_test := src/dag.cpp src/parser_json.cpp
# dru_test's end-to-end case parses plans/deployment_plan.json via
# JsonParser::parse_for_codegen, which needs the real DAG/parser code.
EXTRA_SRCS.dru_test                    := src/dag.cpp src/parser_json.cpp $(ALLOCATOR_SRCS)
EXTRA_SRCS.eru_test                    := src/dag.cpp $(ALLOCATOR_SRCS)
EXTRA_SRCS.tdta_test                   := src/dag.cpp $(ALLOCATOR_SRCS)
EXTRA_SRCS.rrc_test                    := src/dag.cpp src/rrc.cpp $(ALLOCATOR_SRCS)
EXTRA_SRCS.rta_fonseca2016_test        := src/dag.cpp src/rrc.cpp src/rta_fonseca2016.cpp $(ALLOCATOR_SRCS)
EXTRA_SRCS.test_flux                  := src/dag.cpp src/parser_json.cpp src/team_manager.cpp $(ALLOCATOR_SRCS)
EXTRA_SRCS.performance_test           := src/dag.cpp src/parser_json.cpp src/team_manager.cpp $(ALLOCATOR_SRCS)
EXTRA_SRCS.adapter_test               :=
EXTRA_SRCS.codegen_pipeline_test      := src/dag.cpp src/team_manager.cpp src/parser_json.cpp $(GENDIR)/pipeline_generated.cpp

TEST_SRCS  := $(wildcard tests/*.cpp)
TEST_NAMES := $(basename $(notdir $(TEST_SRCS)))
TEST_BINS  := $(addprefix $(BUILD)/,$(TEST_NAMES))

APP_BIN  := $(BUILD)/rt_mid
EVAL_BIN := $(BUILD)/latency_eval
INTERFERENCE_EVAL_BIN := $(BUILD)/interference_eval
JOBS     := 100

.PHONY: all test clean codegen run perf interference_eval

all: test

$(BUILD)/%: tests/%.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(GENDIR) $< $(EXTRA_SRCS.$*) -o $@

$(BUILD):
	mkdir -p $(BUILD)

$(CODEGEN_BIN): tools/codegen/codegen_main.cpp src/dag.cpp src/parser_json.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) tools/codegen/codegen_main.cpp src/dag.cpp src/parser_json.cpp -o $@

# Runs the codegen tool against $(PLAN), emitting $(GENDIR)/pipeline_generated.{hpp,cpp}.
codegen: $(CODEGEN_BIN)
	mkdir -p $(GENDIR)
	$(CODEGEN_BIN) $(PLAN) $(GENDIR)

# codegen_pipeline_test needs the generated files before it can even be
# compiled — this adds that as an order-only prerequisite to the generic
# pattern rule above (make merges prerequisite-only rules onto the same
# target) without needing a bespoke recipe.
$(BUILD)/codegen_pipeline_test: | codegen

# The actual program: parses $(PLAN), builds and runs the real pipeline,
# stays up until Ctrl+C. Also needs the generated files first.
$(APP_BIN): src/main.cpp src/dag.cpp src/team_manager.cpp src/parser_json.cpp $(ALLOCATOR_SRCS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(GENDIR) src/main.cpp src/dag.cpp src/team_manager.cpp \
		src/parser_json.cpp $(ALLOCATOR_SRCS) $(GENDIR)/pipeline_generated.cpp -o $@
$(APP_BIN): | codegen

run: $(APP_BIN)
	./$(APP_BIN) $(PLAN)

# Measures end-to-end response time and deadline miss ratio (paper Section
# 5's own metrics) against $(PLAN) for $(JOBS) jobs per task. CSV to stdout,
# summary to stderr — see tools/eval/latency_eval_main.cpp's header for the
# exact definitions and a known correlation limitation to read before
# trusting numbers under real load.
$(EVAL_BIN): tools/eval/latency_eval_main.cpp src/dag.cpp src/team_manager.cpp src/parser_json.cpp $(ALLOCATOR_SRCS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(GENDIR) tools/eval/latency_eval_main.cpp src/dag.cpp src/team_manager.cpp \
		src/parser_json.cpp $(ALLOCATOR_SRCS) $(GENDIR)/pipeline_generated.cpp -o $@
$(EVAL_BIN): | codegen

perf: $(EVAL_BIN)
	./$(EVAL_BIN) $(PLAN) $(JOBS)

# Measures LOW's response time under HIGH/MID interference on the HIGH/MID/LOW
# plan tools/eval/interference_topology.{hpp,cpp} builds, under each core
# placement (single_core/wf_dru/eru/tdta) — see
# tools/eval/interference_eval_main.cpp's header for the full rationale.
# Doesn't need $(GENDIR)/codegen: this plan is built in C++, not parsed from
# $(PLAN). Usage: ./$(INTERFERENCE_EVAL_BIN) <single_core|wf_dru|eru|tdta>
# <low_freq_hz> [jobs] [--trace]
$(INTERFERENCE_EVAL_BIN): tools/eval/interference_eval_main.cpp tools/eval/interference_topology.cpp \
		src/dag.cpp src/team_manager.cpp $(ALLOCATOR_SRCS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -Itools/eval tools/eval/interference_eval_main.cpp tools/eval/interference_topology.cpp \
		src/dag.cpp src/team_manager.cpp $(ALLOCATOR_SRCS) -o $@ -lpthread

interference_eval: $(INTERFERENCE_EVAL_BIN)

# Builds every tests/*.cpp and runs each binary in turn. Stops at the first
# failing test (non-zero exit) but still reports which one failed.
test: $(TEST_BINS)
	@status=0; \
	for t in $(TEST_BINS); do \
		echo "== $$t =="; \
		./$$t || status=1; \
		echo; \
	done; \
	exit $$status

clean:
	rm -rf $(BUILD)
