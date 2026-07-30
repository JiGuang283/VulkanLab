# Project Instructions

## Default Verification Policy

- Optimize for development speed.
- After code changes, verify the affected configuration builds successfully
  and, when practical, that `VulkanLab.exe` can start and run.
- Do not run CTest, unit tests, visual regression tests, golden image tests,
  Validation smoke profiles, or other test suites by default.
- Run tests only when the user explicitly requests them or explicitly changes
  this policy for a task.
- If a change cannot be exercised locally after building, report that
  limitation instead of substituting a full test run.
