# suite.sh [test ...]   (no arguments = every test)
# Runs each <name>_test.py on a fresh host+guest pair (obscure-test3 host, obscure-test2 guest) with the
# DLL in build/, prints one line per test, and the failing checks only for failures. Refuses to
# start if any Obscure is running (the owner's game). Owner-file snapshot is checked once at the end.
L=$(cd "$(dirname "$0")/.." && pwd)
R=$L/tests
TESTS=${*:-"give bag door p2door ui view msg stall death camera load"}
if powershell.exe -NoProfile -Command "if (Get-Process Obscure -ErrorAction SilentlyContinue) { exit 1 }"; then :; else
	echo "Obscure is running (the owner's game?); not starting test copies"; exit 1
fi
cd $L
sh $R/snapshot.sh > $R/suite-before.txt 2>&1
pass=0; fail=0
for t in $TESTS; do
	rm -f obscure-test2/coop_profile/remote/game0.sav
	sh $R/run.sh start s_${t}h 1 obscure-test3 "OBSCOOP_ROLE=host;OBSCOOP_MULTI=1" > /dev/null
	sh $R/run.sh start s_${t}g 0 obscure-test2 "OBSCOOP_ROLE=guest;OBSCOOP_MULTI=1" > /dev/null
	out=$(cd $R && timeout 400 python -I -u ${t}_test.py s_${t}h s_${t}g 2>&1)
	res=$(echo "$out" | grep "^RESULT:" | tail -1)
	if echo "$res" | grep -q "RESULT: PASS"; then
		echo "$t: ${res#RESULT: }"; pass=$((pass + 1))
	else
		echo "$t: ${res:-no result (setup or script error)}"; fail=$((fail + 1))
		echo "$out" | grep "^FAIL\|Error" | head -5 | sed 's/^/    /'
	fi
	sh $R/run.sh stopall s_${t}h s_${t}g > /dev/null 2>&1
done
sh $R/snapshot.sh > $R/suite-after.txt 2>&1
diff -q $R/suite-before.txt $R/suite-after.txt > /dev/null && owner="owner files unchanged" || owner="OWNER FILES CHANGED, see tests/suite-*.txt"
echo "suite: $pass passed, $fail failed; $owner"
