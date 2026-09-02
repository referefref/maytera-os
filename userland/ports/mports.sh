#!/usr/bin/env bash
# userland/ports/mports.sh - the MayteraOS ports driver ("mports").
#
# WHAT IT IS. One program that takes a third-party library from a pinned
# upstream tarball to a static library plus headers installed into the userland
# tree, so an app can just link it. Every port is described by a DATA file
# (userland/ports/<name>/PORT); this script is the only executable part, so
# seven ports cannot become seven divergent build hacks. That divergence is the
# problem docs/PORTABILITY_HOMEBREW_SNAPCRAFT_ASSESSMENT.md section 8.2 was
# written about: four private limits.h files in three versions, two divergent
# SDL shims, and two divergent zlib shims of which ONE carried a real shipped
# zero-progress busy-loop bug that was fixed in one copy and not the other.
#
# THE DESIGN RULE THAT SHAPES EVERY LINE BELOW: A PORT THAT SILENTLY NO-OPS
# MUST BE IMPOSSIBLE. This repository has shipped a guardrail that was
# `#!/bin/bash; exit 0`, a lint that could not run because its directory was
# never copied into the build container, and an app that shipped with no binary.
# So there is no code path here that prints "skipping" and returns 0. Every
# absence is a die(). Concretely, the build refuses unless ALL of:
#
#   * the tarball's sha256 equals the pin, byte for byte;
#   * every file named in `licence_files` exists in the unpacked tree (an
#     upstream that moved its LICENSE is an attribution problem, not a nit);
#   * every `prepare` copy names a source that EXISTS and a destination that
#     does NOT yet exist (upstream starting to ship the generated file is a
#     manifest-is-stale event, not something to silently overwrite);
#   * every patch named in `patches` exists AND applies, and every file in
#     patches/ is named in `patches` (an unlisted patch is a silent skip);
#   * every file named in `sources` exists and compiles, and the number of
#     object files equals the number of sources;
#   * every port named in `needs` exists as a port directory (or is the
#     literal `libc`), there is no cycle, and a needed port is BUILT FIRST;
#   * every symbol named in `symbols` is DEFINED in the resulting archive,
#     proven with nm. This is the anti-no-op check: an archive that built
#     cleanly out of the wrong sources, or with the API #ifdef'd away, is the
#     exact shape of failure that "it compiled" does not catch;
#   * every header named in `headers` exists and is installed, and likewise
#     every upstream translation unit named in `install_sources`.
#
# Unknown keys in a PORT file are FATAL, not ignored, because a typo'd key that
# is silently dropped is how a guessed constant ships something that runs and
# does nothing.
#
# LICENCE AND ATTRIBUTION are not this script's to enforce alone; they are
# enforced by the EXISTING attribution gate, which now reads these same PORT
# files: tools/license-audit/vendor-attribution-check.sh, called from
# build/repo-guard.sh check 8. A port with no tools/license-audit/components.tsv
# row, or not named in ATTRIBUTION.md, or whose PORT licence disagrees with its
# components.tsv row, or under any CC-BY-SA licence, FATALs the golden build.
# `mports.sh attribution` runs that same gate so you can see it before the
# build does.
#
# USAGE
#   mports.sh list                      # every port, its version and licence
#   mports.sh show <port>               # the parsed manifest, as understood
#   mports.sh fetch <port>... | --all   # populate the cache, verify sha256
#   mports.sh build <port>... | --all   # fetch, verify, patch, compile, install
#   mports.sh clean [<port>...]         # remove work + installed artefacts
#   mports.sh env                       # resolved directories and flags
#   mports.sh attribution               # run the shared attribution gate
#   mports.sh --self-test               # prove the refusals actually refuse
#                                       # (includes the needs= ordering pair,
#                                       #  proven RED without the edge)
#
# ENVIRONMENT
#   MPORTS_CACHE    tarball cache            (default <workspace>)
#   MPORTS_OFFLINE  1 = never touch the network; a cache miss is fatal and
#                   prints the exact wget command to run
#
# EXIT: 0 success. 1 a port failed. 2 usage / cannot run.

set -uo pipefail

SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PORTS_DIR="$SELF_DIR"
: "${MPORTS_CACHE:=<workspace>}"
: "${MPORTS_OFFLINE:=0}"
WORK="$PORTS_DIR/.work"
OUT="$PORTS_DIR/out"

die()  { printf 'mports: FATAL: %s\n' "$*" >&2; exit 1; }
die2() { printf 'mports: %s\n' "$*" >&2; exit 2; }
log()  { printf 'mports: %s\n' "$*"; }

# ---------------------------------------------------------------------------
# Flags come from ports.mk and are never re-spelled here (see that file).
# ---------------------------------------------------------------------------
# --no-print-directory IS LOAD-BEARING, do not drop it as noise.
#
# This captures make's stdout INTO A VARIABLE that becomes CFLAGS. `-C` turns on
# --print-directory, and `-s` normally suppresses that again, but MAKEFLAGS
# INHERITED FROM A PARENT MAKE OVERRIDES THE LOCAL `-s`. So this call is clean
# when a human runs mports.sh by hand and polluted when it is reached the way
# the public build actually reaches it, `./build.sh` -> `make -C
# userland/apps/terminal` -> here. The captured value then reads
#   make: Entering directory '.../ports'  <flags>  make: Leaving directory ...
# and every one of those words is passed to gcc as an input file:
#   gcc: error: Entering: linker input file not found
# The emptiness guard below cannot catch it, because the polluted value is not
# empty. Measured: 8 of the 11 apps that failed the public build, terminal
# among them, failed only for this reason.
mk_var() {
  local v
  v="$(make -s --no-print-directory -C "$PORTS_DIR" -f ports.mk "print-$1" 2>/dev/null)" \
    || die "cannot read $1 from ports.mk (is make installed, is ports.mk present?)"
  [ -n "$v" ] || die "ports.mk defines $1 as empty; refusing to build with no flags"
  printf '%s' "$v"
}

# ---------------------------------------------------------------------------
# PORT manifest: strict key=value. Every key is whitelisted; an unknown key is
# fatal. Required keys must be present and non-empty.
# ---------------------------------------------------------------------------
PORT_KEYS="name version class licence licence_files homepage source_url tarball sha256 srcdir needs adoption copyleft copyleft_note prepare patches build sources exclude exclude_reason cflags headers install_sources lib symbols"
PORT_REQUIRED="name version class licence licence_files homepage source_url tarball sha256 srcdir needs adoption copyleft prepare patches build headers lib symbols"

# Banned outright in a shipped image (share-alike against our GPLv2 base, and
# the owner's hard constraint for this programme).
LICENCE_BANNED_RE='CC-?BY-?SA|CC-BY-SA|Creative Commons Attribution-ShareAlike'

declare -A P
parse_port() {
  # Two statements, not one: in `local a="$1" b="$a/x"`, bash expands the whole
  # command line before the builtin assigns anything, so $a is still unset there
  # and `set -u` kills the script.
  local name="$1"
  local f="$PORTS_DIR/$name/PORT" k v line lineno=0
  [ -f "$f" ] || die "no manifest at ports/$name/PORT"
  unset P; declare -gA P
  while IFS= read -r line || [ -n "$line" ]; do
    lineno=$((lineno+1))
    case "$line" in ''|\#*) continue ;; esac
    case "$line" in *=*) ;; *) die "$name/PORT:$lineno: not a key=value line: '$line'" ;; esac
    k="${line%%=*}"; v="${line#*=}"
    # trim surrounding whitespace
    k="$(printf '%s' "$k" | sed 's/^[[:space:]]*//; s/[[:space:]]*$//')"
    v="$(printf '%s' "$v" | sed 's/^[[:space:]]*//; s/[[:space:]]*$//')"
    case " $PORT_KEYS " in
      *" $k "*) ;;
      *) die "$name/PORT:$lineno: unknown key '$k'. Keys are whitelisted on purpose: a typo'd key that is silently ignored is how a port ships doing nothing. Valid keys: $PORT_KEYS" ;;
    esac
    [ -z "${P[$k]+x}" ] || die "$name/PORT:$lineno: duplicate key '$k'"
    P[$k]="$v"
  done < "$f"

  for k in $PORT_REQUIRED; do
    [ -n "${P[$k]:-}" ] || die "$name/PORT: required key '$k' is missing or empty"
  done
  [ "${P[name]}" = "$name" ] || die "$name/PORT: name='${P[name]}' does not match its directory '$name'"
  case "${P[class]}" in
    [A-I]) ;;
    *) die "$name/PORT: class='${P[class]}' is not one of A..I (see PORTABILITY_HOMEBREW_SNAPCRAFT_ASSESSMENT.md section 3)" ;;
  esac
  case "${P[build]}" in
    objects) [ -n "${P[sources]:-}" ] || die "$name/PORT: build=objects requires a 'sources' list" ;;
    script)  [ -x "$PORTS_DIR/$name/build.sh" ] || die "$name/PORT: build=script but ports/$name/build.sh is missing or not executable" ;;
    *) die "$name/PORT: build='${P[build]}' is not 'objects' or 'script'" ;;
  esac
  case "${P[copyleft]}" in
    none|aggregate|combined) ;;
    *) die "$name/PORT: copyleft='${P[copyleft]}' must be one of: none (permissive), aggregate (GPL/LGPL work shipped as a separate program), combined (linked into a work we distribute). Answer it per library; do not leave it blank." ;;
  esac
  if [ "${P[copyleft]}" != "none" ] && [ -z "${P[copyleft_note]:-}" ]; then
    die "$name/PORT: copyleft=${P[copyleft]} requires a copyleft_note explaining the aggregation-versus-combined-work reasoning for THIS library"
  fi
  if printf '%s' "${P[licence]}" | grep -qiE "$LICENCE_BANNED_RE"; then
    die "$name/PORT: licence='${P[licence]}' is CC BY-SA. Share-alike content is BANNED from the shipped image (hard constraint); it is copyleft against the GPLv2 base and it is not negotiable per-port."
  fi
  case "${P[sha256]}" in
    [0-9a-f]*) [ "${#P[sha256]}" = 64 ] || die "$name/PORT: sha256 must be 64 lowercase hex characters" ;;
    *) die "$name/PORT: sha256 must be 64 lowercase hex characters" ;;
  esac
  # needs: space-separated. Every entry must be either the literal `libc` (the
  # shared platform library, which is built by userland/libc's own Makefile
  # before mports runs at all) or the DIRECTORY NAME of another port. An entry
  # that is neither is FATAL and is named, because the alternative - treating an
  # unrecognised word as "not a port, therefore no ordering edge" - is exactly
  # the silent-no-op shape this driver exists to make impossible: a typo'd
  # dependency would build in the wrong order and fail somewhere else entirely,
  # or worse, succeed by accident because a previous build left the header in
  # out/include.
  local nn
  for nn in ${P[needs]}; do
    [ "$nn" = libc ] && continue
    if [ "$nn" = "$name" ]; then
      die "$name/PORT: needs names itself"
    fi
    [ -f "$PORTS_DIR/$nn/PORT" ] || die "$name/PORT: needs names '$nn', which is neither the literal 'libc' nor a port directory (there is no ports/$nn/PORT). Ports this tree has: $(port_names | tr '\n' ' ')"
  done

  # prepare: `none`, or space-separated SRC:DST pairs relative to srcdir. This
  # is the pre-configure step some upstreams document for a non-autotools
  # build: PCRE2's NON-AUTOTOOLS-BUILD says in so many words "copy or rename
  # src/config.h.generic as src/config.h", likewise pcre2.h.generic and
  # pcre2_chartables.c.dist. Encoding THAT as data beats freezing a copy of
  # upstream's generated header into our patch series, which would silently
  # rot at the next version bump. Validated here so a malformed entry is a
  # manifest error, not a confusing failure five steps later.
  if [ "${P[prepare]}" != "none" ]; then
    local pp s d
    for pp in ${P[prepare]}; do
      case "$pp" in
        *:*) ;;
        *) die "$name/PORT: prepare entry '$pp' is not SRC:DST. Write it as a copy, e.g. prepare=src/config.h.generic:src/config.h, or the literal 'none'." ;;
      esac
      s="${pp%%:*}"; d="${pp#*:}"
      [ -n "$s" ] && [ -n "$d" ] || die "$name/PORT: prepare entry '$pp' has an empty source or destination"
      case "$s:$d" in
        /*|*:/*|*..*) die "$name/PORT: prepare entry '$pp' escapes the unpacked tree. Both sides must be relative paths inside srcdir, with no '..'." ;;
      esac
    done
  fi
}

port_names() {
  local d
  for d in "$PORTS_DIR"/*/; do
    [ -f "$d/PORT" ] || continue
    basename "$d"
  done
}

# ---------------------------------------------------------------------------
# DEPENDENCY ORDER. `needs=` used to be documentation with nothing behind it,
# and docs/MPORTS.md said so. It is now real: `build --all` and `build <name>`
# both build a port's needed ports FIRST, and a dependent port compiles with
# -I<out>/include so it can actually see the headers its dependency installed.
#
# WHY THIS IS NOT COSMETIC. Without it, the order is the order the shell globs
# the directories, so whether a port builds depends on how its name happens to
# sort. That is the worst kind of green build: it works until somebody adds a
# port whose name sorts earlier. The self-test proves it BOTH ways - the same
# two ports go RED with the edge removed and GREEN with it declared.
#
# port_needs runs parse_port in a SUBSHELL on purpose: parse_port populates the
# single global P, and the caller is usually in the middle of building a
# DIFFERENT port whose P must not be clobbered.
port_needs() ( parse_port "$1" >/dev/null; printf '%s' "${P[needs]:-}" )

declare -A ORDER_STATE
ORDER_OUT=()
order_visit() {
  local n="$1" path="$2" d
  case "${ORDER_STATE[$n]:-0}" in
    2) return 0 ;;
    1) die "dependency CYCLE in 'needs': $path -> $n. Ports are built in dependency order; a cycle has no order. Break it in the manifests." ;;
  esac
  [ -f "$PORTS_DIR/$n/PORT" ] || die "no manifest at ports/$n/PORT"
  ORDER_STATE[$n]=1
  local needs; needs="$(port_needs "$n")" || exit 1
  for d in $needs; do
    [ "$d" = libc ] && continue
    order_visit "$d" "$path -> $n"
  done
  ORDER_STATE[$n]=2
  ORDER_OUT+=("$n")
}

# Depth-first topological sort of the requested ports plus everything they need.
resolve_order() {
  local n
  unset ORDER_STATE; declare -gA ORDER_STATE
  ORDER_OUT=()
  for n in "$@"; do order_visit "$n" "(requested)"; done
}

# ---------------------------------------------------------------------------
# fetch: cache first, network only on a miss, sha256 always.
# ---------------------------------------------------------------------------
sha_of() { sha256sum "$1" | cut -d' ' -f1; }

do_fetch() {
  local name="$1"; parse_port "$name"
  local tb="$MPORTS_CACHE/${P[tarball]}"
  mkdir -p "$MPORTS_CACHE" || die "cannot create cache dir $MPORTS_CACHE"
  if [ -f "$tb" ]; then
    local got; got="$(sha_of "$tb")"
    if [ "$got" = "${P[sha256]}" ]; then
      log "$name: cache hit $tb (sha256 ok)"
      return 0
    fi
    log "$name: cached $tb has sha256 $got, pin says ${P[sha256]}; discarding and refetching"
    rm -f "$tb"
  fi
  if [ "$MPORTS_OFFLINE" = 1 ]; then
    die "$name: cache miss and MPORTS_OFFLINE=1. Populate it by hand:
      mkdir -p $MPORTS_CACHE && wget -O $tb '${P[source_url]}'
      echo '${P[sha256]}  $tb' | sha256sum -c -"
  fi
  log "$name: fetching ${P[source_url]}"
  case "${P[source_url]}" in
    file://*)
      # The VENDORED-TARBALL path: source_url may name a file on this host (a
      # tarball committed beside the recipe, or one an air-gapped operator put
      # there). It goes through the SAME sha256 verification below; there is no
      # trusted-because-local shortcut.
      cp -- "${P[source_url]#file://}" "$tb.part" \
        || { rm -f "$tb.part"; die "$name: cannot read vendored tarball ${P[source_url]#file://}"; } ;;
    *)
      command -v wget >/dev/null || die "$name: no wget available to fetch ${P[source_url]}"
      wget -q --tries=3 --timeout=30 -O "$tb.part" "${P[source_url]}" \
        || { rm -f "$tb.part"; die "$name: fetch FAILED from ${P[source_url]}. If this host has no outbound access, place the tarball at $tb by hand and re-run (its sha256 must be ${P[sha256]})."; } ;;
  esac
  mv "$tb.part" "$tb"
  local got; got="$(sha_of "$tb")"
  [ "$got" = "${P[sha256]}" ] || {
    mv "$tb" "$tb.BADSHA"
    die "$name: sha256 MISMATCH. pin=${P[sha256]} got=$got. The downloaded file is at $tb.BADSHA; it has NOT been used. Either upstream re-rolled the tarball (update the pin deliberately, in a commit, after checking what changed) or this is not the file you think it is."
  }
  log "$name: fetched and verified (sha256 ${P[sha256]})"
}

# ---------------------------------------------------------------------------
# build: extract, verify licence files, patch, compile, archive, verify symbols,
# install. Every step refuses rather than skips.
# ---------------------------------------------------------------------------
do_build() {
  local name="$1"; parse_port "$name"
  do_fetch "$name"
  local tb="$MPORTS_CACHE/${P[tarball]}"
  local w="$WORK/$name"
  rm -rf "$w"; mkdir -p "$w" || die "$name: cannot create work dir $w"
  tar -xf "$tb" -C "$w" || die "$name: cannot unpack $tb"
  local s="$w/${P[srcdir]}"
  [ -d "$s" ] || die "$name: the tarball did not contain srcdir '${P[srcdir]}' (it contains: $(ls "$w" | tr '\n' ' '))"

  # --- licence files must be where the manifest says they are ---------------
  local lf
  for lf in ${P[licence_files]}; do
    [ -f "$s/$lf" ] || die "$name: licence_files names '$lf' but the unpacked tree has no such file. An upstream that moved or renamed its licence text is an ATTRIBUTION problem: fix the manifest deliberately and re-check ATTRIBUTION.md."
  done

  # --- prepare: upstream's own documented pre-configure copies --------------
  # Runs BEFORE the patch series on purpose: a patch may target a file this
  # step creates (pcre2's config.h is copied from config.h.generic and then
  # patched with OUR deltas only, which is what makes the patch reviewable).
  local prepared=0 pp psrc pdst
  if [ "${P[prepare]}" != "none" ]; then
    for pp in ${P[prepare]}; do
      psrc="${pp%%:*}"; pdst="${pp#*:}"
      [ -f "$s/$psrc" ] || die "$name: prepare names '$psrc' but the unpacked tree has no such file. Upstream renamed or dropped it; the manifest is stale."
      [ -e "$s/$pdst" ] && die "$name: prepare would write '$pdst' but the unpacked tree ALREADY has it. Upstream has started shipping the generated file, so this copy is now wrong; decide deliberately rather than overwriting it."
      mkdir -p "$(dirname "$s/$pdst")" || die "$name: cannot create directory for prepare destination '$pdst'"
      cp "$s/$psrc" "$s/$pdst" || die "$name: prepare copy '$psrc' -> '$pdst' FAILED"
      prepared=$((prepared+1))
      log "$name: prepared $pdst from $psrc"
    done
  fi

  # --- patches: listed ones must apply, unlisted ones must not exist --------
  local pdir="$PORTS_DIR/$name/patches" p applied=0
  if [ "${P[patches]}" = "none" ]; then
    if [ -d "$pdir" ] && [ -n "$(ls -A "$pdir" 2>/dev/null)" ]; then
      die "$name: patches=none but ports/$name/patches/ is not empty ($(ls "$pdir" | tr '\n' ' ')). An unlisted patch is a silent skip."
    fi
  else
    for p in ${P[patches]}; do
      [ -f "$pdir/$p" ] || die "$name: patches names '$p' but ports/$name/patches/$p does not exist"
      # -F0: ZERO FUZZ. The default fuzz factor lets patch drop up to two lines
      # of context and apply the hunk anyway "at line 1 with fuzz 2", which is
      # how a stale patch lands somewhere it was never meant to and the build
      # still goes green. Measured in this script's own self-test: a patch whose
      # entire context was wrong applied cleanly until -F0 was added.
      ( cd "$s" && patch -p1 -F0 --batch --no-backup-if-mismatch --forward < "$pdir/$p" ) \
        || die "$name: patch '$p' FAILED to apply cleanly against ${P[srcdir]}. Do not force it: a patch that no longer applies is the version bump telling you it needs review."
      applied=$((applied+1))
      log "$name: applied patch $p"
    done
    # every file in patches/ must be listed
    local f base
    while IFS= read -r f; do
      base="$(basename "$f")"
      case " ${P[patches]} " in *" $base "*) ;; *) die "$name: ports/$name/patches/$base exists but is NOT listed in the manifest's 'patches' key. It would have been silently skipped." ;; esac
    done < <(find "$pdir" -maxdepth 1 -type f 2>/dev/null)
  fi

  # --- compile --------------------------------------------------------------
  local cflags; cflags="$(mk_var MPORTS_CFLAGS)"
  # A port that `needs` another port compiles against the headers that one
  # installed, so the install tree's include dir is on the search path for every
  # port. It goes AFTER the libc include and before the port's own -D flags; a
  # port's own sources still resolve their "quoted" includes out of the unpacked
  # tree first, because the compile runs with cwd = srcdir.
  cflags="$cflags $(mk_var MPORTS_INCLUDE) ${P[cflags]:-}"
  local obj objs=0 nsrc=0 src
  if [ "${P[build]}" = "script" ]; then
    log "$name: build=script, running ports/$name/build.sh"
    ( cd "$s" && MPORTS_CFLAGS="$cflags" MPORTS_OUT="$OUT" MPORTS_SRC="$s" \
        bash "$PORTS_DIR/$name/build.sh" ) || die "$name: build.sh FAILED"
  else
    mkdir -p "$w/obj"
    for src in ${P[sources]}; do
      nsrc=$((nsrc+1))
      [ -f "$s/$src" ] || die "$name: sources names '$src' but it is not in the unpacked tree. Upstream moved it; the manifest is stale."
      obj="$w/obj/$(printf '%s' "$src" | tr '/' '_' | sed 's/\.c$/.o/')"
      ( cd "$s" && gcc $cflags -c "$src" -o "$obj" ) \
        || die "$name: compile FAILED for $src"
      [ -f "$obj" ] || die "$name: compile of $src reported success but produced no object"
      objs=$((objs+1))
    done
    [ "$objs" = "$nsrc" ] || die "$name: compiled $objs objects from $nsrc sources"
    rm -f "$w/${P[lib]}"
    ar rcs "$w/${P[lib]}" "$w"/obj/*.o || die "$name: ar FAILED"
  fi
  local libpath="$w/${P[lib]}"
  [ -f "$libpath" ] || die "$name: no ${P[lib]} was produced"

  # --- THE ANTI-NO-OP CHECK -------------------------------------------------
  # A library that compiled is not a library that contains the API. Prove each
  # promised symbol is DEFINED (not merely referenced) in the archive.
  local defined sym missing="" nsym=0 oksym=0
  defined="$(nm --defined-only "$libpath" 2>/dev/null | awk '{print $NF}' | sort -u)"
  [ -n "$defined" ] || die "$name: nm found NO defined symbols in ${P[lib]}. The archive is empty or unreadable."
  for sym in ${P[symbols]}; do
    nsym=$((nsym+1))
    if grep -qxF "$sym" <<<"$defined"; then oksym=$((oksym+1)); else missing="$missing $sym"; fi
  done
  [ -z "$missing" ] || die "$name: ${P[lib]} built, but these promised symbols are NOT defined in it:$missing
      The port compiled and produced nothing usable. Check 'sources' and any -D in 'cflags'."

  # --- install --------------------------------------------------------------
  mkdir -p "$OUT/include" "$OUT/lib" "$OUT/receipt" || die "$name: cannot create $OUT"
  local h
  for h in ${P[headers]}; do
    [ -f "$s/$h" ] || die "$name: headers names '$h' but it is not in the unpacked tree"
    cp "$s/$h" "$OUT/include/$(basename "$h")" || die "$name: cannot install header $h"
  done
  cp "$libpath" "$OUT/lib/${P[lib]}" || die "$name: cannot install ${P[lib]}"

  # install_sources: upstream .c files that belong to a CONSUMER, not to the
  # library. The case this exists for is a program upstream ships beside its
  # library and links against it - Lua's src/lua.c is the interpreter's main(),
  # excluded from liblua.a on purpose and then compiled by userland/apps/lua.
  #
  # WHY NOT JUST REACH INTO .work/. Because .work is scratch: its path contains
  # the upstream version ("lua-5.4.8"), so every app Makefile pointing at it
  # would have to be edited on a version bump, and `mports.sh clean` deletes it
  # while leaving out/ alone. Installing to out/src/ gives a consumer the same
  # stable contract it already has for out/include and out/lib.
  #
  # The file is installed by BASENAME, exactly like headers, and it is copied
  # AFTER the patch series has run, so a consumer compiles OUR version of it.
  local isrc ninst=0
  if [ -n "${P[install_sources]:-}" ] && [ "${P[install_sources]}" != none ]; then
    mkdir -p "$OUT/src" || die "$name: cannot create $OUT/src"
    for isrc in ${P[install_sources]}; do
      [ -f "$s/$isrc" ] || die "$name: install_sources names '$isrc' but it is not in the unpacked tree. Upstream moved it; the manifest is stale."
      case " ${P[sources]:-} " in
        *" $isrc "*) die "$name: install_sources names '$isrc', which is ALSO in 'sources'. A translation unit cannot be both inside the archive and handed to a consumer to compile: the consumer would link two copies of every symbol in it." ;;
      esac
      cp "$s/$isrc" "$OUT/src/$(basename "$isrc")" || die "$name: cannot install source $isrc"
      ninst=$((ninst+1))
    done
  fi

  local bytes; bytes="$(stat -c %s "$OUT/lib/${P[lib]}")"
  {
    echo "port=$name"
    echo "version=${P[version]}"
    echo "licence=${P[licence]}"
    echo "tarball=${P[tarball]}"
    echo "sha256=${P[sha256]}"
    echo "needs=${P[needs]}"
    echo "prepare=${P[prepare]} (copied=$prepared)"
    echo "patches=${P[patches]} (applied=$applied)"
    echo "lib=${P[lib]} bytes=$bytes libsha256=$(sha_of "$OUT/lib/${P[lib]}")"
    echo "headers=${P[headers]}"
    echo "install_sources=${P[install_sources]:-none} (installed=$ninst)"
    echo "symbols_verified=$oksym/$nsym"
    echo "built=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  } > "$OUT/receipt/$name.txt"

  log "OK $name ${P[version]} -> $OUT/lib/${P[lib]} ($bytes bytes), $(printf '%s' "${P[headers]}" | wc -w) header(s), $oksym/$nsym promised symbols defined, $prepared prepare copy(ies), $applied patch(es) applied, $ninst consumer source(s) installed"
}

do_clean() {
  local name="$1"; parse_port "$name"
  rm -rf "$WORK/$name"
  rm -f "$OUT/lib/${P[lib]}" "$OUT/receipt/$name.txt"
  local h; for h in ${P[headers]}; do rm -f "$OUT/include/$(basename "$h")"; done
  local i; for i in ${P[install_sources]:-}; do [ "$i" = none ] || rm -f "$OUT/src/$(basename "$i")"; done
  log "$name: cleaned"
}

do_list() {
  local n
  printf '%-12s %-10s %-6s %-24s %s\n' PORT VERSION CLASS LICENCE STATUS
  for n in $(port_names); do
    parse_port "$n"
    local st="not built"
    [ -f "$OUT/lib/${P[lib]}" ] && st="built ($(stat -c %s "$OUT/lib/${P[lib]}") bytes)"
    printf '%-12s %-10s %-6s %-24s %s\n' "$n" "${P[version]}" "${P[class]}" "${P[licence]}" "$st"
  done
}

do_show() {
  local n="$1" k; parse_port "$n"
  for k in $PORT_KEYS; do [ -n "${P[$k]:-}" ] && printf '%-15s %s\n' "$k" "${P[$k]}"; done
  return 0
}

do_env() {
  echo "PORTS_DIR      $PORTS_DIR"
  echo "MPORTS_CACHE   $MPORTS_CACHE"
  echo "MPORTS_OFFLINE $MPORTS_OFFLINE"
  echo "WORK           $WORK"
  echo "OUT            $OUT"
  echo "MPORTS_CFLAGS  $(mk_var MPORTS_CFLAGS)"
  echo "MPORTS_LDFLAGS $(mk_var MPORTS_LDFLAGS)"
}

do_attribution() {
  # ONE gate, not a second one: this is the same script build/repo-guard.sh
  # check 8 runs. It reads these PORT files directly.
  local chk="$PORTS_DIR/../../tools/license-audit/vendor-attribution-check.sh"
  [ -f "$chk" ] || die "tools/license-audit/vendor-attribution-check.sh not found at $chk"
  bash "$chk" --root "$(cd "$PORTS_DIR/../.." && pwd)"
}

# ---------------------------------------------------------------------------
# SELF-TEST. A refusal that has never been seen to refuse is not a refusal.
# Builds a synthetic port in a temp tree and proves each guard goes RED.
# ---------------------------------------------------------------------------
ST_TMP=""
self_test() {
  local pass=0 total=0 tmp
  # An EXIT trap on a global, not a RETURN trap on a local: a RETURN trap runs
  # AFTER the function's locals are gone, so `trap 'rm -rf "$tmp"' RETURN`
  # under `set -u` dies with "tmp: unbound variable" and turns a passing
  # self-test into a non-zero exit. Measured, here, on the first run.
  ST_TMP="$(mktemp -d)" || die2 "mktemp failed"
  trap 'rm -rf "$ST_TMP"' EXIT
  tmp="$ST_TMP"

  # A synthetic upstream: two C files, a LICENSE, one header.
  mkdir -p "$tmp/up/toy-1.0"
  cat > "$tmp/up/toy-1.0/toy.h" <<'EOF'
int toy_add(int a, int b);
int toy_mul(int a, int b);
EOF
  cat > "$tmp/up/toy-1.0/add.c" <<'EOF'
#include "toy.h"
int toy_add(int a, int b) { return a + b; }
EOF
  cat > "$tmp/up/toy-1.0/mul.c" <<'EOF'
#include "toy.h"
int toy_mul(int a, int b) { return a * b; }
EOF
  # A header that ONLY the 'zzz' port installs, used by the ordering cases far
  # below. Nothing else in this self-test ever installs it, so a compile that
  # finds it proves zzz really was built first.
  cat > "$tmp/up/toy-1.0/zzzapi.h" <<'EOF'
/* installed into out/include by the 'zzz' port */
int toy_add(int a, int b);
EOF
  echo "Toy licence. Permissive." > "$tmp/up/toy-1.0/LICENSE"
  # A "generated file shipped under a .generic name", i.e. what PCRE2 and
  # friends do for a non-autotools build. The prepare cases below copy it into
  # place and then patch it.
  cat > "$tmp/up/toy-1.0/toyconf.h.generic" <<'EOF'
/* upstream's pre-generated config, copied into place by `prepare` */
#define TOY_CONFIGURED 1
EOF
  ( cd "$tmp/up" && tar -czf "$tmp/toy-1.0.tar.gz" toy-1.0 ) || die2 "tar failed"
  local tsha; tsha="$(sha_of "$tmp/toy-1.0.tar.gz")"

  # A private ports tree that reuses THIS script and THIS ports.mk.
  mkdir -p "$tmp/ports/toy/patches"
  cp "$PORTS_DIR/ports.mk" "$tmp/ports/ports.mk"
  cp "${BASH_SOURCE[0]}" "$tmp/ports/mports.sh"
  # ports.mk resolves LIBC_DIR relative to itself; point it at the real one.
  local real_libc; real_libc="$(cd "$PORTS_DIR/../libc" && pwd)"

  write_manifest() {  # $1 = sha, $2 = patches, $3 = symbols, $4 = licence, $5 = prepare (default none)
    cat > "$tmp/ports/toy/PORT" <<EOF
name=toy
version=1.0
class=A
licence=$4
licence_files=LICENSE
homepage=https://example.invalid/toy
source_url=file://$tmp/toy-1.0.tar.gz
tarball=toy-1.0.tar.gz
sha256=$1
srcdir=toy-1.0
needs=libc
adoption=USES the platform; pure computation, no OS surface.
copyleft=none
prepare=${5:-none}
patches=$2
build=objects
sources=add.c mul.c
headers=toy.h
lib=libtoy.a
symbols=$3
EOF
  }

  run_toy() {
    ( cd "$tmp/ports" && MPORTS_CACHE="$tmp/cache" LIBC_DIR="$real_libc" \
        bash mports.sh build toy ) 2>&1
  }

  mkdir -p "$tmp/cache"; cp "$tmp/toy-1.0.tar.gz" "$tmp/cache/"

  chk() { # $1 desc, $2 expect(GREEN|RED), $3 needle
    total=$((total+1))
    local out rc
    # Re-seed the cache each time: a previous case may legitimately have
    # quarantined the tarball as .BADSHA, and this is the fetch step working,
    # not state to preserve between cases.
    cp -f "$tmp/toy-1.0.tar.gz" "$tmp/cache/toy-1.0.tar.gz"
    out="$(run_toy)"; rc=$?
    if [ "$2" = GREEN ]; then
      if [ $rc -eq 0 ]; then echo "  [PASS] $total $1 -> GREEN"; pass=$((pass+1));
      else echo "  [FAIL] $total $1 -> expected GREEN, got RED"; printf '%s\n' "$out" | sed 's/^/         /'; fi
    else
      if [ $rc -ne 0 ] && printf '%s' "$out" | grep -qF "$3"; then
        echo "  [PASS] $total $1 -> RED, and for the right reason"; pass=$((pass+1))
      elif [ $rc -ne 0 ]; then
        echo "  [FAIL] $total $1 -> RED but not for '$3'"; printf '%s\n' "$out" | sed 's/^/         /'
      else
        echo "  [FAIL] $total $1 -> ACCEPTED (GREEN); the guard does not guard"; printf '%s\n' "$out" | sed 's/^/         /'
      fi
    fi
  }

  echo "mports self-test: prove every refusal actually refuses."

  # 1. good port builds
  write_manifest "$tsha" none "toy_add toy_mul" Toy-Permissive
  chk "a correct port" GREEN ""

  # 2. sha256 mismatch is fatal and the file is not used
  write_manifest "0000000000000000000000000000000000000000000000000000000000000000" none "toy_add toy_mul" Toy-Permissive
  chk "sha256 pin mismatch" RED "sha256 MISMATCH"

  # 3. a promised symbol that is not in the archive is fatal
  write_manifest "$tsha" none "toy_add toy_nonexistent" Toy-Permissive
  chk "promised symbol absent from the archive" RED "are NOT defined in it"

  # 4. THE PATCH PATH ITSELF, end to end. The zlib recipe needs no patches, so
  #    without this case the whole patch mechanism would ship unexercised. The
  #    patch introduces a NEW function and the manifest then REQUIRES that
  #    function's symbol, so a patch that silently failed to apply cannot pass:
  #    the nm check would not find toy_patched.
  cat > "$tmp/ports/toy/patches/0001-add-toy-patched.patch" <<'EOF'
--- a/add.c
+++ b/add.c
@@ -1,2 +1,3 @@
 #include "toy.h"
 int toy_add(int a, int b) { return a + b; }
+int toy_patched(void) { return 42; }
EOF
  write_manifest "$tsha" "0001-add-toy-patched.patch" "toy_add toy_mul toy_patched" Toy-Permissive
  chk "a patch that applies, whose new symbol the manifest requires" GREEN ""

  # 5. an unlisted patch file must not be silently skipped, even when the
  #    manifest does list other patches.
  printf -- '--- a/mul.c\n+++ b/mul.c\n' > "$tmp/ports/toy/patches/0002-stray.patch"
  chk "a second patch file the manifest does not list" RED "is NOT listed in the manifest"
  rm -f "$tmp/ports/toy/patches/0002-stray.patch"

  # 6. a patch that no longer applies is fatal, not forced.
  cat > "$tmp/ports/toy/patches/0002-wont-apply.patch" <<'EOF'
--- a/mul.c
+++ b/mul.c
@@ -1,2 +1,3 @@
 #include "nonexistent-context.h"
 int something_else(void) { return 0; }
+int never(void) { return 1; }
EOF
  write_manifest "$tsha" "0001-add-toy-patched.patch 0002-wont-apply.patch" "toy_add toy_mul toy_patched" Toy-Permissive
  chk "a patch whose context no longer matches upstream" RED "FAILED to apply cleanly"
  rm -f "$tmp/ports/toy/patches"/*.patch

  # 7. a listed patch that does not exist is fatal
  write_manifest "$tsha" "0001-missing.patch" "toy_add toy_mul" Toy-Permissive
  chk "a listed patch that is not on disk" RED "does not exist"

  # 8. patches=none with a file sitting in patches/ is fatal
  printf -- '--- a/add.c\n+++ b/add.c\n' > "$tmp/ports/toy/patches/0001-stray.patch"
  write_manifest "$tsha" none "toy_add toy_mul" Toy-Permissive
  chk "patches=none while patches/ is not empty" RED "patches=none but"
  rm -f "$tmp/ports/toy/patches"/*.patch

  # 9. CC BY-SA is refused outright
  write_manifest "$tsha" none "toy_add toy_mul" CC-BY-SA-4.0
  chk "a CC BY-SA licence" RED "is CC BY-SA"

  # 10. an unknown manifest key is fatal, not ignored
  write_manifest "$tsha" none "toy_add toy_mul" Toy-Permissive
  echo "buidl=objects" >> "$tmp/ports/toy/PORT"
  chk "a typo'd manifest key" RED "unknown key"

  # 11. a source file the manifest names but upstream does not have
  write_manifest "$tsha" none "toy_add toy_mul" Toy-Permissive
  sed -i 's/^sources=.*/sources=add.c mul.c ghost.c/' "$tmp/ports/toy/PORT"
  chk "a source the upstream tarball does not contain" RED "is not in the unpacked tree"

  # 12. a licence file the manifest names but upstream does not have
  write_manifest "$tsha" none "toy_add toy_mul" Toy-Permissive
  sed -i 's/^licence_files=.*/licence_files=COPYING/' "$tmp/ports/toy/PORT"
  chk "a licence file the upstream tarball does not contain" RED "no such file"

  # --- prepare: the pre-configure copy step --------------------------------
  # 13. a prepare copy that a patch then edits, whose new symbol is required.
  #     This is the ORDER check as much as the copy check: if prepare ran after
  #     the patch series (or not at all) the patch could not apply, and if the
  #     patch silently did not apply, toy_prepared would not be in the archive.
  cat > "$tmp/ports/toy/patches/0003-use-prepared-config.patch" <<'EOF'
--- a/toyconf.h
+++ b/toyconf.h
@@ -1,2 +1,3 @@
 /* upstream's pre-generated config, copied into place by `prepare` */
 #define TOY_CONFIGURED 1
+#define TOY_MAYTERA 1
--- a/mul.c
+++ b/mul.c
@@ -1,2 +1,7 @@
 #include "toy.h"
 int toy_mul(int a, int b) { return a * b; }
+#include "toyconf.h"
+#if !defined(TOY_CONFIGURED) || !defined(TOY_MAYTERA)
+#error "prepare did not run before the patch series"
+#endif
+int toy_prepared(void) { return TOY_CONFIGURED + TOY_MAYTERA; }
EOF
  write_manifest "$tsha" "0003-use-prepared-config.patch" \
                 "toy_add toy_mul toy_prepared" Toy-Permissive \
                 "toyconf.h.generic:toyconf.h"
  chk "a prepare copy, patched, whose new symbol the manifest requires" GREEN ""
  rm -f "$tmp/ports/toy/patches"/*.patch

  # 14. a prepare source upstream does not have is fatal
  write_manifest "$tsha" none "toy_add toy_mul" Toy-Permissive "ghostconf.h.generic:toyconf.h"
  chk "a prepare source the tarball does not contain" RED "prepare names 'ghostconf.h.generic'"

  # 15. a prepare destination that ALREADY exists is fatal, not an overwrite.
  #     This is the upstream-started-shipping-the-generated-file case; silently
  #     clobbering it would hide a real version-bump event.
  write_manifest "$tsha" none "toy_add toy_mul" Toy-Permissive "toyconf.h.generic:toy.h"
  chk "a prepare destination that already exists" RED "ALREADY has it"

  # 16. a malformed prepare entry is a manifest error, not a mystery later
  write_manifest "$tsha" none "toy_add toy_mul" Toy-Permissive "toyconf.h.generic"
  chk "a prepare entry that is not SRC:DST" RED "is not SRC:DST"

  # --- needs=: ordering that is REAL, proven by making it fail ---------------
  # Two ports out of the same synthetic upstream:
  #   zzz  installs zzzapi.h into out/include.
  #   aaa  is patched so its mul.c does #include <zzzapi.h>, which can ONLY
  #        resolve through -I out/include, i.e. only if zzz built first.
  # Directory order is aaa, zzz - the WRONG order - so these two cases are the
  # whole point: 18 removes the edge and must go RED, 19 declares it and must go
  # GREEN. A guard shown only succeeding has not been shown to guard.
  # --- install_sources: consumer translation units --------------------------
  # 17. a file the tarball does not have
  write_manifest "$tsha" none "toy_add toy_mul" Toy-Permissive
  echo "install_sources=ghostmain.c" >> "$tmp/ports/toy/PORT"
  chk "an install_sources file the tarball does not contain" RED "install_sources names 'ghostmain.c'"
  # 18. a file that is ALSO in the library's own source list: the consumer would
  #     end up with two copies of every symbol in it.
  write_manifest "$tsha" none "toy_add toy_mul" Toy-Permissive
  echo "install_sources=add.c" >> "$tmp/ports/toy/PORT"
  chk "an install_sources file that is also in 'sources'" RED "which is ALSO in 'sources'"

  # `build --all` builds every port in the toy tree, including `toy`, whose
  # manifest the case above deliberately left malformed. Put it back first, or
  # these cases would go RED for somebody else's reason.
  write_manifest "$tsha" none "toy_add toy_mul" Toy-Permissive
  mkdir -p "$tmp/ports/aaa/patches" "$tmp/ports/zzz"
  cat > "$tmp/ports/aaa/patches/0001-need-zzz-header.patch" <<'EOF'
--- a/mul.c
+++ b/mul.c
@@ -1,2 +1,4 @@
 #include "toy.h"
 int toy_mul(int a, int b) { return a * b; }
+#include <zzzapi.h>
+int aaa_uses_zzz(void) { return toy_add(1, 2); }
EOF
  write_dep_manifest() {  # $1 = port, $2 = needs, $3 = sources, $4 = patches,
                          # $5 = headers, $6 = lib, $7 = symbols
    cat > "$tmp/ports/$1/PORT" <<EOF
name=$1
version=1.0
class=A
licence=Toy-Permissive
licence_files=LICENSE
homepage=https://example.invalid/$1
source_url=file://$tmp/toy-1.0.tar.gz
tarball=toy-1.0.tar.gz
sha256=$tsha
srcdir=toy-1.0
needs=$2
adoption=USES the platform; pure computation, no OS surface.
copyleft=none
prepare=none
patches=$4
build=objects
sources=$3
headers=$5
lib=$6
symbols=$7
EOF
  }
  run_all() { ( cd "$tmp/ports" && MPORTS_CACHE="$tmp/cache" LIBC_DIR="$real_libc" \
                  bash mports.sh build --all ) 2>&1; }
  chk_all() { # $1 desc, $2 expect, $3 needle
    total=$((total+1))
    local out rc
    cp -f "$tmp/toy-1.0.tar.gz" "$tmp/cache/toy-1.0.tar.gz"
    # A previous case may have left zzzapi.h in out/include, which would let the
    # WRONG order succeed by accident. That accident is the exact bug this pair
    # is testing for, so wipe the install tree between the two runs.
    rm -rf "$tmp/ports/out" "$tmp/ports/.work"
    out="$(run_all)"; rc=$?
    if [ "$2" = GREEN ]; then
      if [ $rc -eq 0 ]; then echo "  [PASS] $total $1 -> GREEN"; pass=$((pass+1));
      else echo "  [FAIL] $total $1 -> expected GREEN, got RED"; printf '%s\n' "$out" | sed 's/^/         /'; fi
    else
      if [ $rc -ne 0 ] && printf '%s' "$out" | grep -qF "$3"; then
        echo "  [PASS] $total $1 -> RED, and for the right reason"; pass=$((pass+1))
      elif [ $rc -ne 0 ]; then
        echo "  [FAIL] $total $1 -> RED but not for '$3'"; printf '%s\n' "$out" | sed 's/^/         /'
      else
        echo "  [FAIL] $total $1 -> ACCEPTED (GREEN); the guard does not guard"; printf '%s\n' "$out" | sed 's/^/         /'
      fi
    fi
  }
  write_dep_manifest zzz libc add.c none zzzapi.h libzzz.a toy_add
  # 18. THE NEGATIVE: aaa does not declare the edge, so directory order builds it
  #     first and its <zzzapi.h> is not there yet.
  write_dep_manifest aaa libc mul.c 0001-need-zzz-header.patch toy.h libaaa.a "toy_mul aaa_uses_zzz"
  chk_all "two ports built in DIRECTORY order, with the edge NOT declared" RED "zzzapi.h"
  # 19. THE POSITIVE: same two ports, edge declared, nothing else changed.
  write_dep_manifest aaa zzz mul.c 0001-need-zzz-header.patch toy.h libaaa.a "toy_mul aaa_uses_zzz"
  chk_all "the same two ports with needs=zzz declared" GREEN ""
  # 20. a needs entry that is not a port and is not libc
  write_dep_manifest aaa ghostport mul.c 0001-need-zzz-header.patch toy.h libaaa.a "toy_mul aaa_uses_zzz"
  chk_all "a needs entry naming a port that does not exist" RED "which is neither the literal 'libc' nor a port directory"
  # 21. a dependency cycle has no build order
  write_dep_manifest aaa zzz mul.c 0001-need-zzz-header.patch toy.h libaaa.a "toy_mul aaa_uses_zzz"
  write_dep_manifest zzz aaa add.c none zzzapi.h libzzz.a toy_add
  chk_all "a dependency cycle between two ports" RED "dependency CYCLE"
  rm -rf "$tmp/ports/aaa" "$tmp/ports/zzz" "$tmp/ports/out" "$tmp/ports/.work"

  # 22. back to good, to prove the tree was not left poisoned
  write_manifest "$tsha" none "toy_add toy_mul" Toy-Permissive
  chk "the corrected port again" GREEN ""

  echo "mports self-test: $pass/$total"
  [ "$pass" = "$total" ]
}

# ---------------------------------------------------------------------------
main() {
  [ $# -ge 1 ] || { grep -E '^#( |$)' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
  local cmd="$1"; shift
  local names=()
  expand() {
    if [ $# -eq 0 ]; then die2 "which port? give a name or --all"; fi
    if [ "$1" = --all ]; then mapfile -t names < <(port_names); else names=("$@"); fi
    [ "${#names[@]}" -gt 0 ] || die2 "no ports found under $PORTS_DIR"
  }
  case "$cmd" in
    list)  do_list ;;
    show)  [ $# -eq 1 ] || die2 "show <port>"; do_show "$1" ;;
    env)   do_env ;;
    attribution) do_attribution ;;
    fetch) expand "$@"; for n in "${names[@]}"; do do_fetch "$n"; done ;;
    build) expand "$@"
           # Build in DEPENDENCY order, not in the order the caller typed or the
           # shell globbed. resolve_order also pulls in ports that were not asked
           # for but are needed, so `build lua` can never fail on a missing
           # dependency it was perfectly able to build.
           resolve_order "${names[@]}"
           if [ "${#ORDER_OUT[@]}" -ne "${#names[@]}" ]; then
             log "build order (dependencies first): ${ORDER_OUT[*]}"
           fi
           for n in "${ORDER_OUT[@]}"; do do_build "$n"; done ;;
    clean) if [ $# -eq 0 ]; then mapfile -t names < <(port_names); else names=("$@"); fi
           for n in "${names[@]}"; do do_clean "$n"; done; rm -rf "$WORK" ;;
    --self-test) self_test ;;
    -h|--help) grep -E '^#( |$)' "$0" | sed 's/^# \{0,1\}//' ;;
    *) die2 "unknown command '$cmd' (try --help)" ;;
  esac
}
main "$@"
