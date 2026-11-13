#!/usr/bin/env bash
# check.sh OUTPUT LIST: checks the C++ test programs' output (run.sh on the
# emulator, board.sh on the board).  OUTPUT holds "@@BEGIN name" /
# "@@END name exit=N|signal=N" sections (libffi's ffirun format), LIST the
# lines "name path [args]".  FAILS (environment) adds earlier failures.
# Exit 0 only if every check passes.
set -uo pipefail
O=$1
LIST=$2
FAILS=${FAILS:-0}
pass() { echo "PASS $*"; }
fail() { echo "FAIL $*"; FAILS=$((FAILS + 1)); }
grep -q '^CT-ALL-DONE' "$O" && pass "complete" || fail "complete: the command list did not finish"
section() { sed -n "/^@@BEGIN $1\$/,/^@@END $1 /p" "$O"; }
while read -r name _; do
	out=$(section "$name")
	end=$(grep "^@@END $name " <<<"$out" || echo "no @@END line")
	case $name in
	*-noexcept)
		grep -q 'exit=77' <<<"$end" && grep -q '^TERMINATE-HANDLER' <<<"$out" &&
			pass "$name" || fail "$name: $end" ;;
	*-uncaught)
		grep -q 'signal=6' <<<"$end" &&
			grep -q "terminating due to uncaught exception of type std::runtime_error: escaped" <<<"$out" &&
			pass "$name" || fail "$name: $end $(grep -i terminat <<<"$out" | head -2)" ;;
	*)
		bad=$(grep '^FAIL ' <<<"$out" || true)
		if grep -q 'exit=0$' <<<"$end" && grep -q '^DONE [a-z]* failures=0' <<<"$out"; then
			pass "$name ($(grep -c '^PASS ' <<<"$out") checks)"
		else
			fail "$name: $end ${bad//$'\n'/; }"
		fi ;;
	esac
done <"$LIST"
grep -a 'INFO backtrace' "$O" | sort | uniq -c | sed 's/^/INFO /' || true
echo "RESULT: $FAILS failing checks"
[ "$FAILS" = 0 ]
