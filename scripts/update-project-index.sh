#!/bin/bash

set -euo pipefail

README="README.md"
START_MARKER="<!-- PROJECTS:START -->"
END_MARKER="<!-- PROJECTS:END -->"
TEMP_FILE=$(mktemp)
PROJECT_LIST=$(mktemp)

trap 'rm -f "$TEMP_FILE" "$PROJECT_LIST"' EXIT

find . -mindepth 1 -maxdepth 1 -type d \
    ! -name '.git' \
    ! -name '.github' \
    ! -name 'media' \
    ! -name 'scripts' \
    -printf '%f\n' |
sort |
while read -r directory
do
    printf -- '- [%s](./%s/)\n' "$directory" "$directory"
done > "$PROJECT_LIST"

awk \
    -v start="$START_MARKER" \
    -v end="$END_MARKER" \
    -v projects="$PROJECT_LIST" '
    $0 == start {
        print
        while ((getline line < projects) > 0)
            print line
        close(projects)
        skip = 1
        next
    }

    $0 == end {
        skip = 0
        print
        next
    }

    !skip {
        print
    }
' "$README" > "$TEMP_FILE"

mv "$TEMP_FILE" "$README"
