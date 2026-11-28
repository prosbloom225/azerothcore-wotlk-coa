#!/usr/bin/env bash
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)
base_refs=" main origin/main upstream/main origin/HEAD HEAD "
branch_types="fix|feat|chore|docs|test|refactor"

usage() {
    cat <<'EOF'
Usage: tools/seed-worktree.sh [options] <ref> [<worktree-path>]

Creates a git worktree at <ref>, copies the untracked modules and the source
checkout's Ninja build directory into it, and relocates that build directory
so the worktree builds incrementally. Ends with a dry run of the target.

  <ref>                 branch, remote branch or commit to check out
  <worktree-path>       default: ~/tmp/coa-<short name of --branch, or of <ref>>

Options:
  --source DIR          checkout to seed from (default: the current checkout)
  --build NAME          build directory name in both checkouts (default: build-main)
  --target NAME         Ninja target for the dry run (default: worldserver)
  --branch NAME         create branch NAME (<type>/<short-name>, type one of
                        fix, feat, chore, docs, test, refactor) at <ref>;
                        required when <ref> is main or origin/main
  --detach              check out <ref> detached
  --settings-from FILE  write <worktree>/conf/verify-all.json from FILE with the
                        build directory rewritten to the worktree's
  --allow-stale         continue when the source build has pending compiles
  --ignore-running      continue while other builds are running
  -h, --help            show this help

Branch handling without --branch/--detach: a local branch is checked out unless
another worktree holds it (then detached); a branch only on origin becomes a
local tracking branch; anything else is detached.

The source build directory is only read: the dry runs use a copy of its
build.ninja in a temporary directory.
EOF
}

die() { echo "seed-worktree: $*" >&2; exit 1; }
step() { echo "==> $*"; }

source_dir=""
build_name="build-main"
target="worldserver"
new_branch=""
detach=0
settings_from=""
allow_stale=0
ignore_running=0
positional=()

while [ $# -gt 0 ]; do
    case "$1" in
        --source) source_dir="$2"; shift 2 ;;
        --build) build_name="$2"; shift 2 ;;
        --target) target="$2"; shift 2 ;;
        --branch) new_branch="$2"; shift 2 ;;
        --detach) detach=1; shift ;;
        --settings-from) settings_from="$2"; shift 2 ;;
        --allow-stale) allow_stale=1; shift ;;
        --ignore-running) ignore_running=1; shift ;;
        -h|--help) usage; exit 0 ;;
        -*) die "unknown option $1 (see --help)" ;;
        *) positional+=("$1"); shift ;;
    esac
done

[ ${#positional[@]} -ge 1 ] && [ ${#positional[@]} -le 2 ] || { usage >&2; exit 2; }
ref="${positional[0]}"
[ -n "$new_branch" ] && [ "$detach" -eq 1 ] && die "--branch and --detach are exclusive"
if [ -n "$new_branch" ]; then
    [[ "$new_branch" =~ ^($branch_types)/[A-Za-z0-9._-]+(/[A-Za-z0-9._-]+)*$ ]] \
        || die "branch name $new_branch must be <type>/<short-name> with type one of ${branch_types//|/, }"
elif [ "$detach" -eq 0 ] && [[ "$base_refs" == *" $ref "* ]]; then
    die "$ref is a base branch; pass --branch <type>/<short-name> to start new work (or --detach)"
fi

if [ -z "$source_dir" ]; then
    source_dir=$(git rev-parse --show-toplevel)
fi
source_dir=$(cd "$source_dir" && pwd -P)
if [ -n "$new_branch" ]; then
    default_name="${new_branch#*/}"
else
    default_name="$ref"
fi
worktree="${positional[1]:-$HOME/tmp/coa-${default_name//\//-}}"
mkdir -p "$(dirname "$worktree")"
worktree="$(cd "$(dirname "$worktree")" && pwd -P)/$(basename "$worktree")"
source_build="$source_dir/$build_name"
build="$worktree/$build_name"

[ -e "$worktree" ] && die "$worktree already exists"
[ -f "$source_build/build.ninja" ] || die "$source_build is not a Ninja build directory"
[ -f "$source_build/.ninja_deps" ] || die "$source_build has no .ninja_deps"
log_header=$(head -1 "$source_build/.ninja_log")
case "$log_header" in
    "# ninja log v5"|"# ninja log v6") ;;
    *) die "unsupported Ninja log format: $log_header" ;;
esac
[ -z "$settings_from" ] || [ -f "$settings_from" ] || die "$settings_from not found"
for tool in git ninja cmake python3; do command -v "$tool" >/dev/null || die "$tool not found"; done

running=$( (pgrep -a -x 'ninja|cc1plus|cc1|make'; pgrep -af 'cmake --build') || true)
if [ -n "$running" ]; then
    echo "$running" >&2
    [ "$ignore_running" -eq 1 ] || die "builds are running (see above); rerun with --ignore-running to continue"
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT


seed() { python3 -B "$script_dir/seed_worktree.py" "$@"; }

dry_run() {
    local dir="$1" out="$2"
    cp "$dir/build.ninja" "$out.ninja"
    ninja -C "$dir" -f "$out.ninja" -n -d explain "$target" > "$out" 2> "$out.explain" || {
        cat "$out.explain" >&2
        die "dry run failed in $dir"
    }
    grep -E 'Building CXX object|Building C object' "$out" | sed -E 's/.*Building C(XX)? object //' || true
}

check_ccache() {
    local launcher base hash_dir
    launcher=$(sed -n 's/^CMAKE_CXX_COMPILER_LAUNCHER:[A-Z]*=//p' "$source_build/CMakeCache.txt")
    if [[ "$launcher" != *ccache* ]]; then
        echo "warning: $source_build does not use ccache; the worktree's compiles will not be cached" >&2
        return
    fi
    command -v ccache >/dev/null || { echo "warning: ccache is not installed" >&2; return; }
    base=$(ccache -k base_dir)
    hash_dir=$(ccache -k hash_dir)
    if [ -z "$base" ] || [[ "$source_dir/" != "$base"/* ]] || [[ "$worktree/" != "$base"/* ]]; then
        echo "warning: ccache base_dir (${base:-unset}) does not contain both $source_dir and $worktree;" \
            "compiles will not share cache entries across checkouts" >&2
    fi
    if [ "$hash_dir" != "false" ]; then
        echo "warning: ccache hash_dir is $hash_dir; set hash_dir = false to share entries across checkouts" >&2
    fi
}

step "checking ccache"
check_ccache

step "checking that $source_build is up to date for $target"
pending=$(dry_run "$source_build" "$work/source-dry.txt" | wc -l)
if [ "$pending" -gt 0 ]; then
    echo "$source_build has $pending pending compiles for $target" >&2
    [ "$allow_stale" -eq 1 ] || die "build it first or rerun with --allow-stale"
fi

step "checking Ninja command hashes in $source_build"
seed save-commands "$source_build" "$work/old_commands.json"
seed check-hashes "$source_build" "$work/old_commands.json"

step "creating worktree $worktree at $ref"
if git -C "$source_dir" remote get-url origin >/dev/null 2>&1; then
    git -C "$source_dir" fetch --quiet origin
fi
if [ -n "$new_branch" ]; then
    git -C "$source_dir" worktree add --quiet -b "$new_branch" "$worktree" "$ref"
elif [ "$detach" -eq 1 ]; then
    git -C "$source_dir" worktree add --quiet --detach "$worktree" "$ref"
elif git -C "$source_dir" show-ref --verify --quiet "refs/heads/$ref"; then
    holder=$(git -C "$source_dir" worktree list --porcelain \
        | awk -v b="branch refs/heads/$ref" '/^worktree /{w=substr($0,10)} $0==b{print w}')
    if [ -n "$holder" ]; then
        echo "branch $ref is checked out in $holder; checking out detached" >&2
        git -C "$source_dir" worktree add --quiet --detach "$worktree" "$ref"
    else
        git -C "$source_dir" worktree add --quiet "$worktree" "$ref"
    fi
elif git -C "$source_dir" show-ref --verify --quiet "refs/remotes/origin/$ref"; then
    git -C "$source_dir" worktree add --quiet --track -b "$ref" "$worktree" "origin/$ref"
else
    git -C "$source_dir" worktree add --quiet --detach "$worktree" "$ref"
fi
echo "if a later step fails, remove the partial worktree with: git worktree remove $worktree"

step "copying untracked modules"
for module in "$source_dir"/modules/*/; do
    name=$(basename "$module")
    [ -e "$worktree/modules/$name" ] && continue
    [ -n "$(git -C "$source_dir" ls-files -- "modules/$name")" ] && continue
    revision=""
    [ -e "$module/.git" ] && revision=" @ $(git -C "$module" rev-parse --short HEAD 2>/dev/null || echo unknown)"
    echo "  $name$revision"
    mkdir -p "$worktree/modules"
    cp -a --reflink=auto -- "$source_dir/modules/$name" "$worktree/modules/$name"
done

step "copying $source_build"
cp -a --reflink=auto -- "$source_build" "$build"
cp -a "$build/.ninja_log" "$work/ninja_log.orig"

step "restamping unchanged sources to the source checkout's timestamps"
seed restamp "$source_dir" "$worktree" "$build_name"

step "rewriting $source_dir to $worktree in the build directory"
(cd "$build" && grep -rIl --exclude='*.o' --exclude='*.a' --exclude='*.gch' \
    --exclude='.ninja_deps' --exclude='.ninja_log' -- "$source_dir" . || true) > "$work/textfiles.txt"
seed relocate "$source_dir" "$worktree" "$build" "$work/textfiles.txt"

step "reconfiguring $build"
touch "$work/before-cmake"
cmake -S "$worktree" -B "$build" > "$work/cmake.log" 2>&1 || { cat "$work/cmake.log" >&2; die "cmake failed"; }
regenerated=$(find "$build" -newer "$work/before-cmake" -type f \
    \( -name '*.c' -o -name '*.cpp' -o -name '*.cxx' -o -name '*.h' -o -name '*.hpp' -o -name '*.hxx' \) \
    | sed "s|^$build/||")
[ -n "$regenerated" ] && echo "regenerated sources:" && echo "$regenerated" | sed 's/^/  /'

step "rehashing .ninja_log"
seed rehash "$source_dir" "$worktree" "$build" "$work/old_commands.json" "$work/ninja_log.orig"

if [ -n "$settings_from" ]; then
    step "writing $worktree/conf/verify-all.json"
    seed settings "$settings_from" "$source_build" "$build" "$worktree/conf/verify-all.json"
fi

step "dry run of $target in $build"
plan="$build/seed-worktree-plan.txt"
dry_run "$build" "$work/worktree-dry.txt" > "$plan"
cp "$work/worktree-dry.txt.explain" "$build/seed-worktree-explain.txt"
count=$(wc -l < "$plan")
sed 's/^/  /' "$plan"
echo
checked_out="$(git -C "$worktree" rev-parse --abbrev-ref HEAD) @ $(git -C "$worktree" rev-parse --short HEAD)"
echo "worktree:      $worktree ($checked_out)"
echo "compiles:      $count for $target (list: $plan, reasons: $build/seed-worktree-explain.txt)"
echo "build with:    ninja -C $build -j 2 $target"
