/*
 * maytera-dom-limits.h - the ONE maximum DOM depth the browser engine honours.
 * Licensed under the MIT License.
 *
 * THERE IS EXACTLY ONE OF THESE NUMBERS ON PURPOSE (#245). Four places in the
 * engine must refuse to descend an arbitrarily deep DOM, and before this header
 * three of them carried their own copy of "256" with nothing tying them
 * together:
 *
 *   layout.c    WALK_MAX_DEPTH        - the box-tree walk DROPS deeper content
 *   duk_dom.c   FIND_BY_ID_MAX_DEPTH  - getElementById recursion (stack bomb)
 *   libhubbub   insert_element        - the open-element stack / parsed depth
 *   libdom      (bounded transitively by the parser cap above)
 *
 * They are the same number because they answer the same question, and a fourth
 * independent copy is how they drift. Change it HERE.
 *
 * WHY 256 AND NOT MORE. The layout walk already refuses to lay out, and
 * therefore to render, anything nested deeper than this; content below the cap
 * is invisible no matter how faithfully it was parsed. So clamping the PARSER
 * at the same depth cannot change a single pixel of any page, while removing
 * the unbounded-depth work entirely. Blink and WebKit use 512 for the
 * equivalent "maximum HTML parser DOM tree depth" and flatten beyond it; their
 * layout engines render that deep, ours does not, so matching their number
 * would only buy 256 levels of parsed-but-unrenderable nesting.
 *
 * WHAT HAPPENS AT THE CAP. The parser treats an element start tag that would
 * exceed it as a parse error and ignores the token, which is an outcome the
 * HTML5 tree construction algorithm already defines for many tokens. Nothing
 * is dropped except the extra nesting: text and elements inside the ignored
 * tags are still inserted, at the deepest node the cap allows. The document
 * does not truncate and the rest of the page is unaffected.
 */
#ifndef MAYTERA_DOM_LIMITS_H
#define MAYTERA_DOM_LIMITS_H

/* Overridable ONLY so that a measurement rig can separate the effect of the
 * algorithmic fixes from the effect of the cap: with the cap raised, a parse at
 * depth 32000 still happens and its scaling can be fitted. No shipping build
 * sets it. If you are reading this because a build defines it, that build is
 * not the browser. */
#ifndef MAYTERA_DOM_MAX_DEPTH
#define MAYTERA_DOM_MAX_DEPTH 256
#endif

#endif
