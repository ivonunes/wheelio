#!/usr/bin/env bash
# Opens, or updates in place, the pull request for an automated update:
# commits the working tree's changes to a fixed branch, force-pushes it and
# keeps one PR open for it. Used by the update-*.yml workflows and
# release-engine.yml.
#
#   scripts/open-update-pr.sh <branch> <title> <body file> [--draft]
#
# The branch is pushed with the checkout's deploy key, a real push that starts
# the PR's checks. Creating a PR needs GH_TOKEN (GITHUB_TOKEN), which starts
# none, so a new PR is pushed once more, unchanged, to start them.
set -euo pipefail

branch=$1
title=$2
body=$3
draft=${4:-}

git config user.name "github-actions[bot]"
git config user.email "41898282+github-actions[bot]@users.noreply.github.com"
git checkout -q -B "$branch"
git add -A
git commit -q -m "$title"
git push -q --force origin "$branch"

pr=$(gh pr list --head "$branch" --state open --json number --jq '.[0].number' || true)
if [ -n "$pr" ]; then
    gh pr edit "$pr" --title "$title" --body-file "$body" >/dev/null
    if [ -n "$draft" ]; then
        gh pr ready "$pr" --undo >/dev/null 2>&1 || true
    else
        gh pr ready "$pr" >/dev/null 2>&1 || true
    fi
else
    gh pr create --title "$title" --body-file "$body" --base main --head "$branch" ${draft:+--draft} >/dev/null
    sleep 1  # a new commit date, so the amended commit is a new commit
    git commit -q --amend --no-edit
    git push -q --force origin "$branch"
fi
