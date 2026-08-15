---
name: govern-ai-assisted-development
description: Enforce a specification-first, architecture-governed workflow for AI-assisted software changes in VS Code. Use when planning, implementing, fixing, refactoring, or reviewing project code where the agent must preserve visual, functional, data, security, and architectural invariants; analyze impact before coding; keep changes minimal; run a single Verify task; and review the completed diff.
---

# Govern AI-Assisted Development

Treat specifications and invariants as contracts. Complete each gate in order; do not begin implementation before the specification and impact-analysis gates pass.

## Establish the project contract

Read repository instructions and inspect the project before proposing changes. Prefer the following governance structure:

```text
project/
├─ .vscode/
│  └─ tasks.json                 # Includes the Verify task
├─ AI.md                         # Agent workflow and non-negotiable rules
├─ docs/
│  ├─ ARCHITECTURE.md            # Boundaries and dependency direction
│  ├─ DESIGN_SYSTEM.md           # Tokens, components, and visual rules
│  └─ decisions/                 # Architecture decision records
├─ specs/
│  ├─ features/                  # Feature contracts and acceptance criteria
│  └─ invariants/                # Architecture, data, security, and UI rules
└─ tests/
   └─ architecture/              # Executable boundary checks
```

Do not create empty placeholder files. When governance is missing and the task includes project setup, create the smallest useful version based on evidence from the repository. Otherwise, identify the missing contract before changing code.

Use the files as follows:

- Keep `AI.md` short. Define the required workflow, commands, prohibited actions, and precedence of project rules.
- Record stable dependency rules and public module boundaries in `docs/ARCHITECTURE.md`.
- Record semantic tokens, reusable components, responsive behavior, and accessibility rules in `docs/DESIGN_SYSTEM.md` when the project has a UI.
- Give each behavior change a feature contract under `specs/features/`.
- Keep cross-feature truths under `specs/invariants/`; do not duplicate them in every feature spec.
- Convert critical architecture rules into automated checks under `tests/architecture/` where practical.

## Gate 1: Define the specification

Find the relevant feature spec before editing code. Create or update it first when behavior is new or changed. Include only relevant sections:

```text
Outcome
In scope / out of scope
User-visible behavior
Functional and data contracts
Visual contract
Architecture constraints
Failure and edge cases
Acceptance criteria
Verification evidence
```

Write acceptance criteria as observable, testable statements. Link affected invariants instead of copying them. Resolve contradictions by applying this precedence:

1. Explicit user requirement
2. Repository instructions and `AI.md`
3. Feature specification
4. Recorded invariants and architecture decisions
5. Existing implementation

Stop and request direction when a higher-priority requirement conflicts with a non-negotiable invariant or when a missing product choice would materially change behavior. Do not silently turn assumptions into requirements.

## Gate 2: Analyze impact

Search for existing implementations, public interfaces, design-system components, tests, and related decisions. Produce a concise pre-change assessment containing:

- Intended behavior and explicit non-goals
- Affected modules and dependency boundaries
- Public API, schema, state, security, and UI impact
- Invariants at risk
- Existing abstractions to reuse
- Planned files and tests
- Verification commands and key risks

Challenge every planned file. Remove any file that is not required by the specification, a test, or a necessary contract update. Do not code until this assessment is complete.

## Gate 3: Implement the minimum coherent change

Make the smallest change that fully satisfies the acceptance criteria.

- Add or update tests with the behavior change; prefer a failing test first when practical.
- Preserve documented dependency direction and module ownership.
- Reuse existing components and abstractions before introducing new ones.
- Keep domain rules out of presentation and infrastructure code.
- Use design-system tokens and components; do not invent local visual constants without justification.
- Avoid unrelated cleanup, speculative extensibility, duplicate abstractions, and broad renames.
- Do not change a public API, persisted schema, security boundary, or shared component contract unless the specification explicitly requires it.
- Record a decision when the change establishes or reverses a durable architectural rule.

Keep the project runnable after each coherent step. Inspect the working diff frequently so unrelated user changes remain untouched.

## Gate 4: Verify through one entry point

Use the VS Code task named `Verify` as the canonical local quality gate. If project setup is in scope and no equivalent exists, add `.vscode/tasks.json` and route `Verify` to existing project commands rather than duplicating tool configuration.

Make `Verify` run the applicable checks in fail-fast order:

1. Formatting check
2. Linting and static analysis
3. Unit tests
4. Integration tests
5. Architecture tests
6. Build or type check
7. Automatable acceptance tests

Keep individual commands usable outside VS Code and make the task cross-platform when the repository supports multiple operating systems. Never report success from partial checks without naming what was skipped and why. Treat new failures as unfinished work; distinguish pre-existing failures with reproducible evidence.

## Gate 5: Review the completed change

Review the final diff as a skeptical maintainer after `Verify` passes. Check:

- Every acceptance criterion has evidence.
- No invariant or dependency boundary is weakened.
- The implementation contains no unintended public, data, security, or visual change.
- Tests cover success, failure, and important boundary cases.
- No parallel abstraction, dead code, unexplained constant, or unrelated edit was introduced.
- Specifications, decisions, and implementation agree.

Fix material findings, rerun `Verify`, and repeat the review.

## Report the result

Finish with a compact handoff containing:

- Outcome and user-visible behavior
- Specification and implementation files changed
- Architecture, data, security, and visual impact
- Verification performed and results
- Known limitations, skipped checks, or follow-up work

Do not claim completion while required verification is failing or required evidence is missing.
