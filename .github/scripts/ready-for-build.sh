#!/usr/bin/env bash
# shellcheck disable=SC2016  # The single-quoted GraphQL queries use their own $variables.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Moves the roadmap cards of the issues fixed by BEFORE..AFTER to "Ready for build". Cards only
# move forward: closed issues, issues that aren't on the roadmap and cards that are already
# Ready for build or Done stay as they are.
#
# DRY_RUN=1 prints what would change and writes nothing.
set -euo pipefail

PROJECT_NUMBER="${PROJECT_NUMBER:-1}"
READY_STATUS="Ready for build"
ZERO_SHA="0000000000000000000000000000000000000000"

# Prints the issue numbers that GitHub would close for the commit messages on stdin.
fixed_issues() {
    REPO="$REPO" perl -ne '
        my $r = quotemeta $ENV{REPO};
        while (/(?<![\w-])(?:close[sd]?|fix(?:e[sd])?|resolve[sd]?):?[ \t]+(?:(?:$r)?\#|https:\/\/github\.com\/$r\/issues\/)(\d+)\b/gi) {
            print "$1\n";
        }
    ' | sort -un
}

commit_messages() {
    GH_TOKEN="$COMPARE_TOKEN" gh api --paginate "repos/$REPO/compare/$BEFORE...$AFTER?per_page=100" \
        --jq '.commits[].commit.message'
}

load_project() {
    local project
    project=$(GH_TOKEN="$PROJECT_TOKEN" gh api graphql -f owner="${REPO%%/*}" -F number="$PROJECT_NUMBER" -f query='
        query($owner: String!, $number: Int!) {
            user(login: $owner) {
                projectV2(number: $number) {
                    id
                    field(name: "Status") {
                        ... on ProjectV2SingleSelectField { id options { id name } }
                    }
                }
            }
        }' --jq '.data.user.projectV2')
    PROJECT_ID=$(jq -r '.id' <<<"$project")
    STATUS_FIELD_ID=$(jq -r '.field.id' <<<"$project")
    READY_OPTION_ID=$(jq -r --arg name "$READY_STATUS" '.field.options[] | select(.name == $name) | .id' <<<"$project")
    if [[ -z "$READY_OPTION_ID" ]]; then
        echo "::error::The roadmap's Status field has no \"$READY_STATUS\" option."
        return 1
    fi
}

# Returns 1 only when GitHub refused or failed a request.
advance_issue() {
    local number="$1" response item
    if ! response=$(GH_TOKEN="$PROJECT_TOKEN" gh api graphql -f owner="${REPO%%/*}" -f name="${REPO#*/}" -F number="$number" -f query='
        query($owner: String!, $name: String!, $number: Int!) {
            repository(owner: $owner, name: $name) {
                issue(number: $number) {
                    state
                    projectItems(first: 50) {
                        nodes {
                            id
                            project { id }
                            fieldValueByName(name: "Status") {
                                ... on ProjectV2ItemFieldSingleSelectValue { name }
                            }
                        }
                    }
                }
            }
        }' 2>&1); then
        if [[ "$response" == *"Could not resolve to an Issue"* ]]; then
            echo "#$number: not an issue, skipped"
            return 0
        fi
        echo "::error::#$number: $response"
        return 1
    fi

    # The caller's `||` turns off errexit in here, so each read checks for itself.
    local state
    state=$(jq -er '.data.repository.issue.state' <<<"$response") || {
        echo "::error::#$number: unexpected response"
        return 1
    }
    if [[ "$state" != "OPEN" ]]; then
        echo "#$number: closed, skipped"
        return 0
    fi
    item=$(jq -c --arg project "$PROJECT_ID" \
        '.data.repository.issue.projectItems.nodes[] | select(.project.id == $project)' <<<"$response") || {
        echo "::error::#$number: unexpected response"
        return 1
    }
    if [[ -z "$item" ]]; then
        echo "#$number: not on the roadmap, skipped"
        return 0
    fi

    local item_id status
    item_id=$(jq -er '.id' <<<"$item") || return 1
    status=$(jq -r '.fieldValueByName.name // ""' <<<"$item") || return 1
    if [[ "$status" == "$READY_STATUS" || "$status" == "Done" ]]; then
        echo "#$number: already $status, skipped"
        return 0
    fi
    if [[ -n "${DRY_RUN:-}" ]]; then
        echo "#$number: would move from ${status:-no status} to $READY_STATUS"
        return 0
    fi

    if ! response=$(GH_TOKEN="$PROJECT_TOKEN" gh api graphql -f project="$PROJECT_ID" -f item="$item_id" \
        -f field="$STATUS_FIELD_ID" -f option="$READY_OPTION_ID" -f query='
        mutation($project: ID!, $item: ID!, $field: ID!, $option: String!) {
            updateProjectV2ItemFieldValue(input: {
                projectId: $project, itemId: $item, fieldId: $field,
                value: { singleSelectOptionId: $option }
            }) { clientMutationId }
        }' 2>&1); then
        echo "::error::#$number: $response"
        return 1
    fi
    echo "#$number: moved from ${status:-no status} to $READY_STATUS"
}

main() {
    if [[ "$BEFORE" == "$ZERO_SHA" || "$AFTER" == "$ZERO_SHA" ]]; then
        echo "The push created or deleted the branch, so there is no range to read."
        return 0
    fi

    local issues
    issues=$(commit_messages | fixed_issues)
    if [[ -z "$issues" ]]; then
        echo "No commit in this push fixes an issue."
        return 0
    fi
    if [[ -z "${PROJECT_TOKEN:-}" ]]; then
        echo "::error::The ROADMAP_PROJECT_TOKEN secret isn't set, so these issues weren't moved: $(tr '\n' ' ' <<<"$issues")"
        return 1
    fi

    load_project
    local number failed=0
    while read -r number; do
        advance_issue "$number" || failed=1
    done <<<"$issues"
    return "$failed"
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    main
fi
