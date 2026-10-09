# RT-Mid

DAG scheduling middleware for real-time subtask pipelines, with automatic
subtask-to-core allocation as the main contribution of the paper this
repository supports. A deployment plan (JSON) describes a pipeline as a DAG
of subtasks; RT-Mid parses it, allocates subtasks to cores, and dispatches
them at runtime while enforcing release times and deadlines.

Several subtask-to-core allocation strategies are implemented and compared,
including Worst-Fit DRU/ERU and TDTA (a topology-based allocator), together
with response-time analysis (RRC/Fonseca 2016 RTA) and latency/deadline-miss
evaluation tooling.

## Project links

- Source code and issue tracker: <https://github.com/Tassany/RT-Mid>
- Development process and rules this codebase follows: [RULES.md](RULES.md)
- Architecture notes and terminology: [CONTEXT.md](CONTEXT.md), [docs/](docs/)

## Requirements

- A C++17 compiler (`g++`)
- GNU Make
- Linux with a `PREEMPT_RT` kernel is required to reproduce the paper's
  timing-sensitive evaluations (`performance_test`, `interference_eval`);
  it is not required to build or run the unit tests.
- [nlohmann/json](https://github.com/nlohmann/json) is vendored under
  `include/nlohmann/json.hpp` (MIT licensed, see file header) — no
  separate install step needed.

## Build and test

```sh
make test      # builds and runs every tests/*.cpp binary
make codegen   # regenerates generated/pipeline_generated.{hpp,cpp} from
               # plans/deployment_plan.json (make test does this automatically)
make clean     # removes build/
```

Each `tests/*.cpp` file also documents its own standalone `g++` command in a
header comment, for building that one binary in isolation.

Other binaries built from the `Makefile`:

```sh
make run              # builds and runs the real pipeline (src/main.cpp) against plans/deployment_plan.json
make perf             # end-to-end response time / deadline miss ratio evaluation
make interference_eval  # interference evaluation across core-placement strategies
```

## License

RT-Mid is released under the [MIT License](LICENSE). See [AUTHORS](AUTHORS)
for the list of contributors.

## Citing this software

If you use RT-Mid in your research, please cite it using the metadata in
[CITATION.cff](CITATION.cff).

## Contact

Tassany Onofre de Oliveira — <tassany.onofre-de-oliveira@enac.fr>
