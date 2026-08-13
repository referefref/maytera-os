#!/bin/bash
set -e
ROOT=<workspace>
CC=$ROOT/cc.sh
PU=$ROOT/libparserutils/include
WC=$ROOT/libwapcaplet/include
HB=$ROOT/libhubbub/include
INC="-I$PU -I$WC -I$HB"

build_lib() { # $1=dir
  local d=$1
  rm -rf $d/build/obj
  mkdir -p $d/build/obj
  local ok=0 fail=0
  for f in $(find $d/src -name '*.c'); do
    local o=$d/build/obj/$(echo $f | sed "s|$ROOT/||" | tr / _).o
    if $CC -I$d/include -I$d/src $INC -c $f -o $o 2>/tmp/cerr; then ok=$((ok+1)); else fail=$((fail+1)); echo "FAIL $f"; head -4 /tmp/cerr; fi
  done
  echo "$d: OK=$ok FAIL=$fail"
}

build_lib $ROOT/libwapcaplet
build_lib $ROOT/libparserutils
build_lib $ROOT/libhubbub
build_lib $ROOT/libcss
build_lib $ROOT/libdom

cd $ROOT
for L in libwapcaplet libparserutils libhubbub libcss libdom; do
  rm -f $L.a
  ar rcs $L.a $(find $L/build/obj -name '*.o')
  echo "$L.a: $(ar t $L.a | wc -l) members"
done
