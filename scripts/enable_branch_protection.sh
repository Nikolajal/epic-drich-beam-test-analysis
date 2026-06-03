#!/usr/bin/env bash
#
# Enable branch protection on dev (and main) requiring the clang-format
# gate to be green before a PR can merge.
#
# WHY: the clang-format workflow only has teeth if its read-only gate
# job (`clang-format gate`, see .github/workflows/clang-format.yml) is a
# REQUIRED status check.  Without that, `gh pr merge` merges the instant
# it is called — before the formatter has even run — which is exactly
# how an unformatted commit reached dev once (PR #25).  Branch protection
# lives in repo settings, not in the workflow file, so it is applied
# here, once, by a maintainer with admin rights.
#
# Run:  bash scripts/enable_branch_protection.sh
#
# Requires: gh CLI authenticated as a repo admin.  Re-running is safe
# (idempotent PUT).  The gate context must have run at least once on a
# PR before GitHub will accept it as a required check.

set -euo pipefail

REPO="${REPO:-Nikolajal/epic-drich-beam-test-analysis}"
GATE_CONTEXT="clang-format gate"   # = the `name:` of the format-gate job

for BR in dev main; do
    echo "── protecting ${BR} (require '${GATE_CONTEXT}') ──"
    gh api -X PUT "repos/${REPO}/branches/${BR}/protection" \
        -H "Accept: application/vnd.github+json" \
        --input - <<JSON
{
  "required_status_checks": {
    "strict": true,
    "contexts": ["${GATE_CONTEXT}"]
  },
  "enforce_admins": false,
  "required_pull_request_reviews": null,
  "restrictions": null
}
JSON
    echo "   ${BR}: required check '${GATE_CONTEXT}' + up-to-date branch"
done

echo
echo "Done.  PRs to dev/main now block until the clang-format gate is green."
echo "Tip: merge with 'gh pr merge --auto' so the merge queues until checks pass."
