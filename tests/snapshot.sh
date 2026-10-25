#!/bin/sh
# Prints a fingerprint of every owner location the test copy could touch. Diff before/after runs.
R="${OBSCURE_INSTALL:-/c/Program Files (x86)/Steam/steamapps/common/Obscure}"   # your real install (never touched by the tests)
U="${STEAM_USERDATA:?set STEAM_USERDATA to Steam/userdata/<your account id>}"
echo "## real install (size, mtime, path)"; find "$R" -type f -printf '%s %T@ %P\n' | sort -k3
echo "## real install small-file hashes"; find "$R" -type f -size -2000k -exec sha256sum {} \; | sed "s|$R/||" | sort -k2
echo "## Documents/MRS"; ls -laR "$USERPROFILE/Documents/MRS" 2>&1
echo "## userdata 254460 (content hashes; steam.exe rewrites remotecache.vdf with identical content on every launch and exit)"; (cd "$U/254460" 2>/dev/null && find . -type f -exec sha256sum {} \; | sort -k2) || echo "missing"
