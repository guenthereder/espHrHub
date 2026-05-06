# Project: espHrHub

Stack: ESP32 / Arduino (arduino-cli)
Board: esp32:esp32:esp32
Test framework: compilation gate (no host-side unit test runner for Arduino)

# Definition of Done

A task is NOT complete until BOTH:
1. `make verify` exits 0.
2. The reviewer agent returns APPROVE (no must-fix findings).

If either fails, iterate. Do not declare done.

# Workflow

For non-trivial changes (multiple files, API changes, > 30 min work):

1. **Plan** — invoke `@planner` with the task. Wait for plan file and user approval.
2. **Test** — invoke `@tester` to write failing tests against the plan.
3. **Implement** — invoke `@coder` to make tests pass. Coder iterates against
   `make verify` until green.
4. **Review** — invoke `@reviewer` on the diff. Address must-fix findings.
5. **Commit** — conventional commit message describing the change.

For trivial changes (one file, obvious fix), skip planner/tester, go straight to
coder. Reviewer is still mandatory.

# Commands

- `make verify` — runs all gates. Must pass before done.
- `make test`   — runs tests only.
- `make lint`   — runs linter only.

# Project-specific rules

- Build system is arduino-cli. `make verify` compiles the sketch — no hardware needed.
  On-device flashing (`make upload`) requires the board connected via USB.
- Sketch entry point is `espHrHub.ino`. Keep setup()/loop() thin; logic in .h/.cpp files.
- arduino-cli requires the main sketch file to match the directory name (`espHrHub.ino`).
- Do not hard-code MAC addresses or secrets — use a `config.h` excluded from git.
- NimBLE / BLE callbacks run on FreeRTOS tasks. Any shared state needs a mutex or queue.
- Keep ISRs and BLE callbacks minimal; defer real work to the main loop.
- `make verify` only proves the code compiles. The reviewer agent catches logic issues.
