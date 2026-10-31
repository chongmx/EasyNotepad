# 05 · Real workspaces show real data

**Problem.** The sample agent conversation, plan, proposed changes, review findings, comments and
checklist appear even after opening a real folder; asking the agent there gets canned replies and
the review tools invent findings.

**Design.**
- Sample data only in the sample workspace (no folder opened). `openFolder` switches the agent and
  the review to live mode and clears every sample.
- **Agent chat (live):** prompts go to the agent CLI (`EASYNOTEPAD_AGENT_CLI`, default `claude -p`)
  in the workspace folder, with the recent conversation and the user's current file/line as context;
  the answer is the reply. No CLI installed: the reply says how to set one up. Stop discards the
  pending answer.
- **Review tools (live):** each tool runs a command the user configures
  (`EASYNOTEPAD_LINT`, `EASYNOTEPAD_STATIC_ANALYSIS`, `EASYNOTEPAD_SECURITY`, `EASYNOTEPAD_STYLE`,
  `EASYNOTEPAD_COMPLEXITY`; `{file}` = the active file, `{workspace}` = the folder) through a new
  process service; its compiler-style output (`path:line[:col]: severity: message`) becomes findings.
  Agent Review asks the agent CLI to answer in the same format. Not configured: the tool says so and
  how to configure it, and reports no findings.
- Summaries in a real folder are labelled as coming from the code's comments and structure.

**Tests.** Unit: a real folder has no sample messages/findings/comments/checklist; a configured tool
(a fake process service) produces parsed findings; an unconfigured one reports it; the agent chat
goes to the (fake) agent CLI with context and shows its answer; the sample workspace keeps samples.
