/*
 * paintorder.h - CSS stacking contexts and paint order for the MayteraOS
 * browser engine (#245 engstack).
 *
 * WHAT THIS IS, AND WHY IT IS ITS OWN FILE
 *
 * layout.c emits a FLAT array of layout_items and the painter draws that array
 * in index order, so "paint order" is exactly "array order". Until this file
 * existed, the array was ordered by a single global integer key
 * (ZORDER_BASE + z-index, engabspos), which is correct for ONE stacking context
 * and wrong the moment they nest: a child with z-index:100 inside a parent with
 * z-index:1 beat a sibling of the parent with z-index:2, because the key was
 * global. BROWSER_ENGINE_STACKING_CONTEXT_PLAN.md named that key as the seam to
 * REPLACE rather than extend, and this is the replacement.
 *
 * It is a SEPARATE FILE for the same reason rrect.c is: it has no dependency on
 * libcss, libdom, the layout state or the target at all, so the HOST-side
 * harness compiles the SAME BYTES the OS runs. A paint-order bug is a
 * pure-function bug about integers, and a pure function can be exhaustively
 * tested off-target. po_selftest() is compiled into the browser and prints
 * [STACK] selftest on every launch, so the ordering rules are asserted on the
 * shipping image, not only on a developer's host.
 *
 * THE MODEL
 *
 * Every positioned element gets a GROUP. A group is one of two things:
 *
 *   PO_PROMOTE_CTX  a real stacking context (positioned, with an EXPLICIT
 *                   z-index). It may contain other groups, and its children's
 *                   z-indices are scoped to it. This is the nesting.
 *   PO_PROMOTE_UNIT a positioned box with z-index:auto. CSS 2.1 appendix E
 *                   paints it in step 8 as an ATOMIC unit but it does NOT
 *                   establish a context, so its own positioned descendants
 *                   belong to the nearest ancestor CONTEXT, not to it. A UNIT
 *                   is therefore a LEAF: it never appears as a parent.
 *
 * Group 0 is the root context (the initial containing block). An item's `pg`
 * field names the group it paints in, and 0 (what item_new()'s memset already
 * leaves) means the root, so a page with no positioned boxes carries the INERT
 * value on every item without a single extra write. That is deliberate: the
 * engine's own notes record that a raw libcss enum in a zeroed struct arms a
 * path on every page because libcss puts INHERIT at 0 and the initial value at
 * 1. These constants put the inert value at 0 instead.
 *
 * Within one context, po_cmp() orders by CSS 2.1 appendix E:
 *
 *   level 0  the context element's own background and border      (step 1)
 *   level 1  child contexts with z-index < 0, most negative first  (step 3)
 *   level 2  in-flow, non-positioned content, in document order    (steps 4-7)
 *   level 3  z-index:auto and z-index:0 positioned descendants     (step 8)
 *   level 4  child contexts with z-index > 0, lowest first         (step 9)
 *
 * Steps 4, 5, 6 and 7 (block backgrounds, then floats, then inline content)
 * are COLLAPSED into level 2 and ordered by document order. That is an
 * approximation the engine already made before this file and it is DEFERRED
 * here; see the selftest case "in-flow items of one group order by document
 * order (no float bucket)", which asserts the approximation POSITIVELY so that
 * implementing the float bucket has to delete it.
 */
#ifndef MAYTERA_PAINTORDER_H
#define MAYTERA_PAINTORDER_H

/*
 * The group table is a fixed array so layout needs no allocation to build it.
 * 512 positioned elements is well past any page this engine lays out inside
 * LAYOUT_MAX_ITEMS (4096 items), and the overflow behaviour is defined rather
 * than fatal: po_group_new() hands back the PARENT group, so the element paints
 * with its nearest ancestor context as ordinary in-flow content. Degraded
 * layering, never a bad index.
 */
#define PO_MAX_GROUPS 512

/*
 * Stacking-context NESTING ceiling. Past this depth po_group_new() also returns
 * the parent, for the same reason: po_cmp() walks parent chains, and a bounded
 * depth bounds that walk. 32 nested stacking contexts is far past any real page;
 * the walk depth ceiling in layout.c cuts a page off long before this can bite.
 */
#define PO_MAX_DEPTH 32

/* What po_promote() says about an element. */
#define PO_PROMOTE_NONE 0   /* not positioned: paints as in-flow content */
#define PO_PROMOTE_UNIT 1   /* positioned, z-index:auto: atomic step-8 unit */
#define PO_PROMOTE_CTX  2   /* positioned with an explicit z-index: a context */

/*
 * OUR OWN position constants, with the inert value at 0, NOT the libcss enum.
 * CSS_POSITION_INHERIT is 0x0 in this libcss build and CSS_POSITION_STATIC is
 * 0x1, so a raw libcss value in a zeroed struct reads as INHERIT and a test
 * against it arms on every page. layout.c translates once, at the one site that
 * reads css_computed_position().
 */
#define PO_POS_STATIC   0
#define PO_POS_RELATIVE 1
#define PO_POS_ABSOLUTE 2   /* absolute AND fixed: fixed shares the path */

typedef struct po_group {
	int parent;    /* nearest ancestor CONTEXT; -1 for the root group */
	int z;         /* z-index of the establishing box; 0 for auto and the root */
	int depth;     /* 0 for the root */
	int is_sc;     /* 1 = a context (may be a parent); 0 = an atomic unit */
	int own_idx;   /* item index of the establishing box's own background and
	                * border, or -1. Only a CONTEXT uses it (level 0). */
} po_group;

typedef struct po_table {
	po_group g[PO_MAX_GROUPS];
	int n;            /* groups in use; 1 means root only, i.e. nothing to sort */
	int overflowed;   /* a group was refused (cap or depth) and folded into its parent */
} po_table;

/* One item's sort key: the group it paints in, and its document position. */
typedef struct po_key {
	int pg;
	int idx;
} po_key;

/*
 * The promotion predicate. `pos` is a PO_POS_* value; `has_z` is 1 when the
 * cascade produced an explicit z-index (CSS_Z_INDEX_SET) rather than auto.
 *
 * THE SIGNATURE IS THE DEFERRAL GATE for every other stacking-context trigger.
 * opacity < 1, transform, filter, will-change, isolation and mix-blend-mode all
 * establish a stacking context in CSS and NONE of them is modelled here. They
 * cannot be: this function cannot see them. Adding one means changing this
 * signature and the selftest table that pins it, which is exactly the point.
 */
int po_promote(int pos, int has_z);

/*
 * Clamp a z-index to the range the group table stores. The limit exists so that
 * nothing downstream can overflow on a hostile `z-index: 2147483647`, and it
 * lives HERE rather than inline at the call site so the self-test can assert its
 * boundaries.
 *
 * THE VALUE PASSED IN MUST ALREADY BE A PLAIN INTEGER. libcss hands z-index back
 * through an `int32_t *` out-param that actually carries a `css_fixed`, scaled
 * by 1 << 10, so the caller must convert (FIXTOINT) first. That mistake is
 * monotone and therefore invisible to every comparison; the only thing it breaks
 * is this clamp, which then bites at |z-index| >= 977 instead of 1e6.
 */
#define PO_Z_LIMIT 1000000
int po_zclamp(int z);

void po_reset(po_table *t);

/*
 * Allocate a group whose nearest ancestor CONTEXT is parent_sc. Returns the new
 * group id, or parent_sc when the table or the depth ceiling is full (see
 * PO_MAX_GROUPS). parent_sc must itself be a context; a UNIT is a leaf.
 */
int po_group_new(po_table *t, int parent_sc, int z, int is_sc);

/* Record the establishing box's own decorative item (level 0). */
void po_set_own(po_table *t, int g, int idx);

/*
 * Total order over items. Returns < 0 when (ga, ia) paints FIRST (further from
 * the viewer), > 0 when it paints LAST, 0 only for the same item. It is a
 * strict total order: antisymmetric and transitive, which is what the stable
 * sort below requires to be deterministic.
 */
int po_cmp(const po_table *t, int ga, int ia, int gb, int ib);

/*
 * Stable merge sort of n keys into paint order. `scratch` must hold n keys.
 * A merge sort, not qsort: it is stable by construction, it needs no comparator
 * context (qsort has no way to pass the table), and the same bytes run on the
 * host harness and on the target, so the two cannot disagree.
 */
void po_sort(const po_table *t, po_key *k, po_key *scratch, int n);

/*
 * Run the ordering rules against hand-built tables. Returns the number of
 * FAILING cases; calls report() once per case with a "PASS: ..." / "FAIL: ..."
 * line so the caller can count cases and print the detail.
 */
int po_selftest(void (*report)(const char *line));

#endif /* MAYTERA_PAINTORDER_H */
