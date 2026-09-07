# sqlicon CLI review and implementation

The shared libsqli/sqlicon version comes exclusively from `project(sqli VERSION ...)` in `CMakeLists.txt`. CMake generates the public `libsqli/version.h` header and the installed manpage from that value. `sqlicon --version` and `sqlicon -V` print `sqlicon <version>` without opening a connection or reading profiles. Debian packaging uses the generated manpage.

## Findings resolved

| Before | Result |
|---|---|
| No version option; manpage contained an unrelated hardcoded version and malformed `.TH` header | Generated version header and manpage; standalone `--version`/`-V` |
| No arguments always showed help despite the documented automatic mode selection | No arguments reach interactive mode for a TTY or batch mode for redirected stdin, using environment/profile connection settings |
| `--command/-c` and `--execute/-e` were duplicate interfaces; German/spelling aliases enlarged the interface | Keep `--command/-c`, `--password`, `--db-locale`, `--connect-uri`; remove the other aliases |
| Repeated options silently overwrote earlier values | Reject duplicates, including mixed short/long spellings |
| A missing value could consume the next option | Reject missing values; accept `--option=value` for dash-prefixed values, including SQL beginning with a comment |
| `atoi` accepted partial/invalid error numbers | Validate the complete signed decimal code, reject overflow and the unsupported negation boundary; preserve negative Informix codes |
| Help/error lookup and profile/SQL actions could silently suppress each other | Help, version and error lookup must stand alone; reject conflicting operations before side effects |
| Profile actions accepted ignored profile names, connection fields and log options | Validate action-specific options; require fields for profile updates and `--yes` for deletion |
| Profile defaults took precedence over environment values | Normal connection precedence is CLI > environment > profile |
| An explicitly empty password could be replaced by a stored/environment password | Preserve empty passwords as explicit values |
| URI connections loaded profiles and could acquire unrelated default credentials/locales | CLI URI connections bypass profiles and connection-environment merging; conflicting flags are rejected |
| URI-to-profile import lost TLS/Unix transport information | Remove URI profile import; keep direct URI connections and explicit profile fields |
| Profile-test could use environment credentials instead of those saved in the profile | Test saved connection fields; only log level may come from the environment |
| Profiles allowed arbitrary numeric port strings but rejected service names supported by direct connections | Shared validation accepts ports 1..65535, service names and Unix socket paths |
| Help/manpage omitted rules and environment settings; profile path and repository URL were stale | Synchronize documentation and package the generated manpage |

## Retained options

- Execution: `--command/-c`, `--file/-f`; otherwise automatic TTY/stdin mode.
- Information: `--help/-h`, `--version/-V`, `--finderr`.
- Profile selection: `--profile/-p`.
- Profile actions: `--profile-create`, `--profile-show`, `--profile-list`, `--profile-update`, `--profile-delete`, `--profile-default`, `--profile-test`.
- Action modifiers: `--show-secret` only with profile-show, `--yes` only with profile-delete.
- Connection fields: `--host`, `--port`, `--server`, `--database`, `--user`, `--password`, `--client-locale`, `--db-locale`.
- Direct URI connections: `--connect-uri`.
- Connection diagnostics: `--log-level`.

Profile management remains useful as-is; no subcommand redesign is needed for this change. Environment variables supply connection defaults, not destructive actions. Profile create/update deliberately use explicit flags only. URI parameters remain authoritative at the CLI layer; native transport behavior such as sqlhosts lookup is still implemented by libsqli.

## Validation

Two new CTest entries cover the argument parser, automatic mode selection, negative/error-number boundaries, conflicting actions, removed aliases, duplicate options, empty-password behavior, environment precedence, service validation, and a profile round-trip in a dedicated test directory. The profile tests use test-only platform adapters and key material; they do not access the user's profile store.

Process tests verify exact version output against the CMake version, help, error lookup and rejection of invalid combinations before profile actions. They also check the generated manpage version. Native Windows execution is not claimed by these Linux tests.

Validation results: all four CTest entries passed in the ASan/UBSan build (365 library/mock tests, the temporal generator, and the two new CLI entries). The CLI entries also passed in Release. Both complete builds succeeded with strict compiler warnings. A temporary CMake installation produced matching CLI/header/manpage versions, and groff rendered the manpage without diagnostics. An existing DESCRIBE test's socket descriptors were initialized to resolve a Release-only compiler warning exposed during this validation.
