#! /bin/bash

usage () {
    echo "Usage: $(basename $0) target source" >&2
    echo "target: something like 'origin/master'" >&2
    echo "source: something like 'bugfix/foo' or 'feature/bar'" >&2
}

[ "$#" -ne 2 ] && { usage; exit 1; }

TARGET="$1" # most likely "origin/master"
SOURCE="$2" # most likely "bugfix/foo" or "feature/bar"

IGNORE_TYPES="FILE_PATH_CHANGES,LINE_SPACING,GIT_COMMIT_ID,SPLIT_STRING,\
PREFER_PRINTF,SPDX_LICENSE_TAG,BOOL_BITFIELD"

MAX_CHANGED_LINES_ALLOWED=400

GIT_SOURCE_HASH=$(git rev-list -n1 "$SOURCE")
GIT_TARGET_HASH=$(git rev-list -n1 "$TARGET")
GIT_MERGE_BASE=$(git merge-base $GIT_SOURCE_HASH $GIT_TARGET_HASH)

ALL_COMMITS=$(git rev-list --no-merges \
		  "$GIT_MERGE_BASE..$GIT_SOURCE_HASH")
for COMMIT in $ALL_COMMITS
do
    COMMIT_CHANGES=$(git show --format=format:'' --shortstat "$COMMIT")

    REGEX_INSERTIONS=".* ([0-9]+) insertions.*"
    [[ "$COMMIT_CHANGES" =~ $REGEX_INSERTIONS ]]
    INSERTIONS="${BASH_REMATCH[1]}"

    REGEX_DELETIONS=".* ([0-9]+) deletions.*"
    [[ "$COMMIT_CHANGES" =~ $REGEX_DELETIONS ]]
    DELETIONS="${BASH_REMATCH[1]}"

    TOTAL_CHANGE=$(( INSERTIONS + DELETIONS ))

    #echo "$COMMIT $COMMIT_CHANGES"
    #echo "Insertions:$INSERTIONS Deletions:$DELETIONS"
    #echo "Total Change:$TOTAL_CHANGE"

    if [ "$TOTAL_CHANGE" -gt "$MAX_CHANGED_LINES_ALLOWED" ]; then
	echo "$COMMIT is greater than $MAX_CHANGED_LINES_ALLOWED lines,"\
	     "please consider splitting into multiple commits."
	COMMIT_TOO_LARGE=true
    fi
done

checkpatch.pl \
    --no-tree --no-signoff --show-types --emacs \
    --ignore "$IGNORE_TYPES" \
    --git  "$GIT_MERGE_BASE..$GIT_SOURCE_HASH"
CHECKPATCH_EXIT_STATUS=$?

if [ "$COMMIT_TOO_LARGE" = "true" ] || [ $CHECKPATCH_EXIT_STATUS -ne 0 ]; then
    exit 1
fi
