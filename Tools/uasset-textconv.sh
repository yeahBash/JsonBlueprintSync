#!/bin/sh
# Git textconv for .uasset and .umap files: prints the UnrealAssetJson merge text of the file, so
# git diff and git log -p show the values stored in the asset. TortoiseGit's diff doesn't use
# textconv; uasset-diff.js covers it.
#
# Setup (local git config, nothing machine-specific is committed):
#   git config unrealassetjson.path "D:/Path/To/UnrealAssetJson.exe"
#   git config diff.unreal.textconv "sh <path to this script>"
#   git config diff.unreal.cachetextconv true
# and in .gitattributes: *.uasset diff=unreal (after filter=lfs, in place of diff=lfs).
#
# Git passes the file in its stored form. A Git LFS pointer is turned into the real file with
# git lfs smudge first. If the reader can't read the file, the output names the size and SHA-256
# of the file instead, so a change still shows.
#
# Usage: uasset-textconv.sh <file> [<output file>], run inside the repository. Without an output
# file the text goes to stdout, as git expects. uasset-diff.js uses the output file.

file=$1
if [ -n "$2" ]; then
	exec > "$2"
fi
reader=$(git config --get unrealassetjson.path)
if [ -z "$reader" ]; then
	echo "uasset-textconv: set the reader path with: git config unrealassetjson.path <UnrealAssetJson.exe>" >&2
	exit 1
fi

tmp=$(mktemp -d) || exit 1
trap 'rm -rf "$tmp"' EXIT

# Git passes a working copy file by its path relative to the repository root; the reader needs it
# absolute.
input="$(cd "$(dirname "$file")" && pwd)/$(basename "$file")"
if head -c 200 "$file" | grep -q '^version https://git-lfs.github.com/spec/v1'; then
	input="$tmp/$(basename "$file")"
	if ! git lfs smudge < "$file" > "$input" 2> "$tmp/lfs.log"; then
		echo "# Git LFS object not available here"
		cat "$file"
		exit 0
	fi
fi

winpath() {
	if command -v cygpath > /dev/null 2>&1; then cygpath -w "$1"; else printf '%s\n' "$1"; fi
}

if "$reader" -merge -s="$(winpath "$tmp/out.txt")" -f "$(winpath "$input")" > "$tmp/reader.log" 2>&1 && [ -f "$tmp/out.txt" ]; then
	cat "$tmp/out.txt"
else
	echo "# UnrealAssetJson could not read this file"
	echo "# Size: $(wc -c < "$input" | tr -d ' ')"
	echo "# SHA-256: $(sha256sum < "$input" | cut -d ' ' -f 1)"
	echo "uasset-textconv: UnrealAssetJson could not read $file; showing its size and hash" >&2
fi
