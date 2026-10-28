# Scoped market-data performance pilot — draft

## Outreach draft

**Subject:** A measurable optimization pilot for your market-data pipeline

Hi [Name],

I built Bybit FeedGuard, a reproducible prototype that records market messages
and compares C++ map, optimized C++ vectors and a native Goose implementation
on the same order-book events.

In a local experiment, Goose processed the recorded workload at
1.60x the median core throughput of optimized C++, with a 37.9% lower median
per-run p99 in separate instrumented measurements. The repository includes
the input, correctness checks, raw results and the limits of that experiment.

I would like to offer a small paid pilot on one agreed component of your
market-data pipeline. The work would establish a reproducible baseline,
identify the dominant cost, implement a focused change and measure the outcome
against correctness and latency criteria agreed with your team.

Would a short technical discussion help identify a suitable replay workload
and success criterion?

Best,
[Your name]

Repository: [add the approved repository URL after publication]

## Proposed pilot scope

**Workload:** one agreed instrument/feed, parser, replay path or L2 engine.

**Inputs requested:** a replayable sample or generator, build/run instructions,
target hardware constraints, required state semantics, and the primary metric
the team wants to improve.

**Deliverables:**

1. A reproducible baseline with compiler flags and input fingerprints.
2. Golden/differential correctness checks for the chosen component.
3. A focused implementation change with reviewable source and build commands.
4. Before/after throughput, relevant latency percentiles and process memory.
5. A concise report covering benefits, regressions, measurement limits and next steps.

**Acceptance:** identical agreed state/checksum results, completed required
tests, and an explicit report against the pre-agreed performance criterion.
Any performance target should be set after inspecting the client's baseline.
The FeedGuard ratios are observations from its own workload and hardware;
they are not a promised result for a client system.

**Commercial terms to agree:** fixed fee [amount/currency], timebox [duration],
start date [date], access and confidentiality terms, and ownership/license
of client-specific changes. These fields remain draft terms, not commitments.

## Supporting materials

- [Technical report](technical_report.md)
- [Reproduction guide](reproduction.md)
- [Throughput figure](../results/stage7-20261028/figures/throughput.png)
- [p99 figure](../results/stage7-20261028/figures/latency_p99.png)

This is a proposal draft for owner review, not a sent message.
