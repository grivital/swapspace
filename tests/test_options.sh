#!/bin/sh
# Table-driven tests for every entry of the options[] table in src/opts.c.
#
# Nothing here needs root: `swapspace -i` validates the configuration and
# exits, so each case is one invocation checked for its exit status and
# message.  Every case starts from a configuration that passes `-i` and
# changes one thing.
#
# A row of a table reads
#
#     modes|tokens|expect
#
# modes   which ways of passing the settings to run the row through:
#           c  configuration file       "freetarget = 50"
#           l  long option with '='     --freetarget=50
#           s  long option, next arg    --freetarget 50
#           S  short option, next arg   -f 50
#           A  short option, attached   -f50
#         '*' means all five.
# tokens  space-separated settings, "name=value" or a bare "name".  They
#         are applied on top of the base configuration, so a setting that
#         repeats one of the base's overrides it.
# expect  "ok" (exit status 0), "+text" or "!text" (output must or must not
#         contain text, whatever the exit status), or any other text: exit
#         status 1 with that text in the output.
#
# To cover a new option add it to OPTIONS below and give it boundary rows.

BIN="${BIN:-../src/swapspace}"
SRCDIR="${srcdir:-.}"

fail() { echo "FAIL: $*" >&2; exit 1; }
skip() { echo "SKIP: $*" >&2; exit 77; }

test -x "$BIN" || skip "swapspace binary not built (run 'make' first)"

PS=$(getconf PAGESIZE 2>/dev/null) || skip "getconf PAGESIZE is unavailable"
# getconf has no LONG_MAX on glibc, but LONG_BIT tells whether long is 64 bits.
if [ "$(getconf LONG_BIT 2>/dev/null)" = 64 ]; then
  LONG_MAX=9223372036854775807
else
  LONG_MAX=0
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/swapspace-opts.XXXXXX") || exit 1
DPIDS=
trap 'test -n "$DPIDS" && kill $DPIDS 2>/dev/null; rm -rf "$TMP"' EXIT HUP INT TERM

set -f
OUT="$TMP/out"
mkdir -m 700 "$TMP/swap" "$TMP/sp ace" || exit 1

# Thresholds chosen so that minimal_swapfile() is (almost) zero, so the result
# does not depend on the space the build filesystem happens to have.
cat >"$TMP/base.conf" <<EOF
swappath = "$TMP/swap"
lower_freelimit = 50
freetarget = 50
upper_freelimit = 60
psi_threshold = 5
EOF

FAILS=0
CASES=0
bad() { FAILS=$((FAILS+1)); echo "FAIL: $*" >&2; }

# name, short letter, kind (num, str or flag)
OPTIONS="
buffer_elasticity B num
cache_elasticity C num
configfile c str
cooldown a num
daemon d flag
erase e flag
freetarget f num
help h flag
inspect i flag
lower_freelimit l num
max_swapsize M num
min_swapsize m num
paranoid P flag
pidfile p str
psi_threshold x num
quiet q flag
swappath s str
upper_freelimit u num
verbose v flag
version V flag
"

# Make the table's letters available as $L_<name>.
while read -r name letter kind; do
  test -n "$name" && eval "L_$name=$letter"
done <<EOF
$OPTIONS
EOF

# Every option of the daemon's own table must be in OPTIONS above.
sed -n 's/^  { "\([a-z_]*\)",.*/\1/p' "$SRCDIR/../src/opts.c" | sort >"$TMP/src.names"
echo "$OPTIONS" | awk 'NF {print $1}' | sort >"$TMP/test.names"
test -s "$TMP/src.names" || skip "cannot read the options table from src/opts.c"
if ! cmp -s "$TMP/src.names" "$TMP/test.names"; then
  bad "OPTIONS does not match src/opts.c: $(diff "$TMP/src.names" "$TMP/test.names" | tr '\n' ' ')"
fi

# Run the daemon with the arguments given; output and status end up in $OUT
# and $RC.
run() {
  "$BIN" "$@" >"$OUT" 2>&1 </dev/null
  RC=$?
}

# check LABEL EXPECT: compare the last run() against an expectation.
check() {
  CASES=$((CASES+1))
  case $2 in
  ok)
    [ "$RC" -eq 0 ] || bad "$1: expected success, got status $RC: $(head -n 1 "$OUT")"
    ;;
  '+'*)
    grep -F -q -- "${2#+}" "$OUT" || bad "$1: output lacks '${2#+}': $(head -n 1 "$OUT")"
    ;;
  '!'*)
    if grep -F -q -- "${2#!}" "$OUT"; then
      bad "$1: output unexpectedly contains '${2#!}'"
    fi
    ;;
  *)
    if [ "$RC" -ne 1 ]; then
      bad "$1: expected status 1 ('$2'), got $RC"
    elif ! grep -F -q -- "$2" "$OUT"; then
      bad "$1: expected '$2', got: $(head -n 1 "$OUT")"
    fi
    ;;
  esac
}

# run_mode MODE TOKENS: run `swapspace -i` with the settings passed in MODE.
run_mode() {
  mode=$1
  tokens=$2
  cp "$TMP/base.conf" "$TMP/case.conf"
  set --
  for tok in $tokens; do
    case $tok in
    *=*) key=${tok%%=*}; val=${tok#*=}; hasval=1 ;;
    *)   key=$tok; val=; hasval=0 ;;
    esac
    eval "letter=\${L_$key:-}"
    case $mode in
    c)
      if [ $hasval = 1 ]; then
        printf '%s = %s\n' "$key" "$val"
      else
        printf '%s\n' "$key"
      fi >>"$TMP/case.conf"
      ;;
    l) if [ $hasval = 1 ]; then set -- "$@" "--$key=$val"; else set -- "$@" "--$key"; fi ;;
    s) set -- "$@" "--$key"; [ $hasval = 1 ] && set -- "$@" "$val" ;;
    S) set -- "$@" "-$letter"; [ $hasval = 1 ] && set -- "$@" "$val" ;;
    A) set -- "$@" "-$letter$val" ;;
    esac
  done
  run "$@" -i -c "$TMP/case.conf"
}

# rows < FILE: run every row of FILE through its modes.
rows() {
  while IFS='|' read -r modes tokens expect; do
    case $modes in ''|'#'*) continue ;; esac
    test "$modes" = '*' && modes=clsSA
    n=${#modes}
    while [ "$n" -gt 0 ]; do
      m=$(printf '%s' "$modes" | cut -c "$n")
      n=$((n-1))
      run_mode "$m" "$tokens"
      check "[$m] $tokens" "$expect"
    done
  done
}

# Sanity check of the harness itself: the base configuration passes.
run -i -c "$TMP/base.conf"
check "base configuration" ok

# Rows every option of a kind has to satisfy, generated from OPTIONS.
GEN="$TMP/generated.rows"
: >"$GEN"
while read -r name letter kind; do
  case $kind in
  num)
    cat >>"$GEN" <<EOF
*|$name|requires
c|$name=|syntax error
lsSA|$name=|requires
*|$name=abc|invalid unit letter
*|$name=1x|invalid unit letter
*|$name=1.5|invalid unit letter
*|$name=1kb|invalid numeric argument
*|$name=5k5|invalid numeric argument
*|$name=99999999999999999999|out of range
*|$name=-99999999999999999999|out of range
*|$name=9223372036854775807k|out of range
*|$name=-9223372036854775807k|out of range
EOF
    ;;
  flag)
    cat >>"$GEN" <<EOF
*|$name|ok
*|$name=1|does not take an argument
c|$name=|syntax error
lsS|$name=|does not take an argument
EOF
    ;;
  esac
done <<EOF
$OPTIONS
EOF
rows <"$GEN"

# Numeric options.  Rows that need other settings to pass the cross-checks
# carry them, so a failure always points at the option under test.

# Percentages: the parser accepts 0..100 and a value above is "too large".
# 0x64 and 0144 are 100 in hexadecimal and octal, one more is too large.
rows <<EOF
*|buffer_elasticity=0|ok
*|buffer_elasticity=50|ok
*|buffer_elasticity=100|ok
*|buffer_elasticity=+100|ok
*|buffer_elasticity=-1|may not be negative
*|buffer_elasticity=101|too large
*|buffer_elasticity=1k|too large
*|buffer_elasticity=0k|ok
*|buffer_elasticity=0x64|ok
*|buffer_elasticity=0x65|too large
*|buffer_elasticity=0144|ok
*|buffer_elasticity=0145|too large
*|buffer_elasticity=08|invalid unit letter
*|cache_elasticity=0|ok
*|cache_elasticity=100|ok
*|cache_elasticity=-1|may not be negative
*|cache_elasticity=101|too large
*|cache_elasticity=1m|too large
*|psi_threshold=0|ok
*|psi_threshold=100|ok
*|psi_threshold=-1|may not be negative
*|psi_threshold=101|too large
*|psi_threshold=1g|too large
*|freetarget=2 lower_freelimit=2|ok
*|freetarget=99 lower_freelimit=99 upper_freelimit=100|ok
*|freetarget=1 lower_freelimit=0|too small
*|freetarget=0|too small
*|freetarget=-1|too small
*|freetarget=100|too large
*|freetarget=1t|too large
*|lower_freelimit=0 freetarget=2|ok
*|lower_freelimit=99 freetarget=99 upper_freelimit=100|ok
*|lower_freelimit=-1|may not be negative
*|lower_freelimit=100|too large
*|lower_freelimit=1k|too large
*|upper_freelimit=100 lower_freelimit=60 freetarget=60|ok
*|upper_freelimit=2 lower_freelimit=2 freetarget=2|ok
*|upper_freelimit=-1|may not be negative
*|upper_freelimit=101|too large
*|upper_freelimit=1g|too large
*|upper_freelimit=1 lower_freelimit=0|freetarget > upper_freelimit
*|upper_freelimit=0 lower_freelimit=0|freetarget > upper_freelimit
EOF

# Seconds: any non-negative value, suffixes multiply.
rows <<EOF
*|cooldown=0|ok
*|cooldown=1|ok
*|cooldown=600|ok
*|cooldown=-1|may not be negative
*|cooldown=1k|ok
*|cooldown=1m|ok
EOF
if [ "$LONG_MAX" = 9223372036854775807 ]; then
  rows <<EOF
*|cooldown=9223372036854775807|ok
*|cooldown=9223372036854775808|out of range
*|cooldown=8388607t|ok
*|cooldown=8388608t|out of range
*|cooldown=9007199254740992k|out of range
EOF
fi

# Sizes in bytes.  The parser only requires 8192, but swaps_check_config()
# also wants min_swapsize >= 10 pages and min_swapsize <= max_swapsize, and
# both values are truncated to whole pages.  The messages tell which of the
# two layers rejected a value.
rows <<EOF
*|min_swapsize=$((10*PS))|ok
*|min_swapsize=$((10*PS+1))|ok
*|min_swapsize=$((10*PS-1))|min_swapsize < 10*getpagesize()
*|min_swapsize=$((9*PS))|min_swapsize < 10*getpagesize()
*|min_swapsize=8192|min_swapsize < 10*getpagesize()
*|min_swapsize=8191|too small
*|min_swapsize=0|too small
*|min_swapsize=1|too small
*|min_swapsize=-1|too small
*|min_swapsize=4m|ok
*|min_swapsize=4194304|ok
*|min_swapsize=0x400000|ok
*|min_swapsize=1g|ok
*|min_swapsize=2t|ok
*|min_swapsize=3t|min_swapsize > max_swapsize
*|min_swapsize=8192t|min_swapsize > max_swapsize
*|min_swapsize=8388607t|min_swapsize > max_swapsize
*|min_swapsize=8388607t max_swapsize=8388607t|ok
*|min_swapsize=8388608t|out of range
*|min_swapsize=9223372036854775807 max_swapsize=9223372036854775807|ok
*|min_swapsize=9223372036854775808|out of range
*|min_swapsize=7k|too small
*|min_swapsize=8k|min_swapsize < 10*getpagesize()
*|max_swapsize=8192|min_swapsize > max_swapsize
*|max_swapsize=8191|too small
*|max_swapsize=0|too small
*|max_swapsize=-1|too small
*|max_swapsize=7k|too small
*|max_swapsize=8k|min_swapsize > max_swapsize
*|max_swapsize=1m min_swapsize=1m|ok
*|max_swapsize=4m|ok
*|max_swapsize=4m min_swapsize=4m|ok
*|max_swapsize=$((4*1024*1024-PS)) min_swapsize=4m|min_swapsize > max_swapsize
*|max_swapsize=$((4*1024*1024+PS-1)) min_swapsize=4m|ok
*|max_swapsize=$((4*1024*1024+PS-1)) min_swapsize=$((4*1024*1024+PS))|min_swapsize > max_swapsize
*|max_swapsize=1t|ok
*|max_swapsize=2t|ok
*|max_swapsize=9223372036854775807|ok
*|max_swapsize=8388607t|ok
*|max_swapsize=8388608t|out of range
*|max_swapsize=9223372036854775808|out of range
*|max_swapsize=$((10*PS)) min_swapsize=$((10*PS))|ok
*|max_swapsize=$((10*PS-1)) min_swapsize=$((10*PS))|min_swapsize > max_swapsize
*|min_swapsize=$((10*PS+1)) max_swapsize=$((10*PS))|ok
*|min_swapsize=$((11*PS-1)) max_swapsize=$((10*PS))|ok
EOF

# Size suffixes are k, m, g and t (either case), each 1024 times the one
# before.  Pairs of settings that are equal only if the suffix is exact.
rows <<EOF
*|min_swapsize=1m max_swapsize=1024k|ok
*|min_swapsize=1m max_swapsize=1023k|min_swapsize > max_swapsize
*|min_swapsize=1M max_swapsize=1024K|ok
*|min_swapsize=1M max_swapsize=1023K|min_swapsize > max_swapsize
*|min_swapsize=1g max_swapsize=1024m|ok
*|min_swapsize=1g max_swapsize=1023m|min_swapsize > max_swapsize
*|min_swapsize=1G max_swapsize=1024M|ok
*|min_swapsize=1G max_swapsize=1023M|min_swapsize > max_swapsize
*|min_swapsize=1t max_swapsize=1024g|ok
*|min_swapsize=1t max_swapsize=1023g|min_swapsize > max_swapsize
*|min_swapsize=1T max_swapsize=1024G|ok
*|min_swapsize=1T max_swapsize=1023G|min_swapsize > max_swapsize
*|min_swapsize=1024k max_swapsize=1048575|min_swapsize > max_swapsize
*|min_swapsize=1048576 max_swapsize=1k|too small
EOF

# A string of N characters starting with a slash, with a slash every 200
# characters so that no path component is too long.
longstr() {
  awk -v n="$1" 'BEGIN { s = "/"; while (length(s) < n) s = s (length(s) % 200 == 199 ? "/" : "a"); print s }'
}
P4095=$(longstr 4095)
P4096=$(longstr 4096)
P4097=$(longstr 4097)

# String options.  The limit is PATH_MAX including the terminating NUL, so
# 4095 characters fit and 4096 do not; in the configuration file a longer
# token is no longer one word, which is a syntax error.  swappath and
# configfile need a value, pidfile does not.
rows <<EOF
*|swappath|requires an argument
*|swappath=$TMP/swap|ok
*|swappath=relative/dir|not absolute
*|swappath=.|not absolute
*|swappath=~/swap|not absolute
c|swappath=|syntax error
lsS|swappath=|not absolute
*|swappath=$P4095|!string too long
lsSA|swappath=$P4096|string too long
c|swappath=$P4096|string too long
lsSA|swappath=$P4097|string too long
c|swappath=$P4097|syntax error
*|swappath=relative swappath=$TMP/swap|ok
*|swappath=$TMP/swap swappath=relative|not absolute
*|pidfile|ok
*|pidfile=$TMP/pid|ok
*|pidfile=relative|ok
*|pidfile=$P4095|ok
lsSA|pidfile=$P4096|string too long
c|pidfile=$P4096|string too long
lsSA|pidfile=$P4097|string too long
c|pidfile=$P4097|syntax error
*|configfile|requires an argument
*|configfile=$TMP/base.conf|ok
*|configfile=$P4095|ok
lsSA|configfile=$P4096|string too long
c|configfile=$P4096|string too long
lsSA|configfile=$P4097|string too long
c|configfile=$P4097|syntax error
EOF

# Flags: the generated rows above cover the argument checks.  The two that
# end the program early still succeed in every mode.
rows <<EOF
*|quiet verbose|quiet & verbose
*|quiet quiet|ok
*|verbose verbose|ok
*|daemon paranoid quiet|ok
*|daemon erase inspect|ok
*|help|ok
*|version|ok
EOF

# Keywords that are not options, and repeated settings: the last one wins,
# but every one must be valid on its own.
rows <<EOF
cls|not_an_option=1|unknown configuration item
cls|not_an_option|unknown configuration item
cls|free=5|unknown configuration item
cls|freetarget_x=5|unknown configuration item
cls|freetarget_|unknown configuration item
c|Freetarget=50|syntax error
ls|Freetarget=50|unknown configuration item
c|max-swapsize=1m|syntax error
ls|max-swapsize=1m|unknown configuration item
*|freetarget=10 freetarget=55|ok
*|freetarget=55 freetarget=10|freetarget < lower_freelimit
*|freetarget=200 freetarget=55|too large
*|freetarget=55 freetarget=200|too large
*|max_swapsize=1m min_swapsize=1m max_swapsize=2t|ok
*|max_swapsize=2t min_swapsize=1m max_swapsize=1m|ok
*|max_swapsize=2t min_swapsize=1m max_swapsize=1k|too small
*|lower_freelimit=70 lower_freelimit=40|ok
*|lower_freelimit=40 lower_freelimit=70|lower_freelimit > upper_freelimit
EOF

# Syntax of the configuration file: label|file contents|expect.  The contents
# are appended to the base configuration and go through printf %b.
K100=$(awk 'BEGIN { for (i = 0; i < 100; i++) printf "a" }')
K101=${K100}a
conf_rows() {
  while IFS='|' read -r label content expect; do
    case $label in ''|'#'*) continue ;; esac
    cp "$TMP/base.conf" "$TMP/case.conf"
    printf '%b\n' "$content" >>"$TMP/case.conf"
    run -i -c "$TMP/case.conf"
    check "conf: $label" "$expect"
  done
}
conf_rows <<EOF
blanks around everything|   freetarget   =   50   |ok
tabs|\tfreetarget\t=\t50\t|ok
no blanks|freetarget=50|ok
blank after the equals sign only|freetarget= 50|ok
blank before the equals sign only|freetarget =50|ok
comment after a value|freetarget = 50 # comment|ok
comment line|# freetarget = 999|ok
comment line without a blank|#freetarget=999|ok
hash directly after a value|freetarget=50#comment|ok
comment hides nothing it should not|freetarget = 999 # 50|too large
commented out unknown key|# bogus_key = 1|ok
blank lines|\n\n   \n\t\n|ok
carriage return line ending|freetarget = 50\r|ok
double quoted number|freetarget = "50"|ok
double quoted path with a blank|swappath = "$TMP/sp ace"|ok
unquoted path with a blank|swappath = $TMP/sp ace|syntax error
text after a quoted value|swappath = "$TMP/swap" junk|syntax error
text after a bare value|freetarget = 50 51|syntax error
single quotes are not quotes|freetarget = '50'|invalid unit letter
single quoted path|swappath = '$TMP/swap'|not absolute
quote inside a value|freetarget = 5"0"|invalid unit letter
empty double quotes|swappath = ""|not absolute
equals sign without a value|freetarget =|syntax error
equals sign without a key|= 50|syntax error
upper case key|Freetarget = 50|syntax error
upper case key with a quoted value|Freetarget = "50"|syntax error
digit in key|freetarget2 = 50|syntax error
dash in key|free-target = 50|syntax error
colon instead of equals|freetarget: 50|syntax error
two settings on a line|freetarget = 50 quiet|syntax error
command line syntax|--freetarget=50|syntax error
short option|-f 50|syntax error
prose|just some words here|syntax error
number only|50|syntax error
flag with blanks|   quiet   |ok
flag with a comment|quiet # not verbose|ok
flag with a value|quiet = 1|does not take an argument
flag with an empty value|quiet =|syntax error
flag with an empty quoted value|quiet = ""|does not take an argument
number without a value|freetarget|requires an argument
unknown keyword|bogus = 1|unknown configuration item
unknown bare keyword|bogus|unknown configuration item
keyword of 100 characters|$K100 = 1|unknown configuration item
keyword of 101 characters|$K101 = 1|syntax error
EOF

# A last line without a newline is still read, and an empty file is fine.
cp "$TMP/base.conf" "$TMP/case.conf"
printf 'freetarget = 999' >>"$TMP/case.conf"
run -i -c "$TMP/case.conf"
check "conf: last line without newline" "too large"
: >"$TMP/empty.conf"
run -i -c "$TMP/empty.conf" -s "$TMP/swap" -l 50 -f 50 -u 60 -x 5
check "conf: empty file" ok

# The configuration file: -c picks it, a missing one means defaults (with a
# notice unless -q), anything else that cannot be read is an error.
FLAGS="-s $TMP/swap -l 50 -f 50 -u 60 -x 5"
run -i -c "$TMP/missing.conf" $FLAGS
check "missing configuration file" ok
check "missing configuration file notice" "+Using default configuration"
run -i -q -c "$TMP/missing.conf" $FLAGS
check "missing configuration file with -q" ok
check "missing configuration file with -q is silent" "!Using default configuration"
run -i -c "$TMP"
check "configuration file is a directory" "configuration file"
if [ ! -r "$TMP/unreadable.conf" ]; then
  : >"$TMP/unreadable.conf"
  chmod 000 "$TMP/unreadable.conf"
  if [ ! -r "$TMP/unreadable.conf" ]; then
    run -i -c "$TMP/unreadable.conf" $FLAGS
    check "unreadable configuration file" "Could not open configuration file"
  fi
fi
printf 'freetarget = 999\n' >"$TMP/bad.conf"
run -i -c "$TMP/bad.conf"
check "-c selects the bad file" "too large"
run -i -c "$TMP/bad.conf" -c "$TMP/base.conf"
check "the last -c wins" ok
run -i -c "$TMP/base.conf" -c "$TMP/bad.conf"
check "the last -c wins (bad one last)" "too large"
run -i --configfile="$TMP/bad.conf"
check "--configfile selects the bad file" "too large"
cp "$TMP/base.conf" "$TMP/sp ace/space.conf"
run -i -c "$TMP/sp ace/space.conf"
check "configuration file path with a blank" ok
run -c "$TMP/base.conf" -i
check "-c before -i" ok

# Command line syntax that is not an option at all.
LONGNAME=$(awk -v n=16384 'BEGIN { for (i = 0; i < n; i++) printf "a" }')
cli() {
  want=$1; shift
  run "$@" -i -c "$TMP/base.conf"
  check "cli: $*" "$want"
}
cli "Illegal argument: extra" extra
cli "Illegal argument: -" -
cli "Illegal argument: --" --
cli "Illegal argument: --=3" --=3
cli "Illegal argument: --1" --1=3
cli "Illegal option: -Z" -Z
cli "Illegal option: -F" -F
cli "Illegal option: -b" -b
cli "'freetarget': invalid unit letter" -f=50
cli "'free': unknown configuration item" --free=5
cli "'Freetarget': unknown configuration item" --Freetarget=50
cli "'daemon': does not take an argument" --daemon extra
cli "'swappath': requires an argument" --swappath -x 5
cli "Option name too long" --"$LONGNAME"=1
cli "unknown configuration item" --"${LONGNAME%a}"=1

# Relations between options, checked once all sources have been read:
# lower_freelimit <= freetarget <= upper_freelimit, min_swapsize <=
# max_swapsize, no -q together with -v, an absolute swappath, and a swap
# filesystem big enough for the smallest useful swapfile.  Equal values pass.
rows <<EOF
*|lower_freelimit=61|lower_freelimit > upper_freelimit
*|upper_freelimit=49|lower_freelimit > upper_freelimit
*|lower_freelimit=70 freetarget=10|lower_freelimit > upper_freelimit
*|lower_freelimit=51|freetarget < lower_freelimit
*|freetarget=49|freetarget < lower_freelimit
*|freetarget=61|freetarget > upper_freelimit
*|lower_freelimit=61 freetarget=61 upper_freelimit=100|ok
*|lower_freelimit=60 freetarget=60 upper_freelimit=60|ok
*|lower_freelimit=50 freetarget=50 upper_freelimit=50|ok
*|lower_freelimit=2 freetarget=2 upper_freelimit=2|ok
*|min_swapsize=2m max_swapsize=1m|min_swapsize > max_swapsize
*|min_swapsize=2m max_swapsize=2m|ok
*|min_swapsize=2m max_swapsize=3m|ok
*|quiet verbose|quiet & verbose
*|verbose quiet|quiet & verbose
*|swappath=relative|not absolute
*|lower_freelimit=0 freetarget=2 swappath=/proc|isn't big enough
EOF

# The same checks apply when the values come from different sources, and the
# command line wins over the configuration file.
printf 'freetarget = 70\n' >"$TMP/ft70.conf"
sed '/freetarget/d' "$TMP/base.conf" >"$TMP/noft.conf"
cat "$TMP/ft70.conf" >>"$TMP/noft.conf"
run -i -c "$TMP/noft.conf"
check "file alone: freetarget above upper_freelimit" "freetarget > upper_freelimit"
run -i -c "$TMP/noft.conf" -f 55
check "-f overrides the file" ok
run -i -c "$TMP/noft.conf" --freetarget=55
check "--freetarget overrides the file" ok
run -i -f 55 -c "$TMP/noft.conf"
check "-f before -c overrides the file" ok
run -i -c "$TMP/base.conf" -f 70
check "-f overrides a valid file" "freetarget > upper_freelimit"
run -i -c "$TMP/base.conf" -u 49
check "-u against the file's lower_freelimit" "lower_freelimit > upper_freelimit"
run -i -c "$TMP/base.conf" -l 40 -f 40
check "-l and -f against the file's upper_freelimit" ok
printf 'quiet\n' >"$TMP/quiet.conf"
printf 'verbose\n' >"$TMP/verbose.conf"
cat "$TMP/base.conf" >>"$TMP/quiet.conf"
cat "$TMP/base.conf" >>"$TMP/verbose.conf"
run -i -c "$TMP/quiet.conf" -v
check "quiet in the file, -v on the command line" "quiet & verbose"
run -i -c "$TMP/verbose.conf" -q
check "verbose in the file, -q on the command line" "quiet & verbose"
run -i -c "$TMP/quiet.conf" -q
check "quiet in the file and on the command line" ok
printf 'freetarget = 200\n' >"$TMP/ft200.conf"
cat "$TMP/base.conf" >>"$TMP/ft200.conf"
run -i -c "$TMP/ft200.conf" -f 50
check "a bad value in the file is an error even if -f replaces it" "too large"
run -i -c "$TMP/missing.conf" -f 200
check "a bad command line value with no file" "too large"

# Built-in defaults, which are lower_freelimit 20, freetarget 30 and
# upper_freelimit 60: a file that sets only the swap path (and whatever the
# case adds) is checked against them.
printf 'swappath = "%s"\n' "$TMP/swap" >"$TMP/onlypath.conf"
run -i -c "$TMP/onlypath.conf" -u 29
check "default freetarget is above 29" "freetarget > upper_freelimit"
run -i -c "$TMP/onlypath.conf" -l 31
check "default freetarget is below 31" "freetarget < lower_freelimit"
run -i -c "$TMP/onlypath.conf" -l 61
check "default upper_freelimit is below 61" "lower_freelimit > upper_freelimit"
run -i -c "$TMP/onlypath.conf" -f 61
check "default upper_freelimit is below 61 (freetarget)" "freetarget > upper_freelimit"
run -i -c "$TMP/onlypath.conf" -f 19
check "default lower_freelimit is above 19" "freetarget < lower_freelimit"
run -i -c "$TMP/onlypath.conf" -M 4m
check "default min_swapsize is at most 4 MiB" "!min_swapsize > max_swapsize"
run -i -c "$TMP/onlypath.conf" -M $((4*1024*1024-PS))
check "default min_swapsize is above 4 MiB minus a page" "min_swapsize > max_swapsize"
run -i -c "$TMP/onlypath.conf" -m 2t
check "default max_swapsize is at least 2 TiB" "!min_swapsize > max_swapsize"
run -i -c "$TMP/onlypath.conf" -m 2049g
check "default max_swapsize is below 2049 GiB" "min_swapsize > max_swapsize"

# -i only validates: it must not erase, lock or create anything.
SW="$TMP/inspect-swap"
mkdir -m 700 "$SW"
printf 'x' >"$SW/0"
printf 'swappath = "%s"\nlower_freelimit = 50\nfreetarget = 50\nupper_freelimit = 60\n' "$SW" >"$TMP/inspect.conf"
run -i -e -p "$TMP/inspect.pid" -d -c "$TMP/inspect.conf"
check "-i with -e, -p and -d" ok
test -f "$SW/0" || bad "-i erased a swap file"
test -e "$SW/.swapspace.lock" && bad "-i locked the swap directory"
test -e "$TMP/inspect.pid" && bad "-i wrote a pidfile"
run -e -i -c "$TMP/inspect.conf"
check "-e -i" ok
test -f "$SW/0" || bad "-e -i erased a swap file"
printf 'inspect\n' >>"$TMP/inspect.conf"
run -e -c "$TMP/inspect.conf"
check "inspect in the file" ok
test -f "$SW/0" || bad "inspect in the file erased a swap file"
run --inspect -c "$TMP/bad.conf"
check "--inspect with a bad file" "too large"

# -e (erase) runs for real on an empty directory without privileges and
# takes the directory lock, which -i never does.
for how in "-e" "--erase" "-e -q"; do
  dir="$TMP/erase-dir"
  rm -rf "$dir"
  mkdir -m 700 "$dir"
  printf 'swappath = "%s"\nlower_freelimit = 50\nfreetarget = 50\nupper_freelimit = 60\n' "$dir" >"$TMP/erase.conf"
  run $how -c "$TMP/erase.conf"
  check "$how on an empty directory" ok
  test -e "$dir/.swapspace.lock" || bad "$how did not take the directory lock"
done
rm -rf "$dir"
mkdir -m 700 "$dir"
printf 'erase\n' >>"$TMP/erase.conf"
run -c "$TMP/erase.conf"
check "erase in the file" ok
test -e "$dir/.swapspace.lock" || bad "erase in the file did not take the directory lock"
run -e -c "$TMP/bad.conf"
check "-e with a bad file" "too large"
printf 'swappath = "%s"\nlower_freelimit = 50\nfreetarget = 50\nupper_freelimit = 60\n' "$TMP/sp ace" >"$TMP/blank.conf"
run -e -c "$TMP/blank.conf"
check "-e with a blank in the swap path" "swap path contains whitespace"

# -V and -h print to standard output, leave standard error empty and exit
# with status 0 before any configuration is read.
ERR="$TMP/err"
run_split() {
  "$BIN" "$@" >"$OUT" 2>"$ERR" </dev/null
  RC=$?
}
VERSION=$(sed -n 's/^AC_INIT(\[[^]]*\], *\[\([^]]*\)\].*/\1/p' "$SRCDIR/../configure.ac" 2>/dev/null)
for how in -V --version; do
  run_split $how
  check "$how" ok
  test -s "$ERR" && bad "$how wrote to standard error"
  grep -q '^swapspace [0-9]' "$OUT" || bad "$how does not print 'swapspace <version>'"
  if [ -n "$VERSION" ]; then
    [ "$(head -n 1 "$OUT")" = "swapspace $VERSION" ] ||
      bad "$how prints '$(head -n 1 "$OUT")', configure.ac says '$VERSION'"
  fi
  grep -q 'GNU' "$OUT" || bad "$how does not print the licence notice"
done
run_split -V
cp "$OUT" "$TMP/version.out"
run_split --version
cmp -s "$OUT" "$TMP/version.out" || bad "-V and --version print different text"
printf 'version\n' >"$TMP/version.conf"
run_split -c "$TMP/version.conf"
cmp -s "$OUT" "$TMP/version.out" || bad "version in the configuration file prints different text"

run_split -h
cp "$OUT" "$TMP/help.out"
check "-h" ok
test -s "$ERR" && bad "-h wrote to standard error"
grep -q '^Usage: swapspace' "$OUT" || bad "-h does not print a usage line"
run_split --help
cmp -s "$OUT" "$TMP/help.out" || bad "-h and --help print different text"
printf 'help\n' >"$TMP/help.conf"
run_split -c "$TMP/help.conf"
cmp -s "$OUT" "$TMP/help.out" || bad "help in the configuration file prints different text"

# Every option has a line with its short letter, its long name and the kind of
# argument it takes, followed by a description; there are no other lines, and
# they come in the order of the table.
TAB=$(printf '\t')
nopts=0
: >"$TMP/help.names"
while read -r name letter kind; do
  test -n "$name" || continue
  nopts=$((nopts+1))
  case $kind in
  num)  pat="^  -$letter n, +--$name=n *$TAB." ;;
  str)  pat="^  -$letter s, +--$name=s *$TAB." ;;
  flag) pat="^  -$letter, +--$name *$TAB." ;;
  esac
  CASES=$((CASES+1))
  grep -E -q -- "$pat" "$TMP/help.out" || bad "-h has no line like '$pat'"
  echo "$name" >>"$TMP/help.names"
done <<EOF
$OPTIONS
EOF
CASES=$((CASES+1))
[ "$(grep -c '^  -' "$TMP/help.out")" -eq "$nopts" ] ||
  bad "-h lists $(grep -c '^  -' "$TMP/help.out") options, expected $nopts"
sed -n 's/^  -[^,]*, *--\([a-z_]*\).*/\1/p' "$TMP/help.out" >"$TMP/help.order"
cmp -s "$TMP/help.order" "$TMP/help.names" || bad "-h does not list the options in the table's order"
sort -c "$TMP/help.names" 2>/dev/null || bad "OPTIONS is not sorted by long name"

# They stop at the first of -V and -h, but not before an error that comes
# earlier on the command line.
run_split -V -h
cmp -s "$OUT" "$TMP/version.out" || bad "-V -h does not stop at -V"
run_split -h -V
cmp -s "$OUT" "$TMP/help.out" || bad "-h -V does not stop at -h"
for how in -V -h; do
  run_split $how -c "$TMP/missing.conf" --bogus
  check "$how before a bad option" ok
  run_split -c "$TMP/missing.conf" -Z $how
  CASES=$((CASES+1))
  [ -s "$OUT" ] && bad "$how printed text after a bad option"
  cp "$ERR" "$OUT"
  check "$how after a bad option" "Illegal option: -Z"
  run_split $how -c "$TMP/bad.conf"
  check "$how ignores the configuration file" ok
done

# Running daemon: -p, --pidfile and pidfile in the file write the process id,
# -d forks and the parent exits 0 after rewriting the id, and a SIGTERM makes
# the daemon exit 0 and remove the pidfile.  The thresholds (0, 2, 100, no
# PSI) keep it from ever wanting swap, so no privileges are needed.
RUNDIR="$TMP/run-swap"
mkdir -m 700 "$RUNDIR"
cat >"$TMP/run.conf" <<EOF
swappath = "$RUNDIR"
lower_freelimit = 0
freetarget = 2
upper_freelimit = 100
psi_threshold = 0
EOF
PIDF="$TMP/daemon.pid"

# wait_for TEST...: poll up to five seconds for a command to succeed.
wait_for() {
  n=0
  until "$@"; do
    n=$((n+1))
    [ "$n" -le 50 ] || return 1
    sleep 0.1
  done
}
pidfile_ready() { [ -s "$PIDF" ]; }
pidfile_gone() { [ ! -e "$PIDF" ]; }

# Foreground: the pidfile holds our child's process id.
for how in "-p $PIDF" "--pidfile=$PIDF"; do
  rm -f "$PIDF"
  CASES=$((CASES+1))
  "$BIN" -c "$TMP/run.conf" -q $how >"$OUT" 2>&1 </dev/null &
  DPID=$!
  DPIDS=$DPID
  if wait_for pidfile_ready; then
    [ "$(cat "$PIDF")" = "$DPID" ] || bad "$how: pidfile holds '$(cat "$PIDF")', not $DPID"
  else
    bad "$how: no pidfile was written: $(head -n 1 "$OUT")"
  fi
  kill -TERM "$DPID" 2>/dev/null
  wait "$DPID"
  [ $? -eq 0 ] || bad "$how: SIGTERM did not end the daemon with status 0"
  DPIDS=
  pidfile_gone || bad "$how: the pidfile was not removed on exit"
done

# The pidfile can come from the configuration file too.
cp "$TMP/run.conf" "$TMP/runpid.conf"
printf 'pidfile = "%s"\n' "$PIDF" >>"$TMP/runpid.conf"
rm -f "$PIDF"
CASES=$((CASES+1))
"$BIN" -c "$TMP/runpid.conf" -q >"$OUT" 2>&1 </dev/null &
DPID=$!
DPIDS=$DPID
wait_for pidfile_ready || bad "pidfile in the configuration file: no pidfile was written"
[ "$(cat "$PIDF" 2>/dev/null)" = "$DPID" ] || bad "pidfile in the configuration file holds the wrong id"
kill -TERM "$DPID" 2>/dev/null
wait "$DPID"
DPIDS=

# -d: the foreground process exits 0 and the pidfile names the daemon that
# stays behind.
for how in "-d" "--daemon"; do
  rm -f "$PIDF"
  CASES=$((CASES+1))
  "$BIN" -c "$TMP/run.conf" -q -p "$PIDF" $how >"$OUT" 2>&1 </dev/null
  [ $? -eq 0 ] || bad "$how: the foreground process did not exit 0"
  if pidfile_ready; then
    DPID=$(cat "$PIDF")
    DPIDS=$DPID
    kill -0 "$DPID" 2>/dev/null || bad "$how: pidfile names a process that is not running"
    kill -TERM "$DPID" 2>/dev/null
    wait_for pidfile_gone || bad "$how: the pidfile was not removed after SIGTERM"
    DPIDS=
  else
    bad "$how: no pidfile was written"
  fi
done

if [ "$FAILS" -ne 0 ]; then
  echo "$FAILS of $CASES test_options cases failed" >&2
  exit 1
fi
echo "test_options: $CASES cases passed"
exit 0
