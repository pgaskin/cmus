#!/bin/bash
set -e

# our script name
self="_exp_reconcile_branches.sh"

# check the args
if [[ $# -eq 0 ]]
then
    echo "creates a synthetic merge commit from scratch out of the specified branches, then adds it to the history of the current branch"
    echo "usage: ${self} remote/branch[@refspec]..."
    echo "note: this works best with rerere enabled"
    echo "author: Patrick Gaskin"
    exit 2
fi

# get some basic info about the repo
root="$(set -e; git rev-parse --show-toplevel)"
base="$(set -e; git rev-parse HEAD)"

# create the worktree
worktree="${root}/reconcile"
if [[ -d $worktree ]]
then git worktree remove -f "${worktree}"
fi

# do the merges
summary=$'Reconcile branches\n\n'
parents=( "-p" "${base}" )
for spec in "$@" # e.g., cmus/master or cmus/master@COMMIT
do
    branch="${spec%%@*}" # until first @
    remote="${spec%%/*}" # until first /
    commit="${sdf#*@}" # after first @
    commit="${commit:-$branch}" # use branch if no explicit refspec
    commit="$(set -e; git rev-parse "${commit}")" # resolve refspec to hash
    remote="$(set -e; git remote get-url "${remote}")" # resolve remote to url
    remote="$(set -e; echo "$remote" | sed -E 's/^.+(\/\/|@)github.com[:/](.+)(\.git)?$/\2/g')" # extract owner/name from gh remote if possible

    summary="${summary}* ${branch} (${remote}@${commit})"$'\n'
    parents+=( "-p" "${commit}" )

    echo
    echo "===== ${spec} (${commit})"
    echo

    if [[ ! -d reconcile ]]
    then
        git worktree add "${worktree}" "${commit}"
        cp "$0" "${worktree}/${self}"
        chmod +x "${worktree}/${self}"
        git -C "${worktree}" add "${worktree}/${self}"
        git -C "${worktree}" commit -m "${self}"
        continue
    fi

    if ! git -C "${worktree}" merge "${commit}" -m "merge ${spec} (${commit})"
    then
        echo
        if [[ ! -f .git/MERGE_HEAD ]]
        then
            echo "wtf: no merge conflicts in progress"
            exit 1
        fi
        while [[ -f .git/MERGE_HEAD ]]
        do
            echo "still contains merge conflicts, try again"
            sh || true
        done
    else
        echo
        echo merge done, starting shell to inspect
        sh || true # let the merge be tested or amended if necessary
    fi
done
merged="$(set -e; git -C "${worktree}" rev-parse 'HEAD^{tree}')" # get the final tree hash
reconciled="$(set -e; git commit-tree -S -m "${summary}" "${parents[@]}" "${merged}")" # create the final commit

# clean up the worktree
git worktree remove -f "${worktree}"

# show the final commit
echo
echo "====="
git --no-pager show "${reconciled}"
echo "====="
echo

# update the current branch to it if not dirty
git merge --ff-only "${reconciled}"
