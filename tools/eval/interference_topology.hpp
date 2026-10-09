#pragma once

/**
 * @file interference_topology.hpp
 *
 * The HIGH/LOW two-task plan tools/eval/interference_eval_main.cpp (real,
 * measured response time under single_core/wf_dru/eru/tdta) and
 * tools/eval/wcrt_eval_main.cpp (rrc::compute_wcrt's theoretical bound for
 * the same plan) both build — factored out so the tools can never silently
 * drift onto different topologies while both claiming to analyze "the
 * interference_eval scenario".
 *
 * Both tasks share the SAME shape: two nested 2-way forks (e.g. HIGH:
 * Hi -> A,B; A -> A1,A2; B -> B1,B2; {A1,A2,B1,B2} -> Hf) -- two Str
 * structures per task (one at level 1, two at level 2). HIGH_WCET_US is
 * deliberately tight: total WCET across HIGH's 8 subtasks is 18500us and
 * its longest root-to-leaf path (Hi+A+A1+Hf = 9000us) leaves only ~10%
 * margin under HIGH_PERIOD_US's 10000us period -- on purpose, since the
 * schedulability "knee" (near-saturated, not deep in the safe zone) is
 * where wf_dru/tdta's placement choices actually matter; see the paper's
 * own Fig. 4, where the gap between strategies is largest at intermediate
 * utilization, not at the extremes. A wider, lower-margin 5-way-fork HIGH
 * (paired with a flat 4-fan LOW) was tried first and discarded: with ~64%
 * margin it never showed a measurable difference, and a three-task
 * HIGH/MID/LOW variant was tried and discarded too: a second,
 * independently-allocated interferer coincidentally equalized the
 * worst-case load LOW saw under both strategies.
 *
 * Giving LOW the SAME forked shape as HIGH (instead of a flat 4-fan) adds
 * a second, HIGH-independent mechanism for TDTA to win on: self-interference
 * (Definition 3 — two subtasks of the SAME task with no precedence
 * relation landing on the same core serialize each other, since equal
 * priority means neither preempts the other). TDTA runs the identical
 * Algorithm 3 (Theorem 3's own-Str-structure guarantee) when it allocates
 * LOW, not just HIGH, so it should keep LOW's own A/B-equivalent branches
 * apart whenever the remaining cores allow it; WF+DRU's single
 * cross-task global remaining-utilization order has no such per-task
 * guarantee for LOW's branches either. LOW_WCET_US is the same shape,
 * scaled to about half of HIGH's so LOW's own isolated critical path
 * (Li+P+P1+Lf = 4500us) stays well inside its swept 50-90Hz period
 * (11111-20000us) -- interference/self-interference should move LOW's
 * response time without LOW missing its own deadline outright.
 *
 * No execution-specific content here (ring buffers, wiring, drivers) --
 * just the DeploymentPlan construction + allocation, which is all
 * rrc::compute_wcrt needs and all build_plan's own callers share.
 */

#include "allocator.hpp"
#include "deployment_plan.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace interference_topology {

constexpr int NUM_CORES = 4;

// HIGH's longest root-to-leaf path (Hi+A+A1+Hf, see HIGH_WCET_US below) is
// 9000us, leaving only ~10% margin under this period -- deliberately
// tight; see this file's header comment for why.
constexpr uint64_t HIGH_PERIOD_US = 10000;

// HIGH: Hi(1) -> A(2),B(3); A -> A1(4),A2(5); B -> B1(6),B2(7);
// {A1,A2,B1,B2} -> Hf(8). Two Str structures: {A,B} at level 1,
// {A1,A2} and {B1,B2} at level 2.
constexpr uint64_t HIGH_WCET_US[8] = {1000, 3000, 2000, 4000, 2500, 3500, 1500, 1000};

// LOW: Li(11) -> P(12),Q(13); P -> P1(14),P2(15); Q -> Q1(16),Q2(17);
// {P1,P2,Q1,Q2} -> Lf(18). Same shape as HIGH, WCETs halved.
constexpr uint64_t LOW_WCET_US[8] = {500, 1500, 1000, 2000, 1250, 1750, 750, 500};

/**
 * @brief Builds the HIGH/LOW plan and, unless @p mode is "single_core",
 *        allocates it via @p mode's strategy.
 *
 * HIGH is task id 1 (TaskInfo::priority 0, SubtaskInfo::priority 30,
 * subtask ids 1-8); LOW is task id 2 (TaskInfo::priority 1,
 * SubtaskInfo::priority 10, subtask ids 11-18).
 *
 * @param low_period_us LOW's period/deadline (implicit deadline).
 * @param mode "single_core" (every subtask forced to core 0, no
 *        allocator call), "worst_fit"/"eru"/"tdta"'s own
 *        AllocationConfig::strategy value, or "single_core".
 * @return The built (and, for the three real strategies, allocated)
 *         plan.
 */
DeploymentPlan build_plan(uint64_t low_period_us, const std::string& mode);

} // namespace interference_topology
