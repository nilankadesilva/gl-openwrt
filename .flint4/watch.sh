#!/bin/bash
# flint4 watch: what moved upstream since our last image? Runs in GitHub Actions (or locally). Needs: git, gh.
# Inputs: repo checked out at flint4-main with remote "upstream" = openwrt/openwrt fetched. Writes report.md.
set -u
cd "$(git rev-parse --show-toplevel)"
MAN=.flint4/manifest.txt
LASTTAG=$(git tag -l 'r[0-9]*' --sort=-v:refname | head -1); TAGDATE=$(git log -1 --format=%cI "$LASTTAG" 2>/dev/null || date -u -d '14 days ago' +%FT%TZ)
MB=$(git merge-base HEAD upstream/main); AHEAD=$(git rev-list --count $MB..upstream/main)
{
echo "## flint4 watch $(date -u +%F)"
echo
echo "last image tag: **$LASTTAG** ($(git rev-parse --short $LASTTAG), ${TAGDATE%T*}); flint4-main base: $(git rev-parse --short $MB); **upstream main ahead by $AHEAD commits**; kernel pin: $(git show upstream/main:target/linux/generic/kernel-6.18 | head -1 | sed 's/.*= *//') (ours: $(git show HEAD:target/linux/generic/kernel-6.18 | head -1 | sed 's/.*= *//'))"
echo
echo "### upstream main: commits since our base in areas we carry"
git log --format='- %h %ad %s' --date=short $MB..upstream/main -- target/linux/mediatek target/linux/generic/pending-6.18 target/linux/generic/backport-6.18 target/linux/generic/hack-6.18 target/linux/generic/config-6.18 target/linux/generic/kernel-6.18 package/kernel/mt76 package/kernel/mac80211 package/kernel/linux package/network/services/hostapd package/network/services/dnsmasq package/network/config/netifd package/network/config/wifi-scripts package/network/config/firewall4 package/utils/ucode package/utils/busybox package/network/services/bridger package/system/procd include/kernel-6.18 | head -60
echo
echo "### security-flagged subjects anywhere on main since our base"
git log --format='- %h %ad %s' --date=short -i --grep='CVE' --grep='security' --grep='vuln' $MB..upstream/main | head -20
echo
echo "### feeds: commits since $LASTTAG touching packages in our manifest"
for feed in packages luci routing; do
  d=/tmp/feed-$feed; [ -d $d ] || git clone -q --filter=blob:none --shallow-since="$TAGDATE" https://github.com/openwrt/$feed.git $d 2>/dev/null || continue
  names=$(cut -d' ' -f1 $MAN | sed -E 's/^(lib|kmod-)//; s/-[0-9]+$//' | sort -u)
  git -C $d log --format='%h %ad %s' --date=short --since="$TAGDATE" 2>/dev/null | while read h dt s; do pkg=${s%%:*}; echo "$names" | grep -qxF "$pkg" && echo "- [$feed] $h $dt $s"; done
done | head -40
echo
echo "### our stack above upstream base ($(git rev-list --count $MB..HEAD) commits)"
git log --format='- %h %s' $MB..HEAD
} > report.md
echo "report lines: $(wc -l < report.md)"
