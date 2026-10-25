#!/bin/sh
# Test-copy launcher with owner-file guard.
#   run.sh start <name> [VPADS] [COPY] [ENV]   snapshot owner files, install DLL, launch, print pid
#       COPY: obscure-test (default) or obscure-test2; ENV: extra settings "K=V;K=V"
#   run.sh stop <name>                         kill our game process, snapshot again, diff against the start snapshot
#   run.sh stopall <name...>                   stop several runs, one snapshot diff for the first
L=$(cd "$(dirname "$0")/.." && pwd)   # the repo checkout; test copies obscure-test, obscure-test2, obscure-test3 live in it (git-ignored)
R=$L/tests
case "$1" in
start)
	T=$L/${4:-obscure-test}
	sh $R/snapshot.sh > $R/$2-before.txt 2>&1
	cp $L/build/xinput1_3.dll $T/xinput1_3.dll
	: > $T/obscoop.log
	EXTRA=""
	OLDIFS=$IFS; IFS=';'
	for kv in $5; do
		[ -n "$kv" ] && EXTRA="$EXTRA \$env:${kv%%=*}='${kv#*=}';"
	done
	IFS=$OLDIFS
	powershell.exe -NoProfile -Command "\$env:SteamAppId='254460'; \$env:SteamGameId='254460'; \$env:OBSCOOP_VPADS='$3'; \$env:OBSCOOP_BACKGROUND='1'; \$env:OBSCOOP_HIDE_REAL_PADS='1'; $EXTRA (Start-Process -FilePath '$(cygpath -w $T/Obscure.exe)' -WorkingDirectory '$(cygpath -w $T)' -PassThru).Id" | tr -d '\r' > $R/$2.pid
	sleep 10
	echo "pid $(cat $R/$2.pid)"
	powershell.exe -NoProfile -Command "Get-Process Obscure -ErrorAction SilentlyContinue | ForEach-Object { \$_.Id.ToString() + ' ' + \$_.Path }"
	;;
stop | stopall)
	shift
	FIRST=$1
	for n in "$@"; do
		taskkill.exe //PID $(cat $R/$n.pid) //F
	done
	sleep 2
	powershell.exe -NoProfile -Command "Get-Process Obscure -ErrorAction SilentlyContinue | ForEach-Object { 'STILL RUNNING: ' + \$_.Id.ToString() + ' ' + \$_.Path }"
	sh $R/snapshot.sh > $R/$FIRST-after.txt 2>&1
	if diff $R/$FIRST-before.txt $R/$FIRST-after.txt > $R/$FIRST-diff.txt; then echo "OWNER FILES UNCHANGED"; else echo "OWNER FILES CHANGED:"; head -20 $R/$FIRST-diff.txt; fi
	;;
esac
