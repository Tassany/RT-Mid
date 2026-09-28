CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -Isrc -Iinclude
BUILD    := build
GENDIR      := generated
CODEGEN_BIN := $(BUILD)/codegen_main
PLAN        := plans/deployment_plan.json

# Each test only links the src/*.cpp files it actually needs — linking every
# src/*.cpp into every test indiscriminately would make each test's build
# depend on every other part of the codebase compiling. Add a line here when
# a new test needs extra .cpp files to link against.
EXTRA_SRCS.dag_arbitrary_shapes_test := src/dag.cpp
EXTRA_SRCS.component_registry_test   :=
EXTRA_SRCS.integration_pipeline_test := src/dag.cpp src/team_manager.cpp
# src/allocator.hpp is a stub (see that file's header) standing in for the
# real allocation heuristic, which is still being rewritten — this test only
# exercises JsonParser's parse/validate/auto-allocate pipeline, not a real
# allocation decision.
EXTRA_SRCS.parser_json_test          := src/dag.cpp src/parser_json.cpp
EXTRA_SRCS.deployment_plan_fields_test := src/dag.cpp src/parser_json.cpp
EXTRA_SRCS.test_flux                  := src/dag.cpp src/parser_json.cpp src/team_manager.cpp
EXTRA_SRCS.performance_test           := src/dag.cpp src/parser_json.cpp src/team_manager.cpp
EXTRA_SRCS.adapter_test               :=
EXTRA_SRCS.codegen_pipeline_test      := src/dag.cpp src/team_manager.cpp src/parser_json.cpp $(GENDIR)/pipeline_generated.cpp

TEST_SRCS  := $(wildcard tests/*.cpp)
TEST_NAMES := $(basename $(notdir $(TEST_SRCS)))
TEST_BINS  := $(addprefix $(BUILD)/,$(TEST_NAMES))

APP_BIN  := $(BUILD)/rt_mid
EVAL_BIN := $(BUILD)/latency_eval
JOBS     := 100

.PHONY: all test clean codegen run perf

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
$(APP_BIN): src/main.cpp src/dag.cpp src/team_manager.cpp src/parser_json.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(GENDIR) src/main.cpp src/dag.cpp src/team_manager.cpp \
		src/parser_json.cpp $(GENDIR)/pipeline_generated.cpp -o $@
$(APP_BIN): | codegen

run: $(APP_BIN)
	./$(APP_BIN) $(PLAN)

# Measures end-to-end response time and deadline miss ratio (paper Section
# 5's own metrics) against $(PLAN) for $(JOBS) jobs per task. CSV to stdout,
# summary to stderr — see tools/eval/latency_eval_main.cpp's header for the
# exact definitions and a known correlation limitation to read before
# trusting numbers under real load.
$(EVAL_BIN): tools/eval/latency_eval_main.cpp src/dag.cpp src/team_manager.cpp src/parser_json.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -I$(GENDIR) tools/eval/latency_eval_main.cpp src/dag.cpp src/team_manager.cpp \
		src/parser_json.cpp $(GENDIR)/pipeline_generated.cpp -o $@
$(EVAL_BIN): | codegen

perf: $(EVAL_BIN)
	./$(EVAL_BIN) $(PLAN) $(JOBS)

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
