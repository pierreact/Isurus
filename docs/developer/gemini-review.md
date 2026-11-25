# Gemini PR review

Automated, comment-only review of pull requests by Gemini, on GitHub-hosted runners.

Files:
- `.github/workflows/gemini-review.yml`: triggers, gate, review, post
- `.gemini/commands/isurus-review.toml`: review prompt (priorities, severity, output JSON)
- `GEMINI.md`: imports `CLAUDE.md` and points to the documentation
- `.geminiignore`: files Gemini file tools do not read

## How it works

1. `gate` (token: `pull-requests: read`): decides if a review runs. Refuses fork PRs for every trigger, closed PRs, and automatic reviews within the cooldown.
2. `review` (token: `contents: read`, `pull-requests: read`; holds `GEMINI_API_KEY`): checks out the PR head without credentials, writes the PR metadata, the line-annotated diff and the previous bot comments to `.gemini/review/`, runs `google-github-actions/run-gemini-cli` with the prompt `/isurus-review`. Gemini returns JSON. A jq step validates it: severity and side normalized, inline comments only on lines present in the diff (others go to the summary), findings already posted dropped, at most `MAX_COMMENTS`, `@mentions` broken.
3. `post` (token: `pull-requests: write`; no checkout, no model, no Gemini key): rebuilds the request from known fields, forces `event: COMMENT`, posts one review. If GitHub rejects the inline placement, it posts the findings in the review body. If the review job failed, it posts the failure reason instead.

Triggers:

| Trigger | Condition |
|---|---|
| PR opened, reopened, ready for review | same-repository branch, not draft |
| PR updated (push) | as above, and the last Gemini review is older than `COOLDOWN_MINUTES` (30) |
| Comment `/gemini-review [focus]` on a PR | author is OWNER, MEMBER or COLLABORATOR; no cooldown |
| Actions > Gemini Review > Run workflow | `pr_number`, optional focus; needs write access; no cooldown |

## Setup

1. Create the key in Google AI Studio (https://aistudio.google.com/apikey) in a Google Cloud project with no billing account linked. A linked billing account moves the project to a paid tier (https://ai.google.dev/gemini-api/docs/rate-limits, "Usage tiers").
2. Repository > Settings > Secrets and variables > Actions > New repository secret: name `GEMINI_API_KEY`, value the key.
3. Recommended: Settings > Actions > General > Workflow permissions: select "Read repository contents and packages permissions" and clear "Allow GitHub Actions to create and approve pull requests". The workflow does not need either (each job sets its own permissions).

## GitHub permissions

| Job | Token permissions |
|---|---|
| workflow default | none (`permissions: {}`) |
| gate | `pull-requests: read` |
| review | `contents: read`, `pull-requests: read` |
| post | `pull-requests: write` |

Token: the automatic `GITHUB_TOKEN`, scoped to this repository. No GitHub App, no OAuth app, no personal token.

## Security model

- Gemini sees only: the prompt, the PR title and body, the diff, previous bot comments, and repository files through `read_file`, `list_directory`, `glob`, `grep_search` (workspace only, `.geminiignore` applies). `tools.core` is the allowlist; shell, write, edit and web tools are also excluded. No MCP server.
- The action runs `gemini --yolo` (no tool confirmation). Safety comes from the tool allowlist, not from confirmation.
- Gemini has no GitHub token. Prompt injection in a PR can change the text of the review, never approve, merge, push or post elsewhere. Only the `post` job writes, and it fixes `event` to `COMMENT`.
- The prompt must not contain the shell injection trigger or the args placeholder: Gemini CLI then runs its shell processor over the injected diff (`packages/cli/src/services/FileCommandLoader.ts`, v0.70.0). The `review` job refuses to run if either is present, or if any other file exists under `.gemini/`.
- The `review` job fails without posting if the Gemini output contains the API key.
- Fork PRs: `pull_request` from a fork gets no secrets, and `gate` skips it; comment and manual triggers on a fork PR are also skipped. `pull_request_target` is not used.
- Same-repository PRs run the workflow and prompt from the PR branch. Their authors already have write access and could change any workflow, so they are trusted.
- Pinned by commit SHA: `actions/checkout` v6.0.1, `run-gemini-cli` v0.1.22. Gemini CLI pinned to an npm version. The action uses `google-github-actions/auth@v3` and `actions/upload-artifact` by tag internally; both steps are skipped here (no Workload Identity, `upload_artifacts: false`).

## Data sent to Google

Free tier: "Content used to improve our products" (https://ai.google.dev/gemini-api/docs/pricing). Sent: the prompt, `GEMINI.md` and `CLAUDE.md`, PR title and body, diff, previous bot comments, and the files Gemini reads. The repository is public. Secrets are not in the workspace (checkout without persisted credentials) and the file tools cannot read outside it.

## Cost and limits

- Model `gemini-3.8-flash`: input and output "Free of charge" on the free tier (pricing page, checked 2026-11-25). Gemini 4 Carbon, Argon and Neon are not available on the free tier.
- Cost is 0 only while the key's project has no billing account. The workflow cannot detect the tier.
- Free-tier rate limits (RPM, TPM, RPD) are per project and shown only in AI Studio (https://aistudio.google.com/rate-limit); the docs page does not list them per model. Open question: the RPD of `gemini-3.8-flash` for this project.
- Requests per review: one per model turn, at most `maxSessionTurns` (20). Assumption, not measured; check `stats` in the Gemini step log of the first run.
- Diffs above `MAX_DIFF_BYTES` (200000) are skipped. Open question: the value is not derived from a measured token count.
- HTTP 503 (model overloaded, observed on `gemini-3.8-flash` on 2026-11-25: 10 attempts in 3.7 min, all 503) or HTTP 429 (quota): the review runs once more with `GEMINI_FALLBACK_MODEL` (`gemini-3.6-flash`, also "Free of charge" on the free tier).
- Any other failure, or the fallback fails too: the `post` job posts a comment-only review "Gemini review not posted: <reason>" with the run link; PR checks stay green. A failure notice does not start the cooldown. Retry with `/gemini-review`.
- GitHub Actions minutes: free for standard GitHub-hosted runners in public repositories (https://docs.github.com/en/billing/concepts/product-billing/github-actions).

## First test

1. Add the secret (Setup).
2. Merge this workflow to `master` first: `issue_comment` and `workflow_dispatch` use the workflow file from the default branch. `pull_request` uses the PR branch version, so the PR that adds the workflow already reviews itself.
3. Open a small same-repository PR. Expect: Actions > Gemini Review: `gate`, `review`, `post` green; a review by `github-actions[bot]` with a summary and inline comments marked `[SEVERITY] [confirmed|potential]`.
4. Push to the PR within 30 min: `gate` logs "skipped: previous review ... cooldown". Comment `/gemini-review focus on memory ordering`: a new review runs.
5. Check that the review state is "Commented", never "Approved".
