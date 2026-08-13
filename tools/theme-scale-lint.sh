#!/usr/bin/env bash
# build/assets/theme-scale-lint.sh - mtheme v2 design-contract check (#711)
#
# A COMMIT-TIME check, never a runtime dependency. Nothing on the image reads
# this script and nothing on the image needs it: the kernel parser accepts any
# integer a .mtheme file gives it. This exists so six designers producing one
# system cannot quietly drift off the published scales, and so a contrast
# regression is a build failure instead of a taste argument.
#
# It checks, for every build/assets/themes/*.mtheme:
#   1. every radius.* is on the radius scale (0 / 3 / 4 / 6 / 10). 4 was
#      added #711 Loop 2 (director redirect B6, settled): the mandated
#      radius.control value for buttons/inputs/dropdowns/menus/selection-
#      row highlights on the modern themes. It sits between the pre-
#      existing sm=3 and md=6 rungs deliberately, not a typo of either.
#   2. metric.pad and metric.gap are on the 4px spacing scale (4 8 12 16 24 32)
#   3. every type.* size is on the type scale (11 14 16 20 28) and its
#      lineheight is round(size * 1.4)
#   4. every type.*_weight is regular or bold (there is no third weight)
#   5. decor.style is beveled | flat | gradient
#   6. the required foreground/background pairs pass WCAG AA (4.5:1) - the
#      ACCESSIBILITY target, i.e. what we want a theme to hit
#   7. the two gradient stops differ by no more than ~12% relative luminance
#      (the renderer has exactly one gradient: 2-stop vertical linear)
#   8. every fg/bg pair kernel/gui/themes.c's theme_ensure_contrast() family
#      enforces at load time (theme_ensure_all_contrast() +
#      theme_ensure_v2_contrast(), 26 pairs total) clears the RUNTIME floor:
#      BT.601 integer luma delta >= 60/255. This is the INTERVENTION point,
#      not the accessibility target - a pair can be WCAG-marginal and still
#      clear it. #6 and #8 are deliberately two different questions over two
#      overlapping-but-not-identical pair sets (#6 covers 7 pairs at 4.5:1,
#      #8 covers all 26 the kernel actually corrects at a 60/255 luma
#      delta); a theme can fail one without failing the other, and the
#      failure message says which. Added after 5 of the then-14 shipped
#      themes (commit 67e4035) passed #6 clean yet still tripped the kernel's
#      runtime floor and got silently auto-corrected in front of a real user
#      ("Theme adjusted for readability") - #6 simply never looked at the
#      two pairs that broke (gauge_fg/gauge_bg, state.item_hover_fg/bg), and
#      no other check did either. #8 closes that by checking the exact same
#      pair set with the exact same integer arithmetic the kernel runs, so
#      "lint passed" now implies "the runtime will not touch this theme"
#      instead of implying nothing about it. See docs/UI_STYLE_GUIDE.md and
#      kernel/gui/themes.c's theme_ensure_contrast()/theme_luma() for the
#      authoritative runtime side of this contract.
#
#      #8's klum() is INTEGER arithmetic (r*299+g*587+b*114)/1000 with
#      int()-truncated division, chosen specifically to match theme_luma()'s
#      uint32_t truncating division bit for bit rather than approximate it
#      with floating point (r,g,b are always 0..255 so the intermediate is
#      always non-negative and never near double's precision limit, so
#      int(x/1000) truncates identically to C's uint32_t "/"). This was
#      cross-checked, not assumed: a host-compiled C harness built from
#      theme_luma() extracted VERBATIM out of kernel/gui/themes.c (sed, not
#      retyped) was run against a systematic 16-step RGB grid (4096 combos)
#      plus every literal colour in all 14 shipped themes, and diffed
#      byte-for-byte against this awk klum() on the same inputs. Zero
#      divergences. Rerun that check yourself with the same method before
#      changing either theme_luma() or klum() - do not just eyeball them.
#
#      #8 is DELIBERATELY NOT SUBJECT TO lint.baseline=legacy-v1
#      grandfathering (see below), unlike #1-#7. All 5 themes that shipped
#      broken (Retro UNIX, Classic, Ocean, Modern Dark, Maytera Dark) carry
#      lint.baseline=legacy-v1 today; if #8's findings were folded into the
#      same suppressible bucket as #1-#7, every one of those themes would
#      have sailed through this gate exactly as before, and the gate would
#      not have closed the gap it exists to close. A runtime auto-correction
#      is not a style debate a legacy theme gets to be exempt from - it is a
#      fact about what the kernel will do to the file as shipped.
#
#  10. the KEYBOARD FOCUS RING contract (#745). color.focus_ring is present
#      in all 14 shipped .mtheme files and has been parsed into
#      theme_t.c_focus_ring since #711, but until #745 it had ZERO READERS:
#      theme_get_color_by_id() had no case for it, so no app could request it
#      and libc's gui_button()/gui_textfield2() drew their rings from `accent`
#      instead. Wiring it up made 14 previously inert values suddenly load-
#      bearing, and 3 of them were below floor: Dark 1.76:1, Forest 2.23:1,
#      Sunset 1.93:1 against their own surface/surface_raised.
#
#      THRESHOLD: 3:1, the WCAG 1.4.11 NON-TEXT figure, not 4.5:1. A focus ring
#      is a boundary, not a glyph; holding a hairline to the text floor is
#      over-correction and yields a garish UI. Both backgrounds are checked
#      because a focused control can sit on the window surface or on a card:
#          color.focus_ring on color.surface
#          color.focus_ring on color.surface_raised
#
#      NOT baseline-suppressible, for the same reason #8 and #9 are not: this
#      is an accessibility floor on a token the runtime now actually draws, not
#      a scale/taste finding a pre-#711 theme gets grandfathered out of. On an
#      OS where pointer input is unreliable (#334) and the keyboard is the
#      primary path, an invisible focus ring is a functional defect.
#
#   9. the TASKBAR SURFACE contract (#745). taskbar_bg is painted by the
#      compositor's taskbar, by solitaire's status strip and - the case that
#      prompted this - by the Settings app's entire left nav, which draws its
#      panel labels ("Sound", "Network", "Mouse"), its panel icons and its
#      "v1.95.0 (build NNNN)" version line on it. Until #745 taskbar_bg had no
#      companion foreground token at all, so those foregrounds borrowed tokens
#      contracted against a DIFFERENT background: label_text (contract:
#      window_bg), button_disabled (contract: button_bg), menu_text_disabled
#      (contract: menu_bg). In any theme whose window is light but whose bar is
#      dark - Ocean, Forest and Sunset all ship that way deliberately - the
#      borrow renders dark ink on a dark surface. MEASURED on the shipped set
#      before the fix: Ocean 1.26:1, Forest 1.09:1, Sunset 1.08:1 for the nav
#      label and its icon, against a 4.5:1 target. That is the user-reported
#      bug ("black/dark grey on dark blue which makes it unreadable").
#
#      Neither #6 nor #8 could see it, and this is the point of adding a check
#      rather than editing three theme files. #6 checks 7 pairs, #8 checks the
#      26 the kernel corrects at load; taskbar_bg appears as a BACKGROUND in
#      neither list, because no token had ever declared itself its foreground.
#      A pair nothing names is a pair nothing measures. #9 names them:
#          taskbar_text          on taskbar_bg      (nav label + icon, rest)
#          taskbar_text          on taskbar_hover   (nav label + icon, hover)
#          taskbar_selected_text on taskbar_active  (nav label + icon, selected)
#          taskbar_text_muted    on taskbar_bg      (the version line)
#
#      THRESHOLD: WCAG AA 4.5:1 on all four, plus the same 60/255 integer luma
#      floor #8 uses. 4.5:1 is the normal-text figure and every one of these
#      four pairs is text. The nav ICONS are not separately checked at the 3:1
#      non-text figure because they do not need to be: draw_mico() in
#      userland/apps/settings/main.c tints the icon glyph to the SAME colour as
#      its row label and blends it against the SAME row background, and the
#      shipped glyphs were measured (each reaches effective coverage 255 over
#      352-1784 pixels of its core) to render at exactly that tint. The icon
#      pair is therefore identical to the label pair, and the stricter 4.5:1
#      text threshold subsumes the 3:1 icon one.
#
#      The three keys are also REQUIRED, not optional. kernel/gui/themes.c's
#      theme_ensure_all_contrast() does now correct them at load, so an
#      omitting theme is rescued rather than broken - but a rescue produces
#      pure black or pure white chosen by the kernel, which is a safety net,
#      not a palette decision. A theme shipped in this repo states its own ink.
#
#      #9 is NOT SUBJECT TO lint.baseline=legacy-v1, for the same reason #8 is
#      not: 12 of the 14 shipped themes carry that tag, including all three
#      that were actually unreadable, so folding #9 into the suppressible
#      bucket would have made it report nothing on precisely the themes it
#      exists to catch.
#
# GRANDFATHERING (checks #1-#7 only; #8 and #9 above are exempt). The twelve themes
# that predate #711 do NOT satisfy this contract: they were authored before
# it existed (metric.pad was the old GUI_PAD literal 10, and several
# palettes put muted ink below 4.5:1 on their own surface). They carry
# "lint.baseline=legacy-v1", which turns their #1-#7 findings into a
# REPORTED, COUNTED suppression rather than a failure, exactly like the
# concurrency-lint allowlist's [LEGACY] tag. Delete that line from a theme
# and it must pass #1-#7; a NEW theme has no line to delete, so it must pass
# from the start. The suppressed count is printed every run so the debt
# stays visible instead of quietly becoming the standard. #8 findings are
# ALWAYS printed and ALWAYS fail the run, baseline tag or not.
#
# Usage: build/assets/theme-scale-lint.sh [dir]     (default: this script's dir/themes)
#        build/assets/theme-scale-lint.sh --self-test
# Exit 0 = clean, 1 = at least one violation.
set -u

DIR="${1:-$(cd "$(dirname "$0")" && pwd)/themes}"
SELFTEST=0
[ "${1:-}" = "--self-test" ] && { SELFTEST=1; DIR="$(cd "$(dirname "$0")" && pwd)/themes"; }

fail=0
clean=0
suppressed=0
note() { printf '%s\n' "$*"; }

lint_dir() {
  local dir="$1" quiet="${2:-0}" local_fail=0
  local f
  for f in "$dir"/*.mtheme; do
    [ -e "$f" ] || continue
    local out rc
    out=$(awk -v FN="$(basename "$f")" '
      function lum_channel(c,   s) {
        s = c / 255.0
        if (s <= 0.03928) return s / 12.92
        return exp(log((s + 0.055) / 1.055) * 2.4)
      }
      # mawk has no strtonum(), so parse hex by hand (portable across
      # mawk/gawk/busybox awk - the lint must not depend on which awk is here).
      function hex2(s,   i, c, v, d) {
        v = 0
        for (i = 1; i <= length(s); i++) {
          c = toupper(substr(s, i, 1))
          d = index("0123456789ABCDEF", c) - 1
          if (d < 0) d = 0
          v = v * 16 + d
        }
        return v
      }
      function lum(hex,   r, g, b) {
        r = hex2(substr(hex, 1, 2))
        g = hex2(substr(hex, 3, 2))
        b = hex2(substr(hex, 5, 2))
        return 0.2126 * lum_channel(r) + 0.7152 * lum_channel(g) + 0.0722 * lum_channel(b)
      }
      function ratio(a, b,   la, lb, t) {
        la = lum(a); lb = lum(b)
        if (la < lb) { t = la; la = lb; lb = t }
        return (la + 0.05) / (lb + 0.05)
      }
      # Integer BT.601 luma, INTEGER arithmetic to match kernel/gui/themes.c
      # theme_luma() truncating uint32_t division exactly (see the header
      # comment above for how this was cross-checked against the real C).
      # DO NOT change this to floating point.
      function klum(hex,   r, g, b, x) {
        r = hex2(substr(hex, 1, 2))
        g = hex2(substr(hex, 3, 2))
        b = hex2(substr(hex, 5, 2))
        x = r * 299 + g * 587 + b * 114
        return int(x / 1000)
      }
      function norm(v) { sub(/^0[xX]/, "", v); return toupper(substr(v, length(v) - 5)) }
      function bad(msg) { MSG[++n] = sprintf("%s: %s", FN, msg) }
      function badfloor(msg) { FMSG[++nf] = sprintf("%s: %s", FN, msg) }
      function badnav(msg) { NMSG[++nn] = sprintf("%s: %s", FN, msg) }   # #745 check 9
      function badring(msg) { RMSG[++nr] = sprintf("%s: %s", FN, msg) }  # #745 check 10
      /^[ \t]*[#;]/ { next }
      /=/ {
        split($0, a, "=")
        k = a[1]; v = a[2]
        gsub(/[ \t\r]/, "", k); gsub(/[ \t\r]/, "", v)
        V[k] = v
      }
      END {
        n = 0
        nf = 0
        nn = 0
        # 1. radius scale
        split("btn input menu card", R, " ")
        for (i in R) {
          k = "radius." R[i]
          if (k in V) {
            v = V[k] + 0
            if (v != 0 && v != 3 && v != 4 && v != 6 && v != 10)
              bad(k "=" V[k] " is not on the radius scale (0/3/4/6/10)")
          }
        }
        # 2. spacing scale
        split("pad gap", S, " ")
        for (i in S) {
          k = "metric." S[i]
          if (k in V) {
            v = V[k] + 0
            if (v != 4 && v != 8 && v != 12 && v != 16 && v != 24 && v != 32)
              bad(k "=" V[k] " is not on the 4px spacing scale (4/8/12/16/24/32)")
          }
        }
        # 3/4. type scale, lineheight and weight
        split("caption:11 body:14 title:16 heading:20 display:28", T, " ")
        for (i in T) {
          split(T[i], p, ":")
          k = "type." p[1]
          if (k in V) {
            v = V[k] + 0
            if (v != 11 && v != 14 && v != 16 && v != 20 && v != 28)
              bad(k "=" V[k] " is not on the type scale (11/14/16/20/28)")
            lh = k "_lineheight"
            if (lh in V) {
              want = int((v * 14 + 5) / 10)
              if ((V[lh] + 0) != want)
                bad(lh "=" V[lh] " should be " want " (round(size*1.4))")
            }
          }
          w = k "_weight"
          if (w in V && V[w] != "regular" && V[w] != "bold")
            bad(w "=" V[w] " must be regular or bold (there is no third weight)")
        }
        # 5. decor.style
        if ("decor.style" in V &&
            V["decor.style"] != "beveled" && V["decor.style"] != "flat" &&
            V["decor.style"] != "gradient")
          bad("decor.style=" V["decor.style"] " must be beveled|flat|gradient")
        # 6. contrast pairs (WCAG AA, 4.5:1) - the accessibility target
        np = split("color.on_surface:color.surface " \
                   "color.on_surface_muted:color.surface " \
                   "color.on_accent:color.accent " \
                   "color.sel_fg:color.sel_bg " \
                   "color.titlebar_text:color.titlebar_bottom " \
                   "state.btn_rest_fg:state.btn_rest_bg " \
                   "state.input_rest_fg:state.input_rest_bg", P, " ")
        for (i = 1; i <= np; i++) {
          split(P[i], q, ":")
          if ((q[1] in V) && (q[2] in V)) {
            r = ratio(norm(V[q[1]]), norm(V[q[2]]))
            if (r < 4.495)
              bad(sprintf("%s on %s is %.2f:1, below WCAG AA 4.5:1", q[1], q[2], r))
          }
        }
        # 7. gradient stop delta
        np = split("color.titlebar_top:color.titlebar_bottom " \
                   "color.titlebar_inactive_top:color.titlebar_inactive_bottom", G, " ")
        for (i = 1; i <= np; i++) {
          split(G[i], q, ":")
          if ((q[1] in V) && (q[2] in V)) {
            d = lum(norm(V[q[1]])) - lum(norm(V[q[2]]))
            if (d < 0) d = -d
            if (d > 0.12)
              bad(sprintf("%s -> %s luminance delta %.3f exceeds the 0.12 gradient budget", q[1], q[2], d))
          }
        }
        # 8. RUNTIME CONTRAST FLOOR - every pair kernel/gui/themes.c
        # theme_ensure_all_contrast() (9 legacy pairs) and
        # theme_ensure_v2_contrast() (17 v2 pairs) enforce at theme_parse_buffer()
        # time, same field-for-field list, same 60/255 integer BT.601 luma
        # floor. If any of these fail, the kernel WILL silently overwrite the
        # fg colour and post a "Theme adjusted for readability" notification -
        # this check exists so that happens at commit time instead of in
        # front of a user. NOT baseline-suppressible (see header comment).
        np = split("label_text:window_bg " \
                   "button_text:button_bg " \
                   "textbox_text:textbox_bg " \
                   "menu_text:menu_bg " \
                   "selection_text:selection_bg " \
                   "titlebar_text:titlebar_active " \
                   "tooltip_text:tooltip_bg " \
                   "gauge_fg:gauge_bg " \
                   "checkbox_check:checkbox_bg " \
                   "color.on_surface:color.surface " \
                   "color.on_accent:color.accent " \
                   "color.sel_fg:color.sel_bg " \
                   "color.titlebar_text:color.titlebar_top " \
                   "color.titlebar_text:color.titlebar_bottom " \
                   "color.titlebar_text_inactive:color.titlebar_inactive_top " \
                   "color.titlebar_text_inactive:color.titlebar_inactive_bottom " \
                   "state.btn_rest_fg:state.btn_rest_bg " \
                   "state.btn_hover_fg:state.btn_hover_bg " \
                   "state.btn_active_fg:state.btn_active_bg " \
                   "state.btn_selected_fg:state.btn_selected_bg " \
                   "state.item_rest_fg:state.item_rest_bg " \
                   "state.item_hover_fg:state.item_hover_bg " \
                   "state.item_active_fg:state.item_active_bg " \
                   "state.item_selected_fg:state.item_selected_bg " \
                   "state.input_rest_fg:state.input_rest_bg " \
                   "state.input_selected_fg:state.input_selected_bg", FP, " ")
        for (i = 1; i <= np; i++) {
          split(FP[i], q, ":")
          if ((q[1] in V) && (q[2] in V)) {
            lf = klum(norm(V[q[1]]))
            lb = klum(norm(V[q[2]]))
            d = lf - lb
            if (d < 0) d = -d
            if (d < 60)
              badfloor(sprintf("RUNTIME FLOOR: %s vs %s luma delta %d/255 is below 60 - " \
                                "theme_ensure_contrast() will silently correct %s to black/white at load", \
                                q[1], q[2], d, q[1]))
          }
        }
        # 9. TASKBAR SURFACE contract (#745). See the header comment for why
        # this pair set is invisible to both #6 and #8. Always reported, always
        # fatal, never baseline-suppressible.
        # 10. KEYBOARD FOCUS RING contract (#745). Non-text floor: 3:1.
        # 2.995 rather than 3.0 to leave room for awk double rounding, the same
        # slack #6/#9 use against 4.5 with 4.495.
        if (!("color.focus_ring" in V))
          badring("color.focus_ring is missing - it is the colour libc draws " \
                  "every keyboard focus ring from (#745)")
        np = split("color.focus_ring:color.surface " \
                   "color.focus_ring:color.surface_raised", RP, " ")
        for (i = 1; i <= np; i++) {
          split(RP[i], q, ":")
          if ((q[1] in V) && (q[2] in V)) {
            r = ratio(norm(V[q[1]]), norm(V[q[2]]))
            if (r < 2.995)
              badring(sprintf("FOCUS RING: %s on %s is %.2f:1, below the WCAG 1.4.11 " \
                              "non-text floor 3:1 - the keyboard focus ring is the " \
                              "primary input affordance on this OS (#334)", \
                              q[1], q[2], r))
          }
        }
        split("taskbar_text taskbar_text_muted taskbar_selected_text", TK, " ")
        for (i = 1; i <= 3; i++)
          if (!(TK[i] in V))
            badnav(TK[i] " is missing - a theme must state its own ink for the " \
                         "taskbar surface (the Settings left nav is drawn on it)")
        np = split("taskbar_text:taskbar_bg " \
                   "taskbar_text:taskbar_hover " \
                   "taskbar_selected_text:taskbar_active " \
                   "taskbar_text_muted:taskbar_bg", NP, " ")
        for (i = 1; i <= np; i++) {
          split(NP[i], q, ":")
          if ((q[1] in V) && (q[2] in V)) {
            r = ratio(norm(V[q[1]]), norm(V[q[2]]))
            if (r < 4.495)
              badnav(sprintf("TASKBAR SURFACE: %s on %s is %.2f:1, below WCAG AA 4.5:1 " \
                             "- this is the Settings left-nav label/icon/version text", \
                             q[1], q[2], r))
            d = klum(norm(V[q[1]])) - klum(norm(V[q[2]]))
            if (d < 0) d = -d
            if (d < 60)
              badnav(sprintf("TASKBAR SURFACE: %s vs %s luma delta %d/255 is below 60 - " \
                             "theme_ensure_all_contrast() will silently correct %s at load", \
                             q[1], q[2], d, q[1]))
          }
        }
        if (nf > 0)
          for (i = 1; i <= nf; i++) print FMSG[i]
        if (nn > 0)
          for (i = 1; i <= nn; i++) print NMSG[i]
        if (nr > 0)
          for (i = 1; i <= nr; i++) print RMSG[i]
        if (n > 0 && V["lint.baseline"] != "legacy-v1")
          for (i = 1; i <= n; i++) print MSG[i]
        if (n > 0 && V["lint.baseline"] == "legacy-v1")
          printf "%s: %d finding(s) SUPPRESSED by lint.baseline=legacy-v1 (pre-#711 theme)\n", FN, n
        if (nf > 0 || nn > 0 || nr > 0) exit 1
        if (n > 0 && V["lint.baseline"] != "legacy-v1") exit 1
        if (n > 0 && V["lint.baseline"] == "legacy-v1") exit 2
        exit 0
      }
    ' "$f" 2>&1)
    rc=$?
    if [ "$rc" = "2" ]; then
      [ "$quiet" = "1" ] || note "$out"
      suppressed=$((suppressed + 1))
    elif [ -n "$out" ] || [ "$rc" != "0" ]; then
      [ "$quiet" = "1" ] || note "$out"
      local_fail=1
    else
      clean=$((clean + 1))
    fi
  done
  return $local_fail
}

if [ "$SELFTEST" = "1" ]; then
  # PROVE the lint goes RED on a deliberately broken theme and GREEN on the
  # shipped set. A lint nobody has seen fail is not a lint.
  tmp=$(mktemp -d)
  trap 'rm -rf "$tmp"' EXIT
  cp "$DIR"/maytera_dark.mtheme "$tmp"/good.mtheme
  {
    grep -v '^lint.baseline=' "$DIR"/retro_unix.mtheme
    echo "radius.btn=7"
    echo "metric.pad=13"
    echo "type.body=15"
    echo "type.body_lineheight=17"
    echo "type.body_weight=light"
    echo "decor.style=neumorphic"
    echo "color.on_surface=0x00777777"
    echo "color.surface=0x00808080"
  } > "$tmp"/broken.mtheme
  # Dedicated #8-only RED case: a pair that is NOT in the WCAG 7-pair set
  # (#6) at all, so this proves #8 catches a defect #6 structurally cannot
  # see, not just a stricter reading of the same pair. Luma(0x105050)=48,
  # luma(0x184848)=44, delta=4 (<60): fails #8. WCAG ratio on the same two
  # colours is ~1.1:1 too, but gauge_fg/gauge_bg was never one of #6's 7
  # checked pairs, so before #8 existed nothing here would have failed.
  {
    cp "$DIR"/maytera_light.mtheme "$tmp"/floor_only_broken.mtheme
    echo "gauge_fg=0x00105050"
    echo "gauge_bg=0x00184848"
  } >> "$tmp"/floor_only_broken.mtheme

  # Dedicated #9-only RED case (#745). Reproduces the EXACT shipped defect:
  # Ocean's real pre-fix values, a dark navy bar with the theme's window ink on
  # it. Neither #6 nor #8 checks taskbar_bg as a background, so before #9
  # existed this file passed the whole lint clean while being the literal thing
  # the user reported as unreadable.
  {
    grep -v '^taskbar_text' "$DIR"/ocean.mtheme
    echo "taskbar_bg=0x00204060"
    echo "taskbar_text=0x00203040"
    echo "taskbar_text_muted=0x00808080"
    echo "taskbar_selected_text=0x00569CD6"
  } > "$tmp"/nav_broken.mtheme

  # Dedicated #10-only RED case (#745). Reproduces the EXACT shipped defect:
  # Dark's real pre-fix color.focus_ring 0x00405080, which measured 1.76:1 on
  # its own color.surface. Applied to maytera_dark, a theme that is otherwise
  # clean, so a RED here can only be check #10. color.focus_ring is not one of
  # #6's 7 WCAG pairs and not one of #8's 26 runtime-floor pairs, so before #10
  # existed this file passed the entire lint while shipping a focus ring the
  # user cannot see. That is not hypothetical: it is what 3 of the 14 shipped
  # themes were doing, undetected, right up until c_focus_ring got its first
  # reader.
  {
    cat "$DIR"/maytera_dark.mtheme
    echo "color.focus_ring=0x00405080"
  } > "$tmp"/ring_broken.mtheme

  gp=0; lint_dir "$tmp" 1 || gp=1
  # Isolate the floor-only case: lint just that one file directly.
  mkdir -p "$tmp/floor_only"
  cp "$tmp"/floor_only_broken.mtheme "$tmp/floor_only/"
  fp=0; lint_dir "$tmp/floor_only" 1 || fp=1
  floor_msg=$(lint_dir "$tmp/floor_only" 0 2>&1)
  # Isolate the #9 case the same way the #8 case is isolated.
  mkdir -p "$tmp/nav_only"
  cp "$tmp"/nav_broken.mtheme "$tmp/nav_only/"
  npf=0; lint_dir "$tmp/nav_only" 1 || npf=1
  nav_msg=$(lint_dir "$tmp/nav_only" 0 2>&1)
  rm -rf "$tmp/nav_only"
  # Isolate the #10 case the same way #8 and #9 are isolated.
  mkdir -p "$tmp/ring_only"
  cp "$tmp"/ring_broken.mtheme "$tmp/ring_only/"
  rpf=0; lint_dir "$tmp/ring_only" 1 || rpf=1
  ring_msg=$(lint_dir "$tmp/ring_only" 0 2>&1)
  rm -rf "$tmp/ring_only"
  rm -f "$tmp"/broken.mtheme "$tmp"/floor_only_broken.mtheme "$tmp"/nav_broken.mtheme "$tmp"/ring_broken.mtheme
  rm -rf "$tmp/floor_only"
  gg=0; lint_dir "$tmp" 1 || gg=1
  if [ "$gp" = "1" ] && [ "$gg" = "0" ]; then
    note "SELF-TEST PASS: RED on a broken theme, GREEN on a good one"
  else
    note "SELF-TEST FAIL: broken=$gp (want 1) good=$gg (want 0)"
    exit 1
  fi
  if [ "$fp" = "1" ] && printf '%s' "$floor_msg" | grep -q "floor_only_broken.mtheme: RUNTIME FLOOR: gauge_fg vs gauge_bg"; then
    note "SELF-TEST PASS: check #8 (runtime floor) is RED on a pair #6 (WCAG) does not even check, and names it"
  else
    note "SELF-TEST FAIL: check #8 floor=$fp (want 1), message=[$floor_msg]"
    exit 1
  fi
  if [ "$npf" = "1" ] && printf '%s' "$nav_msg" | grep -q "nav_broken.mtheme: TASKBAR SURFACE: taskbar_text on taskbar_bg"; then
    note "SELF-TEST PASS: check #9 (taskbar surface) is RED on Ocean's real pre-#745 palette, and names it"
  else
    note "SELF-TEST FAIL: check #9 nav=$npf (want 1), message=[$nav_msg]"
    exit 1
  fi
  if [ "$rpf" = "1" ] && printf '%s' "$ring_msg" | grep -q "ring_broken.mtheme: FOCUS RING: color.focus_ring on color.surface"; then
    note "SELF-TEST PASS: check #10 (focus ring) is RED on Dark's real pre-#745 ring colour, and names it"
  else
    note "SELF-TEST FAIL: check #10 ring=$rpf (want 1), message=[$ring_msg]"
    exit 1
  fi
  note "--- now linting the shipped themes in $DIR ---"
fi

clean=0; suppressed=0
lint_dir "$DIR" || fail=1
note "mtheme v2 scale/contrast lint: ${clean} clean, ${suppressed} grandfathered (lint.baseline=legacy-v1)"
if [ "$fail" = "0" ]; then note "RESULT: OK"; else note "RESULT: FAILED"; fi
exit $fail
