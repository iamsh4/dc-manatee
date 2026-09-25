#!/bin/sh
# Rebuild site/ and publish it to the gh-pages branch (served at https://iamsh4.github.io/dc-manatee/).
# The branch contains only the built site: *.html, assets/ and .nojekyll.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
python3 tools/build_site.py
SRC_REV=$(git rev-parse --short HEAD)
WT=$(mktemp -d)
trap 'git worktree remove --force "$WT" >/dev/null 2>&1 || true' EXIT
if git ls-remote --exit-code --heads origin gh-pages >/dev/null 2>&1; then
  git fetch -q origin gh-pages
  git worktree add -q "$WT" -B gh-pages origin/gh-pages
else
  git worktree add -q --detach "$WT"
  (cd "$WT" && git checkout -q --orphan gh-pages && git rm -rq --cached . && git clean -fdxq)
fi
(cd "$WT" && find . -mindepth 1 -maxdepth 1 ! -name .git -exec rm -rf {} +)
cp site/*.html "$WT"/
cp -R site/assets "$WT"/
touch "$WT/.nojekyll"
cd "$WT"
git add -A
if git diff --cached --quiet; then
  echo "gh-pages already up to date"
else
  git commit -q -m "Publish site from main@$SRC_REV"
  git push -q origin gh-pages
  echo "published site from main@$SRC_REV to gh-pages"
fi
