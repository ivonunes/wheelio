# shellcheck shell=bash disable=SC2154  # here, cache and fail come from the sourcing script
# The engine's pinned inputs (UPSTREAM.txt), shared by build.sh and
# update-crossover.sh. Sourced; needs $here (this folder), $cache (where
# downloads are kept) and a fail function.

upstream() {
    awk -v name="$1" -v field="$2" '$1 == name { print $field }' "$here/UPSTREAM.txt"
}

sha256() {
    shasum -a 256 "$1" | cut -d' ' -f1
}

# Downloads an UPSTREAM.txt input into the cache once, verified; prints its path.
fetch() {
    local name=$1 digest url file
    digest=$(upstream "$name" 2)
    url=$(upstream "$name" 3)
    [ -n "$url" ] || fail "$name is not in UPSTREAM.txt"
    file=$cache/$digest-$(basename "$url")
    if [ ! -f "$file" ]; then
        curl -fsSL --retry 3 -o "$file.part" "$url" || fail "could not download $url"
        [ "$(sha256 "$file.part")" = "$digest" ] || { rm -f "$file.part"; fail "$url does not match its SHA-256 in UPSTREAM.txt"; }
        mv "$file.part" "$file"
    fi
    echo "$file"
}

# Checks out a git input at its pinned commit into the cache once, limited to
# the given paths if any; prints its folder.
fetch_git() {
    local name=$1 commit url dir
    shift
    commit=$(upstream "$name" 2)
    url=$(upstream "$name" 3)
    [ -n "$url" ] || fail "$name is not in UPSTREAM.txt"
    dir=$cache/$name
    if [ "$(git -C "$dir" rev-parse HEAD 2>/dev/null)" != "$commit" ]; then
        rm -rf "$dir"
        git init -q "$dir"
        git -C "$dir" remote add origin "$url"
        [ $# -eq 0 ] || git -C "$dir" sparse-checkout set --no-cone "$@"
        git -C "$dir" fetch -q --depth 1 --filter=blob:none origin "$commit" || fail "could not fetch $url"
        git -C "$dir" -c advice.detachedHead=false checkout -q FETCH_HEAD
        [ "$(git -C "$dir" rev-parse HEAD)" = "$commit" ] || fail "$name is not at $commit"
    fi
    echo "$dir"
}

# The parts of Proton the engine uses, at the pinned commit.
fetch_proton() {
    fetch_git proton /lsteamclient/ /steam_helper/ /LICENSE >/dev/null
}
