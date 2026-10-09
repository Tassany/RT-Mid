# Development Rules

This file exists because most of RT-Mid's first implementation was produced by
unsupervised AI code generation ("vibecoding"). The result ran, but large parts
of it were not understood by the author well enough to be defended in the
paper it supports. These rules govern how this codebase is rebuilt and
extended from here on, with any AI assistant (Claude or otherwise) included.

## 1. Git and merge protocol

- An assistant may **propose** a branch (including its name), the exact set
  of files/items for each commit, and a draft commit message — but may never
  **execute** `git add`, `git commit`, `git branch`/`checkout -b`,
  `git merge`, `git push`, or any other git write operation without the
  human author's explicit approval for that specific action. Proposing is
  not executing: nothing is staged, committed, branched, or pushed until the
  author approves that action by name, in that session. No blanket
  approval carries over to a different action or a later session.
- An assistant may edit files directly, but every edit must come with an
  explanation of what changed and why, given before or alongside the edit —
  never as a large silent diff the author is expected to review after the
  fact. Nothing is staged until the author can restate that explanation in
  their own words.
- Keep individual AI-assisted diffs small (target: under ~150 lines per
  accepted change). A diff too large to explain in one sitting is a diff too
  large to accept in one sitting — split it.
- A drafted commit message is a proposal, not a final: the author reviews
  and can rewrite it freely before approving. An assistant never executes a
  commit whose message the author hasn't seen and approved for that
  specific commit.

## 2. Rewrite vs. document

Not every unclear function needs a rewrite. The test is: **can the difficulty
be named?**

- Legitimately complex code has a name for its difficulty — "SPSC ring buffer
  with cache-line alignment to avoid false sharing," "Worst-Fit bin packing
  with decreasing remaining utilisation." If you can say it in one sentence,
  document it (a short comment stating the invariant/contract) and move on.
- Obscure code has no nameable invariant — it only makes sense by tracing
  execution, has magic numbers, or needs re-reading every time to re-explain.
  This gets rewritten, not documented around.

This bar is not uniform across the codebase — raise it where the code is
closer to the paper's scientific claim:

- **Core contribution** (subtask-to-core allocation, response-time-relevant
  scheduling logic in the allocator and `Dispatcher`): must be *provably*
  faithful to the model described in the paper, not just individually
  understandable. See §4.
- **Supporting mechanism** (JSON parsing, ring buffer plumbing, the
  `TeamManager` state machine): documenting-if-nameable is enough, unless it
  turns out to affect the numbers in the evaluation (e.g. ring buffer
  overhead skewing measured response time), in which case it's promoted to
  the stricter bar.

## 3. Tests

As of 2026-09, the only automated validation in this project is the Python
evaluation scripts (deadline miss ratio, end-to-end response time). Those are
a system-level empirical oracle — useful, but they don't tell you that any
individual function does what you think it does; a bug in the allocator and a
"legitimately hard scheduling problem" can produce the same table.

Going forward: every non-trivial function touched during the rewrite gets at
least one automated test — unit-level where possible, a targeted timing
harness where the behavior is inherently about timing (e.g. release-time
enforcement in `Dispatcher`). A function without a test that would fail if its
behavior changed is not considered understood, only observed.

## 4. Spec-first for the core scientific contribution

For the subtask-to-core allocation logic (the paper's main claim): write the
algorithm in pseudocode or as a precise restatement of the paper's model
*before* writing or reading the corresponding C++. Then check the
implementation against that spec explicitly, line by line.

Do not infer the specification by reading generated code and guessing what it
was probably supposed to do — that reconstructs whatever assumptions the
original generation made, not the author's own model. If code and spec
disagree, that's a finding: figure out whether the code is wrong or the paper
claim is wrong before writing either one off.

## 5. Concurrency gets elevated scrutiny

Anything touching shared/atomic state across threads (ring buffers, the
`Dispatcher` queue and timer handoff between its two threads) requires a
written ordering/happens-before argument before it's accepted — not just "it
passed a test run." Concurrency bugs are the hardest to catch by running the
system and the most damaging to the paper's credibility if they surface after
submission.

## 6. Citation hygiene

Any comment that cites "paper Section X" must be checked against the current
PDF before being trusted or kept during the rewrite. The previous codebase had
comments citing section numbers (e.g. "Section V-B/V-C") that don't exist in
the current paper — most likely stale references to the MCFlow paper (Huang
et al. 2012) used as design inspiration, not to this work. A citation that
turns out to be wrong is worse than no citation; fix or remove it, don't carry
it forward uninspected.

## 7. Paper/code sync

Architectural decisions made in code (e.g. dropping a layer, changing the
allocation heuristic) are tracked against the paper text as they happen, not
reconciled at the end. If the code no longer has four layers, Section 4 of
the paper is a known pending edit from the moment that decision is made, not
a surprise discovered during a later read-through.

## 8. Mapping process (for onboarding back into a module, or after a gap)

1. **Structural pass** (hours): what files exist, what includes what, what
   the main classes/structs are.
2. **One end-to-end trace** (a day or two): follow a single task from
   deployment-plan parsing through DAG construction, core allocation,
   dispatch, and ring-buffer handoff to the next subtask. This trace is the
   spine; everything else is detail filled in as it's encountered on this
   path.
3. **Stopping criterion**: the pass is done when the trace can be drawn and
   explained from memory — not when every line of every file has been read.
   Don't let this step expand past its criterion.

## 9. Elaborate work: grill before you spec

Trigger, kept concrete on purpose (see §2's "can the difficulty be named"
test for why): a request counts as **elaborate** when it will plainly take
more than one commit-sized change to implement (§1's ~150-line guideline) —
a new architectural piece, a rewrite spanning a whole layer, anything
touching more than one of the layers mapped in §8/the architecture
artifact. A single bugfix, a single new test, or a one-file addition does
not trigger this — this is a gate on scope, not a feeling that the task
matters.

When it triggers:

1. Pressure-test the idea before it becomes a spec — `grill-with-docs`
   (`mattpocock-skills`) is the named tool for this, but its skill is
   marked `disable-model-invocation`, so an assistant cannot call it
   directly; only the human author can, by typing `/grill-with-docs`. If the
   author hasn't done that when this step is due, an assistant reproduces
   the same effect itself by calling `grilling` then `domain-modeling` in
   sequence (that's literally what `grill-with-docs` chains) — or simply
   asks the author to run `/grill-with-docs`.
2. Once the idea survives that, use `spec-driven-development` (`sdd`) to
   turn it into a spec → plan → tasks, and keep using it to track execution
   as the work proceeds — not just at kickoff. `sdd` has no such
   restriction; an assistant can trigger it directly.

This doesn't replace §4 (spec-first for the core scientific contribution) —
core-contribution work still needs the pseudocode/paper-model check §4
requires either way. It adds the grilling step in front of that, and covers
elaborate work outside the core contribution too.

## 10. Doxygen comments: rewritten alongside the code, capped at 120 words

Whenever code carrying a Doxygen comment (`/** ... */` with `@brief`,
`@param`, `@return`, `@var`, etc.) is edited, that comment is rewritten in
the same change to match the new behavior — never left describing what the
code used to do. Each comment block is capped at 120 words.

If describing the new behavior honestly needs more than 120 words, that's a
signal per §2's own test ("can the difficulty be named?") that the code
needs rewriting or splitting, not a longer comment.

This applies to whatever a change actually touches, going forward — it does
not require rewriting every existing Doxygen comment in the codebase up
front.
