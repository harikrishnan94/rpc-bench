# AGENTS.md

## Scope
- These instructions apply to the entire repository.
- Do not assume, invent, fill in gaps, or silently choose unspecified
  behavior. If something is unclear, missing, or ambiguous, stop and ask for
  explicit user approval before proceeding.

## Project Baseline
- Build system: Meson.
- Dependency acquisition method: WrapDB.
- Language standard: C++23.

## Required Workflow

1. Inspect the current repository state before making changes.
2. Look up the latest relevant official documentation before proposing
   or making changes.
3. Base all recommendations and code changes on the current repository
   state plus the latest documentation.
4. Break every edit task into milestone-based patches that are easy to
   review, verify, and test.
5. When a task includes commits, record each verified milestone as its
   own separate commit.
6. Use subagents for bounded subtasks whenever there is an opportunity
   to decompose the work safely so the main agent keeps its context
   focused on the critical path.
7. If documentation, repository state, or user intent conflicts or is
   incomplete, pause and ask the user instead of guessing.

## Plan Mode Clarification Policy

- During any task-planning or plan-mode exchange, ask as many targeted
  clarification questions as needed to remove ambiguity before making
  assumptions or committing to a plan.
- Do not treat "reasonable assumptions" as acceptable in plan mode when
  the answer could change design, scope, interfaces, persistence,
  testing, performance, usability, maintainability, readability, or
  correctness.
- Plan-mode questions must cover the missing decisions that materially
  affect the work, including scope, acceptance criteria, priorities,
  constraints, interfaces, data expectations, failure handling, edge
  cases, migration needs, and operational concerns when relevant.
- When more than one viable design or implementation path exists,
  present the alternatives explicitly instead of collapsing them into a
  single recommendation or question.
- For each alternative, explain the trade-offs in the dimensions that
  matter for the task, including performance, maintainability,
  readability, usability, correctness, safety, testability,
  operational complexity, and future extensibility when relevant.
- Every plan presented for approval must make the expected consequences
  explicit so the user understands what will improve, what will become
  more complex, what risks remain, and what behavior or limitations to
  expect from the chosen design.
- The goal of plan mode is to eliminate surprises for the user before
  implementation starts; favor complete clarification over speed.

## Documentation Policy

- Always consult the latest official documentation relevant to the task
  before changing build files, dependencies, compiler settings, module
  structure, or public APIs.
- Treat documentation lookup as mandatory, not optional.
- Do not rely on stale memory for Cargo, crates.io, the Rust compiler,
  or standard library behavior when current documentation can be checked.
- If the latest documentation cannot be accessed, say so clearly and
  wait for user approval before continuing.

## Delegation and Verification Policy

- All tasks handled by agents must use subagents whenever there is an
  opportunity to reduce the main agent's context usage.
- Keep only the blocking critical-path reasoning in the main agent when
  possible; delegate repository exploration, isolated implementation
  work, and independent verification to subagents whenever there is an
  opportunity to do so safely.
- If a task cannot be meaningfully decomposed, explicitly say why the
  main agent had to keep the work local.
- Every task that edits files must include an independent verification
  run through a fresh subagent that did not produce the edits.
- Milestone verification must be scoped so each milestone can be
  reviewed, tested, and committed independently when commits are part of
  the task.
- The fresh verification subagent must review the final diff, run or
  inspect the relevant checks, and report completion as two percentage
  scores: a confidence score and an accuracy score, together with any
  remaining gaps or risks.
- Do not treat an edit task as complete until that fresh subagent
  verification has been reported.

## Architecture Documentation
- Treat `ARCH.md` as the single source of truth for the project's
  architecture, behavior specification, and design decisions.
- `ARCH.md` must define the intended behavior and semantics of the database in
  language-agnostic terms so the library can be reimplemented in another
  language without changing observable behavior.
- `ARCH.md` must capture the full behavioral spec, including externally
  visible semantics, invariants, edge cases, ordering rules, persistence and
  recovery expectations, error cases, and any other non-trivial details needed
  to reproduce the same results.
- `ARCH.md` must describe the high-level implementation approach in
  programming-language-agnostic terms, including major data structures,
  algorithms, file/layout rules, performance characteristics, and important
  trade-offs that affect behavior or operational expectations.
- `ARCH.md` must record significant design decisions together with the
  rationale behind them whenever that context is needed to preserve behavior,
  semantics, or maintenance intent across future rewrites.
- `ARCH.md` must be detailed enough that an agent can use it as the single
  point of truth to rewrite the library in any language and still reproduce
  the same semantics and behavior.
- Every task must include an explicit decision about whether `ARCH.md` needs
  to be updated.
- Update `ARCH.md` whenever the core architecture changes or evolves.
- Update `ARCH.md` with every significant codebase change that affects the
  overall system structure, design, behavior, semantics, edge cases, or
  implementation constraints that `ARCH.md` is expected to capture.
- If `ARCH.md` is not updated for a task, the agent must explicitly state why
  no update was required.

## Build And Dependency Rules
- Use Meson for configuration, compilation, testing, and project structure changes.
- Before configuring or building, inspect the existing Meson build directories
  and determine whether one is already configured with the repository's local
  native files.
- Always use the repository's local native files for Meson builds. Prefer the
  local debug native file when choosing or creating a build directory unless a
  different local native file is clearly more appropriate for the task.
- If an existing build directory is already configured correctly with the
  needed local native file, reuse it instead of creating a new one.
- Only configure a new build directory when no suitable existing build
  directory can be inferred from the repository state, and when doing so use
  the appropriate local native file explicitly.
- All dependencies added to the repository must be added via WrapDB.
- Use WrapDB for third-party dependencies unless the user explicitly approves a different method.
- Do not replace WrapDB with vendoring, manual source drops, system-package
  assumptions, or FetchContent-style workflows without explicit approval.
- Keep Meson files consistent with the repository's existing Meson layout and naming.

## Testing And Coverage Rules
- Use Meson for test execution and coverage workflows.
- Use the repository coverage target to produce both the text summary and the
  HTML coverage report.
- Agent-generated tests must include coverage verification for the agent's
  changes.
- For agent-generated changes, the agent must achieve 100% measured coverage
  for the relevant new or modified code whenever feasible.
- If 100% coverage cannot be achieved, the agent must explicitly tell the
  user, explain the reason, and offer concrete suggestions for closing the
  remaining gap.
- Do not claim coverage goals were met without running the relevant coverage
  workflow and reporting the result.

## C++ Rules
- Target C++23.
- Do not assume a compiler-specific module setup is correct without checking
  the latest Meson and compiler documentation first.
- Do not introduce fallback behavior for older C++ standards unless the user explicitly requests it.

## Commenting Rules
- Any newly created non-trivial source or header file must start with a short
  overview comment describing its responsibility and boundary with neighboring
  files.
- Every new or materially changed declaration in `include/` must have a
  nearby comment covering its purpose, important constraints, failure
  behavior, and any non-obvious ownership or lifetime rules.
- New public enums, option structs, overloads, and other public API additions
  must not be left undocumented.
- Keep overview comments at the top of each non-obvious class so readers can
  quickly understand why the class exists, how it should be used, and any
  important constraints or pitfalls.
- Every new non-obvious class or struct in `src/` or `tests/` must have an
  overview comment explaining why it exists, what state or invariants it owns,
  and how it is meant to be used.
- Add clear, concise why and how comments to all non-trivial functions,
  routines, utilities, and complex code paths.
- Every new non-trivial free function, syscall wrapper, persistence helper,
  parsing helper, fault-injection utility, and similar helper must have a
  short why/how comment unless that behavior is already explained by an
  immediately adjacent overview comment.
- Prefer plain language over unnecessary jargon so comments remain accessible
  to developers with varying levels of experience.
- Keep comments structured, accurate, and non-redundant; do not restate what
  the code already makes obvious.
- Use comments to capture design decisions, trade-offs, assumptions, and
  potential pitfalls when that context will help future maintenance.
- Update comments whenever the surrounding code changes so they remain
  accurate and relevant.
- `ARCH.md` does not replace inline code comments. Architectural updates do
  not satisfy the requirement to document new code where readers will use and
  maintain it.

## Documentation Completion Gate
- Before finishing any task, inspect the staged and unstaged diff and verify
  comment coverage for new public API surface, new non-obvious types, new
  complex helpers, and new test infrastructure.
- Do not treat a task as complete while comment coverage for those changes is
  missing or obviously thinner than the surrounding codebase.
- If you cannot confidently document a new behavior, invariant, ownership
  rule, or failure mode from the repository state and latest documentation,
  stop and ask the user instead of shipping minimally commented code.
- Final responses must explicitly state which files received comment or
  documentation updates.
- Final responses must also list any changed files that intentionally did not
  need new comments, together with a brief reason.

## Codegen Priorities
- Optimize first for readability, maintainability, and reducing cognitive
  overload for future readers.
- After readability and maintainability are satisfied, optimize for reducing
  incremental compilation time.
- Prefer `std::format`, `std::print`, and `std::println` over traditional
  formatting, ad hoc buffers, and stream-heavy output unless a clear,
  documented reason makes another choice better.
- Prefer supported modern standard features and actively look for
  opportunities to use them when they improve clarity, maintainability, and
  compile-time behavior.
- Treat the inventories below as the verified intersection for Clang 22,
  GCC 15, latest libc++, and latest libstdc++; re-verify and update them
  whenever the supported toolchain versions change.

### Supported C++23 Language Features
- Literal suffix `uz` and `z` for `size_t` and `ssize_t` (P0330R8).
- `deducing this` (P0847R7).
- `auto(x)`: decay-copy in the language (P0849R8).
- Make `()` in lambdas optional in all cases (P1102R2).
- `static operator()` (P1169R4).
- Narrowing contextual conversions to `bool` (P1401R5).
- Portable assumptions (P1774R8).
- Make declaration order layout mandated (P1847R4).
- `if consteval` (P1938R3).
- C++ identifier syntax using UAX 31 (P1949R7).
- Named universal character escapes (P2071R2).
- Multidimensional subscript operator (P2128R6).
- Allow duplicate attributes (P2156R1).
- Attributes on lambda-expressions (P2173R1).
- Mixed string literal concatenation (P2201R1).
- Trimming whitespaces before line splicing (P2223R2).
- Non-literal variables, labels, and gotos in `constexpr` functions
  (P2242R3).
- Simpler implicit move (P2266R3).
- Using unknown pointers and references in constant expressions (P2280R4).
- Delimited escape sequences (P2290R3).
- Support for UTF-8 as a portable source file encoding (P2295R6).
- Character sets and encodings (P2314R4).
- Consistent character literal encoding (P2316R2).
- De-deprecating volatile compound operations (P2327R1).
- Add support for preprocessing directives `elifdef` and `elifndef`
  (P2334R1).
- Extend init-statement to allow alias-declaration (P2360R0).
- Remove non-encodable wide character literals and multicharacter wide
  character literals (P2362R3).
- Support for `#warning` (P2437R1).
- Relaxing some `constexpr` restrictions (P2448R2).
- Relax requirements on `wchar_t` to match existing practices (P2460R2).
- The Equality Operator You Are Looking For (P2468R2).
- `char8_t` compatibility and portability fix (P2513R3).
- `consteval` needs to propagate up (P2564R3).
- `static operator[]` (P2589R1).
- Permitting `static constexpr` variables in `constexpr` functions
  (P2647R1).
- Lifetime extension in range-based `for` loops (P2718R0).

### Supported C++23 Standard Library Features
- `std::expected` (P0323R12).
- `std::flat_map` (P0429R9).
- `std::unreachable` (P0627R6).
- Monadic operations for `std::optional` (P0798R8).
- Support C atomics in C++ (P0943R6).
- Type trait to detect scoped enumerations (P1048R1).
- `basic_string::resize_and_overwrite` (P1072R10).
- `out_ptr` (P1132R8).
- Printing volatile pointers (P1147R1).
- `std::flat_set` (P1222R4).
- `ranges::find_last`, `ranges::find_last_if`, and
  `ranges::find_last_if_not` (P1223R5).
- Byteswapping (P1272R4).
- Making `std::type_info::operator==` `constexpr` (P1328R1).
- Deprecate `std::aligned_storage` and `std::aligned_union` (P1413R3).
- Iterator-pair constructors for `std::stack` and `std::queue` (P1425R4).
- Stop overconstraining allocators in container deduction guides (P1518R2).
- `string::contains` (P1679R3).
- `std::to_underlying` (P1682R3).
- Default arguments for pair forwarding constructor (P1951R1).
- Range constructor for `std::string_view` (P1989R2).
- Conditionally borrowed ranges (P2017R1).
- Formatted output, including `std::print` and `std::println` (P2093R14).
- `invoke_r` (P2136R3).
- Inheriting from `std::variant` (P2162R2).
- Prohibit construction of `std::basic_string` and
  `std::basic_string_view` from `nullptr` (P2166R1).
- Relax requirements for `time_point::clock` (P2212R2).
- Require `std::span` and `std::basic_string_view` to be trivially copyable
  (P2251R1).
- Making `std::unique_ptr` `constexpr` (P2273R3).
- `constexpr` `to_chars` and `from_chars` for integral types (P2291R3).
- `std::ranges::contains` (P2302R4).
- Zip views (P2321R2).
- Clarifying the status of the C headers (P2340R1).
- Pipe support for user-defined range adaptors (P2387R3).
- Conditional `noexcept` for `std::exchange` (P2401R0).
- A more `constexpr` `std::bitset` (P2417R2).
- `ranges::iota`, `ranges::shift_left`, and `ranges::shift_right`
  (P2440R1).
- `views::join_with` (P2441R2).
- `views::chunk_by` (P2443R1).
- `views::as_rvalue` (P2446R2).
- Standard library modules `std` and `std.compat` (P2465R3).
- `views::repeat` (P2474R2).
- Monadic functions for `std::expected` (P2505R5).
- Expose `std::basic_format_string` (P2508R1).
- Relaxing ranges just a smidge (P2609R3).

## Style And Quality Gates
- Generated and edited code must strictly obey the repository's
  `.clang-format` and `.clang-tidy`.
- Treat `.clang-format` and `.clang-tidy` as the source of truth for
  formatting and linting decisions.
- If either `.clang-format` or `.clang-tidy` is missing, do not infer or
  invent the style. Ask the user how to proceed before generating or
  mass-editing code.
- Do not introduce style changes unrelated to the task.

## Tool Resolution Rules
- For tools such as `clang-format` and `clang-tidy`, refer to
  `.local-tools.yml` for the tool paths.
- If `.local-tools.yml` is present, the paths defined there must take
  precedence in all cases.
- If `.local-tools.yml` is not present, search for the relevant tools on `PATH`.
- Do not ignore `.local-tools.yml` in favor of a `PATH` tool when both are
  available.

## Line Length Rules
- All files, including `.md` files, must use a maximum line length of 100
  characters.
- If a file cannot be formatted to fit this limit, or another protocol,
  standard, or external mandate requires a different layout, treat that case
  as an explicit exception.
- Outside those exceptions, the 100-character maximum is mandatory.

## Approval Boundaries
- Ask before introducing new dependencies, changing public interfaces,
  altering the module layout in a non-local way, or making assumptions not
  directly supported by the repository and latest documentation.
- When presenting options, clearly separate verified facts from proposals.
- When uncertain, prefer a short clarification request over speculative
  implementation.

## Commit Message Rules
- Commit messages generated by agents must be formatted properly.
- Wrap all commit message lines at 72 characters or fewer.
- Do not generate overly long subjects or body lines that exceed the
  72-character limit.

## Response Expectations For Future Tasks
- State what was verified from the repository.
- State what was verified from the latest documentation.
- Call out any missing information or assumptions that need user approval.
- State that the staged and unstaged diff was reviewed for documentation
  coverage before finishing.
- Explicitly list which files received comment or documentation updates.
- Explicitly list any changed files that did not need new comments, with a
  brief reason.
- Explicitly state at the end whether `ARCH.md` was updated and summarize the
  changes made, or state why no `ARCH.md` change was required.
- Keep changes minimal, traceable, and consistent with the existing codebase.
