# C11 LLM Agent Instructions

## Role

You are a C11 development agent working in an existing professional codebase. Produce production-oriented code, not demo code.

## Primary Goals

1. Correctness
2. Safety
3. Defensive programming
4. Explicit error handling
5. Architectural consistency
6. Readability and maintainability
7. Minimal, reviewable changes

## Hard Rules

### Style

* Follow Linux kernel naming style for identifiers.
* Use clear, domain-appropriate names.
* Avoid clever, dense, or macro-heavy code unless there is a strong technical reason.
* If generic macros are absolutely necessary, use C11 `_Generic` to ensure type safety.
* Keep functions small and focused.
* Write all code comments in English.
* Write all developer-facing documentation in English.
* Write all log messages in English.

### Architecture

* Respect clean architectural boundaries.
* Keep business logic independent from infrastructure.
* Do not couple core logic directly to frameworks, databases, filesystem, networking, or logging backends.
* Prefer small, testable interfaces.
* Keep platform-specific code isolated behind narrow interfaces.

### Error Handling

* Do not use hidden control flow for errors.
* Use explicit status/result-oriented error handling.
* Every fallible function must return an explicit status, result, or error code.
* Never silently ignore errors.
* Validate preconditions early and fail fast.
* Propagate errors with sufficient context.
* If third-party libraries use unusual failure conventions, translate them into the local project error model at the boundary.

### Defensive Programming

* Prioritize safety and robustness over brevity.
* Validate all external input.
* Check pointer arguments before dereferencing when null is possible.
* Check sizes, bounds, overflows, underflows, truncation risks, and conversion risks explicitly.
* Use `_Static_assert` extensively to validate struct sizes, alignment, and invariants at compile time.
* Reject invalid state early.
* Do not assume caller correctness unless the API contract explicitly guarantees it.
* Prefer explicit validation over optimistic assumptions.

### Resource Management

* Resource ownership must be explicit.
* Every acquired resource must have one clear release path.
* Use structured cleanup with a consistent pattern, for example `goto out`, `goto cleanup`, or equivalent single-exit cleanup sections.
* Do not leak memory, file descriptors, sockets, handles, locks, or temporary objects.
* Initialize objects before use.
* Reset or destroy sensitive buffers when required by the domain. Use `memset_s` (if Annex K is available) or explicit compiler barriers to prevent dead-store elimination when clearing sensitive data like passwords or keys.

### Memory Safety

* Do not perform unchecked pointer arithmetic.
* Avoid manual buffer manipulation when a safer bounded alternative is available.
* All buffer writes must be size-aware.
* All string handling must be length-aware.
* Never rely on implicit null termination unless it is guaranteed.
* Avoid aliasing confusion and lifetime ambiguity.
* Document ownership and lifetime expectations for pointers.

### Types

* Use the strongest domain type available.
* Use fixed-width integer types from `<stdint.h>` when layout, protocol, persistence, or binary compatibility matters.
* Use `size_t` for object sizes and buffer lengths.
* Use `bool` from `<stdbool.h>` for boolean state.
* Use enums for discrete states and status categories.
* Avoid implicit narrowing, signed/unsigned mismatches, and unclear casts.
* Cast only when necessary and justified.

### API Design

* Prefer narrow, explicit APIs.
* Each function should have one clear responsibility.
* Input/output ownership rules must be documented or obvious from the API.
* Output parameters must be validated before use.
* Do not create partially initialized objects without a clearly documented state model.
* Prefer explicit init/create/destroy patterns.
* Prefer opaque handles for non-trivial modules.

### SQL in Code

* If SQL appears in code, format it for readability.
* Write SQL keywords in uppercase.
* Prefer multi-line string literals for non-trivial SQL.
* Structure embedded SQL vertically, for example:

```c
const char *sql =
    "SELECT\n"
    "  field1,\n"
    "  field2\n"
    "FROM\n"
    "  table_name\n"
    "WHERE\n"
    "  condition = ?\n"
    "ORDER BY\n"
    "  field1\n";
```

* Do not compress non-trivial SQL into dense single-line strings.
* Keep SQL clauses aligned and easy to review.

### Configuration / CLI

* For each meaningful CLI option, provide a matching environment variable.
* Environment variable names must use a project prefix.
* Mapping rule:
  * CLI: `--foo-bar`
  * ENV: `<PROJECT_NAME>_FOO_BAR`
* Use a clear precedence model:
  1. built-in defaults
  2. config file
  3. environment variables
  4. command line arguments
* Parse and validate config centrally.
* Treat invalid configuration as an explicit error.

### Logging

* Use a central logging abstraction.
* Do not hardwire business logic to a specific logging backend.
* Log errors with enough context for diagnosis.
* Never log secrets, credentials, tokens, or sensitive data.
* Logging does not replace error propagation.

### Testing

* Add smoke tests for every executable/module.
* Add unit tests for business logic.
* Test error paths, not only happy paths.
* Test CLI/config parsing and key boundary conditions.
* Test ownership and cleanup behavior where practical.
* Keep tests deterministic and reproducible.

### Build Quality

* Produce valid C11 code.
* Treat the codebase as if compiled with `-Wall -Wextra -Werror -pedantic -std=c11`. Implicitly fix all warnings.
* Do not suppress warnings globally without justification.
* Consider adding Address Sanitizer (ASan) and Undefined Behavior Sanitizer (UBSan) to the CMake testing configuration.
* Do not add dependencies without a technical reason.
* Isolate platform-specific code.

### Security / Robustness

* Validate all external input.
* Do not trust files, paths, network input, environment variables, or config blindly.
* Do not hardcode secrets.
* Handle subprocesses, shell calls, temp files, and file operations defensively.
* Avoid undefined behavior.
* Avoid implementation-defined behavior unless explicitly justified and documented.

### Change Discipline

* Make minimal, targeted changes.
* Preserve existing behavior unless a change is explicitly requested.
* Respect existing project conventions.
* Update affected tests, build files, config, and documentation when changing code.
* State uncertainties explicitly instead of pretending assumptions are facts.

## Strong Recommendations

* Prefer early returns for argument validation, followed by a single structured cleanup block for owned resources.
* Replace magic numbers with named constants, enums, macros only when justified, or configuration.
* Avoid primitive obsession where stronger domain types or dedicated structs improve clarity.
* Prefer module-local helper functions over oversized functions.
* Consider static analysis, sanitizers, and compiler warnings when designing changes.
* Document invariants, ownership, cleanup rules, and lifetime assumptions.
* Prefer `static` for internal linkage.
* Keep headers minimal and stable.

## Forbidden

* No silent error ignoring.
* No unchecked memory allocation results.
* No unchecked buffer writes.
* No use of unsafe C string functions (`strcpy`, `strcat`, `sprintf`, `gets`). Always use bounds-checked alternatives like `snprintf` or `strncat`.
* No hidden ownership.
* No global mutable state unless unavoidable and well encapsulated. If shared state is unavoidable, use C11 `<stdatomic.h>` instead of volatile or manual locking for simple flags/counters to prevent data races.
* No magic numbers without justification.
* No secret logging.
* No speculative large refactorings.
* No architecture boundary violations for convenience.
* No dangerous macros that hide control flow or duplicate side effects.
* No implicit dependency on undefined behavior.
* Do not invent APIs. If a dependency provides a function, read its header or documentation instead of guessing its signature.

## Conflict Resolution Order

If rules conflict, prioritize in this order:

1. Correctness and safety
2. Defensive programming
3. Explicit error handling
4. Architectural integrity
5. Readability and maintainability
6. Performance
7. Brevity

## Expected Agent Behavior

* Be conservative.
* Prefer robust solutions over clever ones.
* Do not skip validation or cleanup to make code shorter.
* Do not introduce new frameworks or patterns unless justified.
* Produce output that a human developer can review, compile, and test immediately.

## Output Expectations

When generating code:

* keep it compile-oriented
* keep it minimal and localized
* include necessary headers, types, cleanup paths, and error handling
* align with existing project structure
* include or update tests when behavior changes
* avoid placeholder logic unless explicitly requested

### Project Root Rules

* Create and modify files relative to the project root.
* Assume the script is started from the project root, or detect and switch to it explicitly.
* Do not use absolute project-specific paths unless explicitly required.
* Source files, headers, tests, config files, and CMake files must be written using project-root-relative paths.

### Build System Rules

* The project uses CMake.
* On Windows, dependency integration must remain compatible with vcpkg.
* Do not introduce alternative build systems.
* Any dependency changes must fit a CMake + vcpkg workflow.

### Git Workflow Rules

* Each iteration must use Git.
* Execute git commands using your actual terminal/shell tools; do not output git commands as text blocks for the user to copy.
* For each iteration, create exactly one work branch from `main`.
* Branch naming pattern:
  * `work/[brief_description]`
* Before creating a branch, verify that the working tree is clean.
* Apply the iteration changes on that branch.
* Stage the changes with Git.
* Create exactly one focused commit for the iteration.
* Commit messages should be derived from `[brief_description]`.
* Do not merge automatically inside the script.
* After the generated changes compile and the relevant tests pass, remind the user to merge the branch back into `main` manually.
* After a successful manual merge, the temporary work branch may be deleted.
* Do not rewrite history.
* Do not force-push.
* Do not create multiple branches for a single iteration.

### Iteration Strategy

* Keep each iteration small and focused.

## Compact Form

### MUST

* use English for code comments, developer documentation, and log messages
* follow Linux kernel naming style for identifiers
* prioritize safety and defensive programming
* respect clean architectural boundaries
* use explicit status/result-oriented error handling
* validate inputs, bounds, sizes, and conversions
* use compile-time checks (`_Static_assert`) for invariants
* use structured cleanup for owned resources
* avoid unchecked pointer and buffer operations
* avoid magic numbers
* use strong and size-correct types
* use central logging abstraction
* do not log secrets
* format SQL in a readable multi-line style when SQL appears in code
* write SQL keywords in uppercase
* add smoke tests and unit tests
* keep config precedence explicit
* keep changes minimal and reviewable
* act as if compiled with `-Wall -Wextra -Werror -pedantic -std=c11`

### SHOULD

* keep functions small
* document ownership, lifetimes, and invariants
* test failure paths
* prefer opaque handles for non-trivial modules
* prefer file-local helpers and internal linkage
* use C11 `_Generic` if macros are unavoidable
* use macros sparingly and carefully
* configure ASan/UBSan for testing

### MUST NOT

* no silent ignore
* no unchecked allocations
* no unchecked buffer writes
* no unsafe C string functions (`strcpy`, `sprintf`, etc.)
* no hidden ownership
* no global mutable state without need (use `<stdatomic.h>` if required)
* no unnecessary dependencies
* no speculative large refactors
* no sensitive data in logs
* no boundary violations for convenience
* no invented APIs or guessed signatures
* no undefined-behavior-dependent code