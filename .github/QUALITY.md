# CI quality baseline

Checks run on pushes and pull requests with read-only repository permissions.
GitHub Actions are pinned to immutable commits; Dependabot updates those pins.

Python and extension JavaScript checks compare finding counts by file, rule and
message against the PR base, ignoring line shifts. Existing findings remain
visible; new findings fail CI. Pushes compare against the previous commit.
Manual runs or new branches without a valid previous commit report the current
baseline. Syntax, builds, tests and project validators remain strict.

Type checks, Perl::Critic and Prettier begin as nonblocking diagnostic steps.
Their failures appear in Actions logs; they are not claimed as passing gates.
Tighten them after reviewing and fixing the legacy baseline. Luxeva has no
runtime test suite yet; syntax/HACS/hassfest do not replace behavioral tests.
The irrigation coverage report is informational, without a fabricated target.

ESP32 clang-format checks only changed lines in first-party C/C++ on PRs.
Firmware size builds both PR and base, reports binary growth and flash/RAM
usage, and leaves capacity enforcement to the real firmware build.

Choose required checks in branch rules only after the first CI results are
reviewed. These changes do not configure external Sonar accounts or branch
rules, and do not merge themselves.
