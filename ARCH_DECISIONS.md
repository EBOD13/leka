# Architecture Decisions

Leka is a price-time-priority limit order book / matching engine in C++20.
This file records the decisions that shaped its hot path, in the order they
were made, with the measurements that justified them. Each entry follows a
short ADR (Architecture Decision Record) format: Context, Decision,
Consequences, Alternatives Considered, Status.

---

## ADR-001: Event model has no in-place MODIFY

**Status:** Accepted, 2026.

### Context

The original event model was `NEW / CANCEL / MODIFY`, where `MODIFY` carried
a new price and a new quantity. The matching engine inspected both fields and
decided, at dispatch time, whether the change could be applied in place
(price unchanged, quantity decreased: keep queue priority) or required a
remove-and-reinsert (price changed, or quantity increased: lose priority).
That decision was computed twice — once in `MatchingEngine::processEvent` to
decide whether to allocate a new sequence number, and again inside
`OrderBook::modifyOrder` to decide how to apply the change — against two
separate lookups of the same order.

It also had a correctness gap: `MODIFY` returned no executions
unconditionally, so repricing a resting order through the opposite side's
best price left the book crossed instead of trading.

Real venues don't have this primitive. Nasdaq TotalView-ITCH 5.0 has no
in-place modify message: `X` (Order Cancel) shrinks an order's size and keeps
its priority; `U` (Order Replace) retires the original `order_ref` and issues
a new one at the back of its queue. Measured on `20190730.BX_ITCH_50` (all
8,849 symbols, 24,074,237 events): `X` is 1.2% of flow, `U` is 8.5%.

### Decision

Replace `MODIFY` with two primitives that map onto what a venue actually
sends:

- `REDUCE{orderId, newQuantity}` — shrinks a resting order in place, same
  price, same queue position, same sequence number. This is the only change
  that preserves time priority, so it is the only one with a decision to make
  and the only one that needed a class of its own.
- A reprice or size increase is expressed as `CANCEL` followed by `NEW`. `NEW`
  is routed through the matcher, so a reprice that crosses the book now
  trades instead of leaving it crossed — the correctness gap is closed as a
  consequence of the representation, not by adding a special case for it.

### Consequences

- One hash lookup instead of two on every reduce; no duplicated priority
  decision.
- `NEW` is the only event that allocates a sequence number, which is now
  structurally obvious rather than conditional.
- Emitted event counts go up (`U` becomes two events), which matters when
  calibrating arrival-rate statistics from replayed data.

### Alternatives Considered

Keep `MODIFY` and just fix the crossed-book bug by routing price-reset
modifications through the matcher internally. Rejected: it would still
compute the priority decision twice, and it does not correct the mismatch
between Leka's event model and what a venue's feed actually contains, which
matters for the ITCH replay tool this event model needs to consume.

---

## ADR-002: Tick-indexed PriceLadder replaces `std::map<Price, PriceLevel>`

**Status:** Accepted, 2026.

### Context

`OrderBook` stored each side's resting levels as `std::map<Price,
PriceLevel>`, a red-black tree. Two costs followed from that choice:

1. **Allocation.** Every new price level cost a tree node allocation
   (`try_emplace`), and an emptied level was `erase`d, costing a deallocation.
   Allocator latency has an unbounded, unpredictable tail — the wrong kind of
   cost to pay on a path whose whole job is to have a bounded tail.
2. **Pointer chasing.** `getBestBid()`/`getBestAsk()` — called on every
   incoming order, resting or aggressive — walked the tree's rightmost or
   leftmost spine. Each `PriceLevel` was a separate heap allocation, so each
   hop was a likely cache miss. This was paid far more often than the
   allocation cost above, since best-of-book is queried on every order, while
   most orders join a level that already exists.

`Price` is already an integer with no operations beyond comparison. Nothing
about "the set of currently occupied price levels" requires tree ordering —
a price is naturally an index, not a search key, once the tick size and a
usable range are known.

### Decision

Replace both maps with `PriceLadder`: a fixed-range array of `PriceLevel`,
one slot per tick, indexed directly by `(price - minPrice) / tickSize`. Every
level is constructed once, at `PriceLadder` construction, and never destroyed
or reallocated for the ladder's lifetime — so `Order::priceLevel`, a raw
pointer cached at insertion, stays valid across every operation at any other
price.

The two sides share one implementation by choosing which direction price
increases with index: ascending for asks (index 0 = lowest = best ask),
descending for bids (index 0 = highest = best bid). "Best" is therefore
always "lowest occupied index" on either side.

Occupancy is tracked in a bitset, not by probing `PriceLevel::isEmpty()`.
Finding the best level after the current best empties is a hardware
find-first-set (`std::countr_zero`) over 64-bit words, not a scan of level
objects. The best index is additionally cached, so the common case — a level
away from the current best changing occupancy — costs one array write and one
bit flip, no search at all.

This requires a bounded, pre-configured price range and tick size, decided
once at construction (`OrderBook(minPrice, tickSize, levelCount)`); a price
outside that range throws rather than silently growing the book. `tools/itch/
itch_replay.cpp` sizes this automatically from a session's observed trading
range plus headroom (ADR-003).

### Consequences

- No allocation on the insert or remove path, ever, once the ladder exists.
- Best-of-book is an array read plus, in the common case, a cached index —
  not a tree descent.
- Two related redundant lookups were removed as a direct result of levels
  never being created or destroyed: insert no longer needs a rollback branch
  that erases a newly-created level on failure, and `OrderBook::removeOrder`
  now trusts `Order::getPriceLevel()`'s already-cached pointer instead of
  re-deriving the level from price and comparing — a check that
  `PriceLevel::removeOrder` already performs internally, so the map-based
  re-lookup was paying for a validation that was happening regardless.
- The book must be sized for a real price range up front; a 20%+ intraday
  gap outside that range throws instead of degrading. This is a real
  tradeoff, not a rounding error: it needs a deliberate range/headroom choice
  at construction, which `itch_replay` now makes from observed data instead
  of guessing (ADR-003).

### Alternatives Considered

- **Intrusive red-black tree** (avoid `std::map`'s node allocator, keep tree
  descent cost). Rejected: removes cost (1) but not cost (2), which the
  measurements below show is the larger of the two in a book with real depth.
- **Skip list.** Similar profile to a tree: no fixed bound needed, but still
  pointer-chases to find the best level. Rejected for the same reason as the
  intrusive tree.
- **Unmodified `std::map`.** Kept as the baseline for measurement below.

### Measured Results

Same benchmark binary, two static libs built from the same source tree at
different points (current `PriceLadder` vs. the `std::map` version at the
commit before this change), `-O2`, no sanitizers, isolated via a throwaway
git worktree so the comparison is apples-to-apples:

**Realistic case — 2,000 price levels per side, periodic best-bid/ask
queries, rolling cancel window, 500,000 operations, min of 7 runs:**

```
std::map<Price, PriceLevel>   min 273.3 ns/op
PriceLadder                   min 131.3 ns/op        → 2.1x
```

**Degenerate case — everything at one price level** (what the benchmark
measured before this change; kept here to show the ladder is not "free" —
it targets book depth specifically):

```
std::map<Price, PriceLevel>   min 146.5 ns/order
PriceLadder                   min 113.8 ns/order      → 1.3x
```

The single-level number is smaller and less clean than the multi-level one
because it bundles every other change from the same working session
(ADR-001's removed double lookup, `std::get_if` over `holds_alternative` +
`get`), since the comparison baseline predates all of them. It's included
for honesty about what a one-level workload can and can't show: an array and
a single tree node cost about the same to reach, so the ladder's actual
payoff is the multi-level number — a book with real depth, which is the case
that matters.

`benchmarks/benchmark_main.cpp` was rewritten to produce the multi-level
number as its standing regression benchmark (see ADR-004); the single-level
result above will not reproduce from the current benchmark binary, since that
scenario was specifically what was replaced.

---

## ADR-003: `itch_replay` sizes the ladder from observed data, not a guess

**Status:** Accepted, 2026.

### Context

ADR-002 requires a price range and tick size at construction. Choosing that
range by hand for a real instrument means either guessing generously (wasting
memory and, more importantly, hiding whether the sizing logic is even
correct) or guessing narrowly (risking the exact `std::out_of_range` failure
mode ADR-002 accepts as a tradeoff). Neither is a real answer, and there was
no code path that fed real market data through `OrderBook` at all — the
ladder had only been exercised by synthetic benchmarks and hand-written unit
tests.

### Decision

`tools/itch/itch_replay.cpp` reads a `tools/itch/itch_to_csv`-produced
session file in two passes. The first pass scans every priced row to find
the session's actual traded range, then adds a configurable headroom
percentage (default 20%) on each side before constructing the `OrderBook`.
The second pass replays the session through `MatchingEngine::processEvent`
in order, mapping `ADD_ORDER`/`ADD_ORDER_MPID` → `NEW`, `DELETE` → `CANCEL`,
`CANCEL_PARTIAL` → `REDUCE` (ITCH gives shares cancelled, not the remaining
quantity, so the replayed quantity is read from the book's own live state),
and `REPLACE` → `CANCEL` + `NEW` per ADR-001.

`EXECUTED`/`EXECUTED_WITH_PRICE` carry no order of their own — ITCH never
publishes the aggressor as a message, only its effect on the resting side —
so they are reconstructed exactly as validated by hand earlier against this
same file (2,154 of 2,154 executions grouped correctly, 99.2% touching a
single price level): group executions sharing an identical timestamp and
resting side, then inject one synthetic `NEW` on the opposite side, sized to
the summed executed quantity and priced at the worst price paid in the
group, an observed bound rather than a modeled one.

### Consequences

Running this against `20190730.BX_ITCH_50`, symbol AAPL, confirms the design
end to end:

```
rows scanned              191,812
observed price range      175.3600 .. 242.2300
ladder configured         161.9859 .. 255.6041, tick=1, levels=936,183 (37.45 MB/side)
reconstructed aggressors  2,154            (matches the hand-validated count exactly)

events replayed           NEW=95,015  CANCEL=93,676  REDUCE=1  AGGR=2,154
desyncs                   0 REDUCE-to-cancel, 0 REPLACE-with-unknown-order
resting levels at EOD     bid=0  ask=0     (the book fully unwinds)
```

Zero desyncs and a fully unwound end-of-day book are, together, an external
check that the aggressor-reconstruction rule from earlier in this project and
the `REDUCE`/`CANCEL`+`NEW` mapping from ADR-001 are internally consistent
against 191,812 real rows, not just the smaller hand-checked sample.

Per-event-type latency, measured on real data rather than synthetic load:

```
CANCEL  n=93,676  p50=83ns   p99=167ns   p99.9=666ns    max=10,375ns
NEW     n=95,015  p50=83ns   p99=209ns   p99.9=1,958ns  max=36,041ns
AGGR    n=2,154   p50=208ns  p99=667ns   p99.9=2,042ns  max=6,834ns
```

The tool does not model resting orders consumed by non-displayed ("hidden")
liquidity, which is why the mapping is described as a real-data validation of
the sizing and event-mapping logic, not a claim of full market replay
fidelity — full fidelity is the R-side simulator's job, feeding a wider event
stream into this same replay path.

### Alternatives Considered

Hand-picking a "generous" default range for common equities. Rejected: it
would have hidden exactly the bug class ADR-002 exists to make loud (an
out-of-range price failing silently or wastefully), and it would not have
exercised the aggressor-reconstruction logic against a real full-day file.

---

## ADR-004: Benchmark reports percentiles across a realistic mix, not one mean

**Status:** Accepted, 2026.

### Context

The original `benchmarks/benchmark_main.cpp` looped 100,000 times over a
single price level, alternating add and cancel, and printed one mean. Two
problems: a single level cannot distinguish `PriceLadder` from `std::map`
(ADR-002), and a mean is the wrong statistic for a latency-sensitive system —
it hides the tail, which is the number that actually matters when an order
book is on the hot path of a trading system.

### Decision

Rewrite the benchmark to build a book with liquidity resting across 2,000
price levels per side, then run a fixed-seed, mixed stream of resting `NEW`
(55%), crossing/marketable `NEW` (15%), `CANCEL` (25%), and best-of-book
query (5%) operations, timing each category independently and reporting
p50/p90/p99/p99.9/max, matching the reporting format `itch_replay` (ADR-003)
already uses on real data, so the two are directly comparable.

### Consequences

Release build (`-O2`, no sanitizers), 300,000 timed events:

```
BEST       p50=41ns    p90=42ns    p99=42ns    p99.9=42ns    max=167ns
NEW_REST   p50=84ns    p90=166ns   p99=291ns   p99.9=2,834ns max=951,250ns
CANCEL     p50=166ns   p90=500ns   p99=1,167ns p99.9=2,625ns max=27,250ns
NEW_CROSS  p50=167ns   p90=250ns   p99=542ns   p99.9=1,000ns max=10,500ns
```

`BEST`'s p99.9 and max sitting almost exactly at its p50 is the ladder's
cached-best-index design working as intended (ADR-002): the expensive
bit-scan path is reached only when the current best level itself empties,
which is rare relative to how often best-of-book is queried. `NEW_REST`'s max
being three orders of magnitude above its p50 is real and worth noting rather
than explaining away — it is consistent with an occasional allocator call
into `OrderPool` for a fresh page (see `include/lob/book/order_pool.hpp`),
which remains a real, unaddressed source of tail latency; it is not a
regression introduced by this change; the same effect exists in the
`std::map` version, just harder to see under fewer measured percentiles.

This benchmark, run before and after a future change, is now capable of
detecting a regression in depth-dependent behavior; the one it replaced was
not.

### Alternatives Considered

Report only a mean with a standard deviation. Rejected: still hides the
specific tail behavior (P99.9/max) that ADR-002's cached-best-index and
`OrderPool`'s page allocation both specifically target, and does not let a
future reader separate "engine got slower" from "engine's tail got longer,"
which are different bugs with different fixes.

---

## ADR-005: Executions are appended into a caller-owned buffer

**Status:** Accepted, 2026.

### Context

`MatchingEngine::processOrder`/`processEvent` returned `std::vector<Execution>`
by value. Return-value optimization elides the copy on the way out, but it
cannot eliminate the allocation the vector itself needs the first time
anything is appended to it: an empty vector has zero capacity, so the first
`emplace_back` on every order that executes at least once calls the
allocator. That is one malloc per aggressive order — precisely the pattern
`itch_replay` (ADR-003) hits on every reconstructed aggressor and every
crossing `NEW`.

### Decision

Give `MatchingEngine` a second form of `processOrder`/`processEvent` that
takes `std::vector<Execution>& out`, clears it, and fills it in place; the
core matching loop was pulled into a private `matchOrder(..., out)` that
never clears `out` itself, so both the buffer-taking and by-value overloads
funnel into one implementation. The original by-value overloads stay,
implemented as `std::vector<Execution> out; processOrder(..., out); return
out;` — identical allocation behavior to the code they replaced, so every
existing call site (tests, `itch_replay`'s `CANCEL`/`REDUCE` paths, anything
that doesn't run in a loop) keeps compiling and keeps its current cost
unchanged. `itch_replay` and `benchmark_main.cpp` were updated to declare one
`std::vector<Execution>` outside their hot loops and reuse it via the buffer
overload.

### Consequences

A caller reusing one buffer across many calls pays for at most one
allocation, ever, once that vector's capacity has grown to cover the largest
execution burst seen so far — not one allocation per call. A caller that
doesn't care (a test asserting on one order's result) is unaffected.

### Alternatives Considered

- **Callback per execution**, invoked from inside the matching loop instead
  of collecting a vector at all. True zero allocation even on the first
  call, but it changes the ergonomics of every existing call site (no more
  `.size()`, no more indexing into the result) for a benefit the buffer
  approach already captures after warmup. Rejected as unnecessary churn for
  the marginal gain of removing the one-time warmup allocation.
- **Small-vector optimization** (fixed inline capacity, spilling to the heap
  beyond it) on the return type itself, keeping the by-value signature.
  Rejected: it does not actually get to zero steady-state allocations for a
  crossing order that consumes more resting levels than the inline capacity,
  and the buffer approach achieves genuine zero-allocation reuse with a
  smaller code change.

### Measured Results

Isolated together with ADR-006 below, since both were implemented in the
same pass; see that entry's measurement for the combined and separated
effect.

---

## ADR-006: OrderIndex is open-addressed, not `std::unordered_map`

**Status:** Accepted, 2026.

### Context

`OrderIndex` wrapped `std::unordered_map<OrderId, Order*>`. It is queried on
every `NEW` (duplicate-ID check, twice removed from this path already — see
ADR-002's insert simplification), every `CANCEL`, every `REDUCE`, and every
fill — the single hottest lookup in the engine, hotter than `PriceLadder`'s
best-of-book query, since every event touches it and only some events touch
a price level. A chained hash table allocates one node per entry and chases
a pointer to reach it, the same two costs ADR-002 removed from the price
axis, just not yet removed from the order-ID axis.

Unlike price levels, order count has no natural fixed bound the way a
tick-indexed price range does for a given instrument, so this could not be
pre-sized the way `PriceLadder` was — it needs to grow.

### Decision

Replace the map with a hand-rolled open-addressed table: one flat
`std::vector<Slot>`, linear probing, growing by amortized doubling at a 0.5
load factor, same as `std::vector`'s own growth policy. `OrderId::isValid()`
already reserves zero as never a legitimate order ID, so a default-constructed
`Slot{}` doubles as the empty sentinel with no separate occupancy bitmap or
tombstone state needed.

Deletion is the subtle part of any open-addressed table without tombstones:
naively clearing a slot can strand a *different*, still-live key whose own
probe run passed through that slot, making it silently unreachable even
though it was never touched. `OrderIndex::removeOrder` uses backward-shift
deletion — after vacating a slot, every following entry in that probe run is
pulled back into it if doing so keeps that entry reachable — which is what
lets `findOrder` keep using "stop at the first empty slot" as a correctness
condition forever, with no accumulating tombstone debt across a long session
with many cancels.

Because that correctness property is exactly the kind of thing that can look
right on a handful of hand-picked cases and still be wrong, it is verified in
`tests/stress/test_randomized_operations.cpp`: 20,000 randomized add/remove
operations against a `std::unordered_map` oracle, with the entire live set
(not a sample) re-verified against that oracle after every single operation,
seeded for reproducibility. It passes clean under ASan and UBSan.

### Consequences

No per-order node allocation; lookups walk a contiguous probe run instead of
chasing a pointer per entry. Deletion costs more than a single slot write
(the backward-shift scan), in exchange for lookups that never need
tombstone-skipping logic and never degrade as more orders churn through the
book over a session.

### Alternatives Considered

- **Robin Hood hashing** (bounds the worst-case probe length by letting a
  new insertion displace an existing entry that is "richer," i.e. closer to
  its own natural slot). A real improvement for adversarial key patterns.
  Rejected for now as more complexity than this workload's key
  distribution (`OrderId` is date-scoped and effectively arrival-ordered)
  needs; worth revisiting if a pathological ID-generation scheme ever
  produces long probe runs in practice.
- **Chaining with a pooled allocator** (keep `std::unordered_map`'s
  chaining shape, but bump-allocate nodes from a page like `OrderPool`
  instead of using `new` per node). Rejected: it removes the allocation cost
  but not the pointer-chasing cost, and open addressing removes both with a
  simpler structure.

### Measured Results

Both this entry and ADR-005 were implemented together; measured separately
via a three-way comparison — a scratch build with only these two files
reverted to their pre-change form (`std::map`-derived `PriceLadder` work
from ADR-002 held constant on both sides), release, no sanitizers, same
seeded 300,000-operation mixed workload as ADR-004's benchmark, min of 5
runs:

```text
BEFORE (std::unordered_map index, by-value executions)         min 151.2 ns/op
AFTER, by-value calls only (open-addressed index alone)         min 123.4 ns/op   → 18.4% vs before
AFTER, buffer API (open-addressed index + reused execution buf) min 115.5 ns/op   → 23.6% vs before
```

The middle row isolates ADR-006 by holding the calling convention fixed
(by-value, discarding results, on both the before and after library) and
changing only `OrderIndex`'s internals. The bottom row isolates ADR-005 on
top of that, by holding `OrderIndex` fixed (both rows use the same library)
and changing only whether the caller reuses one buffer or accepts a fresh
allocation each call. Both effects are real and independent; combined they
are worth slightly more than either alone, as expected since they sit on the
same call path rather than competing for the same cycles.

---

## ADR-007: `Order::isValid()` runs once per insert, not twice

**Status:** Accepted, 2026.

### Context

`Order::isValid()` checks order ID, quantities, timestamp, side, type, and
price consistency — six field checks, no hashing, no memory access beyond
the `Order` itself. It was called twice on every accepted insert:
`OrderBook::addOrderWithQuantities`, right after allocation and before the
order is linked into a `PriceLadder`, and again inside `OrderIndex::addOrder`,
after the order was already linked into that ladder.

Unlike the double hash lookup ADR-002 and ADR-006 removed, this pair of
checks is cheap enough that it was flagged and deliberately left alone
earlier in this project: removing the `OrderIndex` copy trades a real,
if small, safety property — a reusable index component validating its own
precondition rather than trusting every caller forever — for a handful of
integer comparisons per insert, which did not look like a good trade in
isolation.

### Decision

Removed anyway, on inspection of what the second check was actually
guarding. `PriceLevel::addOrder` — the class between `OrderBook`'s check and
`OrderIndex`'s, already linking the order into the book by the time
`OrderIndex::addOrder` runs — does not call `order->isValid()` either. It
checks only what it owns: the order isn't null, its price matches the level,
it isn't already linked elsewhere, it isn't fully filled, and the quantity
addition doesn't overflow. `OrderIndex` re-checking the *entire* `Order` was
therefore not this codebase's pattern for defense in depth; it was the one
inconsistent holdout of it. The fix brings `OrderIndex::addOrder` in line
with `PriceLevel::addOrder`'s existing division of responsibility: check only
this index's own invariants (non-null, key not already present), document the
precondition that the caller has already validated the order as a whole, and
let `OrderBook::addOrderWithQuantities` — which runs first and is the only
call site that can still reject an order before it touches any book
structure at all — remain the single place that owns that check.

### Consequences

One `Order::isValid()` call per accepted insert instead of two. No test
depended on `OrderIndex` rejecting an invalid order on its own — the two call
sites are `OrderBook` (which always validates first) and the ADR-006 stress
test (which only ever constructs valid orders) — so this is a pure
subtraction, not a behavior change reachable through the public API.

No benchmark number is reported for this one. Six integer comparisons is
below what any measurement setup used elsewhere in this document could
reliably resolve from surrounding noise, and reporting a number anyway would
misrepresent how the earlier ADRs' numbers were produced.

### Alternatives Considered

Leave both checks in place, as originally decided. Revisited because the
original reasoning ("weakens the index's own invariant") assumed `OrderIndex`
validating the whole `Order` was this codebase's real safety pattern; once
`PriceLevel::addOrder`'s narrower, invariant-scoped checks were read again as
the actual precedent, keeping `OrderIndex`'s broader check became the
inconsistent choice rather than the conservative one.

---

## ADR-008: `reserveOrderCapacity()` moves OrderPool/OrderIndex growth out of the hot path

**Status:** Accepted, 2026.

### Context

ADR-004's benchmark showed `NEW_REST`'s max sitting three to four orders of
magnitude above its p50 (84ns typical, up to ~1ms observed). ADR-004's text
attributed this to `OrderPool`: a fresh page is `mmap`ped lazily, the first
time `allocate()` finds no free slot, and `mmap` is a syscall with a
genuinely unbounded tail. That was a reasonable hypothesis at the time. It
was not tested, and it turned out to be wrong.

Testing it meant isolating the claim rather than trusting it. `OrderPool`
was given a `reserve(orderCount)` that pre-creates every page a run will
need, so no page is ever created mid-run; a standalone driver calling
`OrderPool::allocate()` directly, with no `OrderBook` or matching involved,
confirmed `reserve()` works (page count is fixed from the start, zero growth
during the timed region) and that it measurably shrinks p99.9 (~2,900-3,080ns
down to ~1,700-1,830ns, five runs each). But it did **not** explain the ~1ms
`NEW_REST` max: that outlier reproduced identically, run after run, in the
full `MatchingEngine`-driven benchmark whether or not `OrderPool::reserve()`
was called (five runs each, before: 969,875-1,258,375ns; after: 962,583-
1,031,833ns — no separation at all). Something else was causing it, and
`OrderPool` reserve alone left it completely intact.

The other candidate was sitting in ADR-006's own "Consequences" section,
already named and already left unaddressed: `OrderIndex` still grows by
amortized doubling, because order count has no fixed bound to pre-size to
the way a price range does. Doubling isn't a fixed small cost — it rehashes
every currently-live entry into a new table, which is O(n) in whatever the
book's current live order count happens to be at that moment. Late in a run,
with tens of thousands of orders live, that is not a "rare, cheap" event; it
is a rare, expensive one, which is exactly the shape ADR-004's max/p50 ratio
was showing.

### Decision

Give `OrderIndex` its own `reserve(orderCount)`, matching `OrderPool`'s: grow
directly to the smallest capacity that fits `orderCount` under the load
factor, doing at most one rehash instead of however many doubling steps
would otherwise happen one at a time as the book fills. `OrderBook` exposes
one combined entry point, `reserveOrderCapacity(orderCount)`, that calls
both — callers should not need to know that two different internal
structures each have their own growth policy to size against; they have one
number to reason about (how many orders this book will hold) and one call to
make. `benchmark_main.cpp` and `itch_replay.cpp` were updated to call it
once, before trading/replay begins, sized generously above their respective
worst cases.

### Consequences

Rerunning the exact same full-`MatchingEngine` driver that showed the
reproducible ~1ms max, this time with `reserveOrderCapacity()` covering both
structures, five runs each:

```text
before (no reserve at all)        max  978,000 - 1,065,709 ns
after (OrderPool + OrderIndex)    max   59,125 -    79,750 ns
```

A 13-17x reduction, consistent across every run — this is the fix ADR-004
was actually looking for. The residual ~60-80μs is not investigated further
here; it is in the same range the standalone `OrderPool`-only driver showed
without any matching engine involved, which points at general OS scheduling
noise on a shared development machine rather than anything internal to
`OrderBook`.

### Alternatives Considered

Trust the original ADR-004 hypothesis and ship `OrderPool::reserve()` alone,
since it did produce a measurable, real improvement (the p99.9 numbers
above). Rejected once the full-benchmark comparison showed it left the
actual, much larger, reproducible outlier completely untouched — reporting
that fix as "done" would have been reporting a correlation-free win instead
of the one that mattered. The lesson generalizes past this one instance:
attribute a benchmark tail to a call site only after isolating that call
site's contribution specifically, not on the strength of "this looks like
the kind of thing that would cause a tail."

---

## ADR-009: A lock-free SPSC queue splits feed-handling from matching

**Status:** Accepted, 2026.

### Context

Every prior ADR in this document is about a single thread. Real venue-facing
systems are not: a feed-handler thread decodes/produces events and a
separate matching thread applies them, handed off through some queue,
because decoding and matching have different, independently-scaling costs
and because a slow matching engine should not stall the network read loop
(or vice versa). None of that shape existed here before this change.

### Decision

`include/lob/concurrency/spsc_event_queue.hpp`: a single-producer/
single-consumer queue of `OrderEvent`, wait-free (no lock, no CAS, no
retry loop — each side has exactly one writer for its own cursor, so a
correctly-ordered atomic store is sufficient). Two implementation choices
worth naming:

- **Cached cross-thread cursors.** Each side keeps a private, non-atomic copy
  of the *other* side's cursor, refreshed via an acquire load only when its
  own stale copy says the queue might be full (producer) or empty
  (consumer). A producer draining into a consumer that is keeping up almost
  never pays the cross-core load at all.
- **Cache-line-padded cursors** (`alignas(64)` on `head_`, `tail_`, and each
  side's cached copy). Without this, the producer's `tail_` store and the
  consumer's `head_` store would share a cache line and false-share on every
  single operation, a real, measurable throughput cost specifically because
  there is no lock here to hide it behind.
- **Placement-constructed raw storage**, not `std::array<OrderEvent,
  Capacity>` — the same technique `OrderPool` already uses for `Order`
  (`include/lob/book/order_pool.hpp`), reused here because `OrderEvent` has
  no default constructor by design (order_event.hpp) and a `std::array` of
  it would require one.

`tools/concurrency/threaded_replay_benchmark.cpp` demonstrates and measures
the split: parses an interchange CSV upfront exactly like
`benchmark_interchange.cpp` (ADR-005's methodology unchanged — nothing
timed here ever includes parsing), then runs a feed-handler thread pushing
into the queue against a matching thread popping and calling
`processEvent()`, reporting two latencies: `HANDOFF` (push to pop of the
same event) and per-event-type `processEvent()` cost, identically defined
to the single-threaded tool so the two are directly comparable.

### A real data race, caught by actually running this — not by inspection

The first version of the benchmark tool recorded each push's timestamp into
a plain `std::vector<uint64_t>`, written by the feed-handler thread and read
by the matching thread. `tryPush()`'s internal release-store only publishes
what was written *before* it in program order; a timestamp written *after*
`tryPush()` returns has no happens-before relationship to the consumer's
later read of it. In practice this surfaced as an apparent `HANDOFF` max of
`5,557,079,624,184,416` ns — the signature of a torn or stale read, not a
real duration.

This was not caught by inspection; it was caught by running the tool under
ThreadSanitizer, which is exactly why `test_spsc_queue` (below) and this
tool are both built under a `-fsanitize=thread` configuration in addition to
the normal ASan/UBSan one. Reconstructing the buggy version and compiling it
under TSan reproduced the exact race, at the exact line, on the first run:

```text
WARNING: ThreadSanitizer: data race
  Read of size 8 at 0x... by main thread:
    #0 main buggy_threaded.cpp:303
  Previous write of size 8 at 0x... by thread T1:
    #0 main::$_2::operator()() const buggy_threaded.cpp:285
```

Fixed by giving each timestamp its own `std::atomic<uint64_t>` with an
explicit release (producer) / acquire (consumer) pair, independent of the
queue's own internal synchronization. Confirmed clean by running the fixed
binary under the same TSan configuration against the same real data (zero
warnings) and by rerunning `tests/stress/test_spsc_queue.cpp` — two real
threads, 2,000,000 events through a 1,024-capacity queue (roughly 2,000
forced wraparounds), exact FIFO order verified — under both ASan/UBSan and
TSan separately (see `cmake/Sanitizers.cmake`'s `enable_tsan()`; it cannot
be combined with `enable_sanitizers()` in the same binary, so these are two
separate configure-and-test passes, matching how a real CI matrix would run
them).

### What HANDOFF actually measures, and why the queue is small on purpose

Release build, real 193,075-event interchange CSV, three runs:

```text
REDUCE/NEW/CANCEL processEvent() latency: p50 in the low tens of ns,
  comparable to the single-threaded benchmark_interchange.cpp baseline --
  matching cost itself is not materially affected by the pipeline split.

HANDOFF   min ~375-1,800 ns   p50 ~660,000-790,000 ns
```

That gap between min and p50 is not noise, and not a queue defect. `min`
is what the mechanism costs when the queue is not near capacity: sub-2μs,
consistent with the plain-atomic, no-CAS design. The p50 is dominated by
something else entirely: this benchmark's feed-handler thread does no real
decoding work (events are pre-parsed before either thread starts), so it
produces far faster than the matching thread consumes, and the queue runs
close to its capacity almost the entire replay. A typical event's wait is
therefore close to (queue depth ahead of it) × (mean per-event matching
cost) — with capacity 4,096 and matching averaging roughly 170-220ns/event,
that predicts ~700-900μs, matching the observed p50 closely.

This was tested, not asserted: rerunning with the capacity raised to
1,048,576 (256x) made the median **worse** — 14.4ms, not better — because a
much larger buffer just gives the same fast-producer/slow-consumer mismatch
more room to accumulate before anything pushes back, so most events end up
waiting for a much longer backlog to drain instead of being bounded by a
small cap. This is queueing theory doing exactly what it predicts for a
saturated single-server queue, and it is the argument for keeping the queue
small: a bounded buffer caps the staleness a lagging consumer can cause,
which is the actual reason production feed-handler/matching splits use a
small, fixed-capacity ring buffer rather than an unbounded one. A real feed
handler doing real per-message decoding work would keep the queue far
emptier on average than this synthetic, instant-production benchmark does,
and would show a `HANDOFF` distribution much closer to its `min`.

### Alternatives Considered

- **A blocking queue (mutex + condition variable).** Simpler, and the
  matching thread would not busy-yield while empty. Rejected for the same
  reason every prior ADR in this document rejected an allocation or a
  pointer-chase on the hot path: a lock has an unbounded, scheduler-dependent
  wait, which is exactly the kind of tail this whole project has been
  measuring and removing elsewhere.
- **MPMC (multi-producer/multi-consumer).** More general, but requires a
  CAS retry loop on both ends, which is strictly more expensive than SPSC's
  plain-atomic design for a problem — one feed handler, one matching
  engine — that does not need the generality.

### Measured Results

See the code block above; full percentile tables are in the tool's own
output, reproducible via:

```text
Rscript-free path: ./threaded_replay_benchmark <interchange.csv>
compared directly against: ./benchmark_interchange --input <interchange.csv>
```
