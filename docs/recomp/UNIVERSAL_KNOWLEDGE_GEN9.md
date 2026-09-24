# Recomp Gen 9 — Universal Knowledge Base

Gen 9 changes learned recompilation knowledge from **guest-address facts** into
versioned, confidence-scored **code identity evidence**.  The objective is to
reuse what Xenon learns across title updates, regional builds and eventually
other games which contain the same compiler/runtime/library routines, without
letting stale data override the current executable.

It builds on Gen 6 static discovery, Gen 7 runtime observations and Gen 8
semantic verification.  It does not replace any of them.

## Core rule

A remembered address is never universal knowledge.

```text
old revision
  address 0x82......
          |
          v
normalized function fingerprint
          |
          +-- instruction shape
          +-- entry anchor
          +-- CFG topology
          +-- constant shape
          +-- call neighbourhood
          |
          v
new revision / other image
  candidate moved address
          |
          v
full fingerprint + confidence match
          |
       accepted?
       /       \
     yes        no
      |          |
corroborating   discard nomination
analysis evidence
```

The first-block anchor is intentionally only a **nomination mechanism**.  A
candidate found from an anchor is removed again if the completed function does
not pass the stronger whole-function matcher.

## Versioned normalized fingerprint

`FunctionFingerprint::kVersion == 1` contains independent dimensions rather
than one opaque hash:

- `instruction_shape_hash`
- `entry_anchor_hash`
- `cfg_shape_hash`
- `constant_shape_hash`
- `call_neighborhood_hash`
- instruction count / byte size
- entry-anchor instruction count
- block count
- external-call count

Relocation-sensitive branch displacements and address-like D/DS immediates are
kept out of the primary instruction-shape identity.  Constants are retained in
a separate corroborating channel, allowing a function whose data addresses
changed between links to remain recognizable without pretending the constants
are identical.

CFG identity is based on ordered topology and local block relationships, not
absolute guest addresses.  Call-neighbourhood identity records call/branch
shape without embedding relocated external targets.

No one secondary dimension is trusted on its own.

## Match scoring and confidence

The current structural score is:

| Evidence | Weight |
|---|---:|
| whole instruction shape + count | 50 |
| entry anchor + length | 15 |
| CFG shape + block count | 20 |
| constants | 5 |
| call neighbourhood + external call count | 10 |

Whole-function instruction shape is mandatory.  The structural total is then
bounded by the record's own confidence.  The default acceptance threshold is
70/100 and is configurable with `--knowledge-min-score`.

This makes a stale learned record weaker than a curated/high-confidence record
without changing the bytes which define identity.  `KnowledgeMatch` is also a
non-semantic discovery source: it can corroborate a function, but does not by
itself become authoritative proof of a function boundary.

## Efficient cross-revision seeding

When knowledge seeding is enabled, Xenon builds an index of 4–8 instruction
entry anchors from sufficiently confident records and scans executable
sections for possible moved functions.

The scanner performs one bounded decode pass per candidate address and derives
all requested prefix hashes incrementally.  It does **not** rescan an address
once per knowledge record or once per anchor length.  This keeps the first
stage proportional to executable code size rather than to database size.

Blocked data regions and import thunks remain ineligible.  Every nominated
function must survive normal analysis plus the full Gen 9 matcher before it can
remain in the report.

## Universal record types

The JSONL schema can describe:

- ordinary functions;
- compiler helpers;
- CRT routines;
- runtime/SDK helpers;
- middleware routines;
- engine routines.

These are represented by `KnowledgeKind` rather than separate hard-coded
systems.  Xenon deliberately does **not** guess that an anonymous function is
`memcpy`, an engine helper, or a middleware routine merely because it looks
similar.  Such semantic labels must come from curated data or previously
verified knowledge.  Automatic analysis export uses the neutral `function`
kind unless a real name already exists.

This distinction is important: Gen 9 supplies the reusable recognition
machinery, not a fabricated signature database.

## JSONL persistence

The schema is `schemaVersion: 1`.  64-bit fingerprint values are serialized as
hexadecimal strings so JSON number precision cannot truncate them.

Each row also records:

- record id;
- knowledge kind;
- optional family and label;
- source effective-image hash;
- source guest address;
- observation count;
- confidence;
- whether the record was curated.

Repeated identical observations from the same source image/address are compacted
when saved.  Observation counts accumulate, confidence may increase, and a
curated record remains curated.  Records from a different executable revision
remain separate so cross-revision provenance is not erased.

The database's build fingerprint intentionally ignores JSONL ordering, duplicate
rows and hit counts.  It changes when semantic fingerprint/category/label or
matching confidence changes.  This gives deterministic prepared-artifact cache
keys while avoiding rebuilds caused only by log ordering.

## Developer workflow

`recomp-driver` accepts:

```text
--knowledge knowledge.jsonl
--knowledge-export knowledge.jsonl
--knowledge-min-score 70
--no-knowledge-seed
```

A typical accumulating workflow is:

```text
recomp-driver game.xex out --knowledge knowledge.jsonl \
  --knowledge-export knowledge.jsonl
```

The previously loaded knowledge is combined with the current analysis export;
exact repeated source records are compacted on save.

The normal `xenon-prepare` path also accepts `--knowledge`.  Its artifact cache
key includes the canonical knowledge-base fingerprint, so a prepared native
module cannot remain falsely "fresh" after semantically relevant knowledge
changes.

## Reports and diagnostics

Analysis report schema version is now 4 and analysis engine revision is 9.
Reports expose, per function:

- the five normalized fingerprint hashes and shape counts;
- accepted knowledge matches;
- knowledge kind/family/label;
- match score;
- whether the match crossed executable revisions.

Top-level diagnostics include:

- knowledge records loaded;
- functions fingerprinted;
- matches considered;
- matches accepted;
- cross-revision matches;
- entry-anchor seed candidates.

These counters make it possible to distinguish "the DB was loaded" from "the
DB actually changed discovery" when validating a random title.

## Safety properties

Gen 9 is deliberately fail-closed:

- an entry-anchor collision is never a completed match;
- whole-function normalized instruction identity is mandatory;
- stale source addresses are not copied into the new executable;
- knowledge is non-semantic boundary evidence;
- import/NX/data-region restrictions still apply;
- candidates which fail the completed fingerprint are removed;
- labels do not silently rename/replace module-provided symbols;
- records preserve their source-image provenance;
- knowledge changes invalidate prepared artifacts deterministically.

## Relationship to Gen 7 learning

Gen 7 runtime observations remain revision-scoped execution facts.  Its raw
runtime block hash is **not** reinterpreted as a Gen 9 normalized function
fingerprint.  A runtime observation may help current-revision discovery; once
that code has been analyzed into a complete function, Gen 9 can export the
stronger normalized identity for future revisions.

That separation prevents a short dynamically observed block from being given
the authority of a whole-function cross-revision match.

## What Gen 9 does not claim

Gen 9 does not ship a magically complete CRT/SDK/engine signature corpus, and
it does not use machine learning to override CFG facts.  It establishes the
safe, persistent format and matching pipeline into which curated and verified
signatures can be added over time.

It also does not make compilation incremental by itself.  Gen 10 builds on the
new stable identities with a content-addressed per-region IR/object dependency
graph, so a small analysis change does not force regeneration of an entire
title.
