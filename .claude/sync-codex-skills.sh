#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
claude_skills="$repo_root/.claude/skills"
codex_home="${CODEX_HOME:-$HOME/.codex}"
codex_skills="$codex_home/skills"

mkdir -p "$codex_skills"

link_skill() {
	local source_dir="$1"
	local skill_name
	skill_name="$(basename "$source_dir")"
	local target="$codex_skills/$skill_name"

	if [[ ! -f "$source_dir/SKILL.md" ]]; then
		return
	fi

	if [[ -e "$target" && ! -L "$target" ]]; then
		echo "skip $skill_name: $target exists and is not a symlink" >&2
		return
	fi

	ln -sfn "$source_dir" "$target"
	echo "linked $skill_name -> $source_dir"
}

for source_dir in "$claude_skills"/*; do
	[[ -d "$source_dir" ]] || continue
	[[ "$(basename "$source_dir")" == "shared" ]] && continue
	link_skill "$source_dir"
done

if [[ -d "$claude_skills/shared" ]]; then
	for source_dir in "$claude_skills/shared"/*; do
		[[ -d "$source_dir" ]] || continue
		link_skill "$source_dir"
	done
fi
