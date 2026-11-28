#!/bin/sh
# Pre-commit hook: refuses the commit while a merge of an Unreal asset made with uasset-merge.js
# isn't finished, that is while a project's Saved/JsonBlueprintSync/Merge folder holds a
# merged.txt. The .uasset in the working tree is still ours until the plugin applies merged.txt,
# so committing it would lose theirs' changes without notice. Deleting the asset's merge folder
# marks the merge as finished.
#
# Setup: call it from .git/hooks/pre-commit (not core.hooksPath, which would turn off the Git LFS
# hooks in .git/hooks):
#   #!/bin/sh
#   sh "<path from the repository root to>/Tools/pre-commit-merges.sh" || exit 1

found=$(git ls-files -- '*.uproject' | while IFS= read -r project; do
	merge="$(dirname "$project")/Saved/JsonBlueprintSync/Merge"
	if [ -d "$merge" ]; then
		find "$merge" -name merged.txt | sed "s|^$merge||; s|/merged.txt\$||; s|^|  |; s|\$|  ($merge)|"
	fi
done)

if [ -n "$found" ]; then
	echo "Commit refused: these Unreal assets have a merge that isn't applied yet:" >&2
	echo "$found" >&2
	echo "Apply merged.txt in the editor, then delete the asset's merge folder, and commit again." >&2
	exit 1
fi
exit 0
