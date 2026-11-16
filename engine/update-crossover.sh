#!/bin/bash
# Moves the engine to CodeWeavers' newest CrossOver source release when it's
# newer than the one UPSTREAM.txt pins. engine/patches are rebased onto it with
# git's three-way merge in a throwaway repository; when they all still apply,
# they're written back regenerated, and the pin moves.
#
#   engine/update-crossover.sh
#
# Exit status: 0 when it moved to a newer release; 3 when the pinned release
# is the newest; 2 when a newer release exists but a patch no longer applies
# (the pin moves anyway and the patches stay as they were, so the conflict is
# there to resolve). A summary for the pull request goes to stdout.
#
# WHEELIO_ENGINE_WORK  downloads and the throwaway repository (default: build/engine)
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/.." && pwd)
work=${WHEELIO_ENGINE_WORK:-$repo/build/engine}
cache=$work/cache
mkdir -p "$cache"

fail() {
    echo "update-crossover: $*" >&2
    exit 1
}

# shellcheck source=engine/inputs.sh
source "$here/inputs.sh"

current_digest=$(upstream crossover-sources 2)
current_url=$(upstream crossover-sources 3)
current=${current_url##*crossover-sources-}
current=${current%.tar.gz}
release_url() {
    echo "${current_url%crossover-sources-*}crossover-sources-$1.tar.gz"
}

# CodeWeavers' folder isn't listed, so ask for the next likely names: the next
# major, minor and patch release, newest first, until none is there.
exists() {
    curl -fsIL --retry 2 -o /dev/null "$(release_url "$1")"
}
newest=$current
while :; do
    IFS=. read -r major minor patch <<<"$newest"
    next=
    for candidate in "$((major + 1)).0.0" "$major.$((minor + 1)).0" "$major.$minor.$((patch + 1))"; do
        if exists "$candidate"; then
            next=$candidate
            break
        fi
    done
    [ -n "$next" ] || break
    newest=$next
done
if [ "$newest" = "$current" ]; then
    echo "CrossOver $current is CodeWeavers' newest source release."
    exit 3
fi

old_tar=$(fetch crossover-sources)
new_url=$(release_url "$newest")
new_tar=$cache/crossover-sources-$newest.tar.gz
if [ ! -f "$new_tar" ]; then
    curl -fsSL --retry 3 -o "$new_tar.part" "$new_url" || fail "could not download $new_url"
    mv "$new_tar.part" "$new_tar"
fi
new_digest=$(sha256 "$new_tar")
fetch_proton

# An arm64 engine needs CodeWeavers' FEX (x86 emulation for arm64 Wine). Their
# source releases list each component under sources/; flag the first with FEX.
fex=$(tar -tzf "$new_tar" | awk -F/ 'NF > 2 { print $2 }' | sort -u | grep -i fex | paste -sd ' ' - || true)
note=
if [ -n "$fex" ]; then
    note="**This release includes CodeWeavers' FEX** (sources/$fex), which an arm64 engine needs."
fi

# The tree build.sh patches: CrossOver's Wine with Proton's lsteamclient and
# steam helper copied in.
populate() {
    tar -xzf "$1" -C "$tree" --strip-components 2 sources/wine
    rsync -r "$cache/proton/lsteamclient/" "$tree/dlls/lsteamclient/"
    rsync -r "$cache/proton/steam_helper/" "$tree/programs/steam.exe/"
}

tree=$work/update/wine
rm -rf "$tree"
mkdir -p "$tree"
# The trees as they are: no line-ending conversion, whatever git's own settings.
git() {
    command git -C "$tree" -c user.name="Wheelio engine update" -c user.email="engine-update@localhost" \
        -c commit.gpgsign=false -c core.autocrlf=false -c core.safecrlf=false -c apply.whitespace=nowarn "$@"
}
git init -q -b old
populate "$old_tar"
git add -A
git commit -q -m "CrossOver $current"
git tag base
git am -q "$here"/patches/*.patch || fail "engine/patches don't apply to CrossOver $current"
git checkout -q --orphan new
git rm -rfq .
populate "$new_tar"
git add -A
git commit -q -m "CrossOver $newest"

status=0
if git rebase -q --onto new base old >/dev/null 2>&1; then
    rm -f "$here"/patches/*.patch
    git format-patch -q --zero-commit --no-signature --no-numbered -o "$here/patches" new..old
    result="The patches rebased cleanly and have been regenerated."
else
    failing=$(git log -1 --format=%s REBASE_HEAD)
    files=$(git diff --name-only --diff-filter=U)
    hunks=$(git diff | head -n 200)
    git rebase --abort
    status=2
    result=$(printf '%s\n\n%s\n\n%s\n%s\n\n%s\n%s\n%s\n' \
        "A patch no longer applies, so the patches are as they were and this needs finishing by hand." \
        "Patch: $failing" \
        "Conflicting files:" "$(printf '%s\n' "$files" | sed 's/^/- /')" \
        '```diff' "$hunks" '```')
fi
rm -rf "$work/update"

# The new pin, and a fresh engine revision for the new release.
sed -i.bak "s|^crossover-sources  $current_digest  $current_url$|crossover-sources  $new_digest  $new_url|" \
    "$here/UPSTREAM.txt"
sed -i.bak 's/^ENGINE_REVISION=.*/ENGINE_REVISION=1/' "$here/build.sh"
rm -f "$here/UPSTREAM.txt.bak" "$here/build.sh.bak"
grep -q "$new_digest" "$here/UPSTREAM.txt" || fail "couldn't update UPSTREAM.txt"

printf 'Moves the engine from CrossOver %s to %s ([source](%s)).\n\n' "$current" "$newest" "$new_url"
[ -z "$note" ] || printf '%s\n\n' "$note"
printf '%s\n' "$result"
exit $status
