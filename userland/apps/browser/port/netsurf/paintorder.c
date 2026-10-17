/*
 * paintorder.c - CSS stacking contexts and paint order (#245 engstack).
 * See paintorder.h for the model. No libc, no libcss, no layout state: the
 * host-side harness compiles these exact bytes.
 */
#include "paintorder.h"

int po_promote(int pos, int has_z)
{
	if (pos == PO_POS_ABSOLUTE)
		return has_z ? PO_PROMOTE_CTX : PO_PROMOTE_UNIT;
	if (pos == PO_POS_RELATIVE)
		/*
		 * DEFERRED, and this is the whole of the deferral. CSS 2.1 appendix E
		 * paints a position:relative box with z-index:auto in step 8, above
		 * in-flow content, exactly like an absolute one. This engine has never
		 * done that (engabspos promoted a relative box only with an explicit
		 * z-index, to keep its own AE=0 argument), and engstack deliberately
		 * did NOT change it, so that every paint-order movement measured for
		 * engstack is attributable to NESTING and nothing else.
		 *
		 * GATE: the selftest case "DEFERRED: relative with z-index:auto is NOT
		 * promoted", which asserts PO_PROMOTE_NONE positively on every browser
		 * launch. Whoever implements it has to delete that case, which is the
		 * point: the boundary is stated, not forgotten.
		 */
		return has_z ? PO_PROMOTE_CTX : PO_PROMOTE_NONE;
	return PO_PROMOTE_NONE;
}

int po_zclamp(int z)
{
	if (z > PO_Z_LIMIT) return PO_Z_LIMIT;
	if (z < -PO_Z_LIMIT) return -PO_Z_LIMIT;
	return z;
}

void po_reset(po_table *t)
{
	t->n = 1;
	t->overflowed = 0;
	t->g[0].parent = -1;
	t->g[0].z = 0;
	t->g[0].depth = 0;
	t->g[0].is_sc = 1;
	t->g[0].own_idx = -1;
}

int po_group_new(po_table *t, int parent_sc, int z, int is_sc)
{
	int id;
	if (parent_sc < 0 || parent_sc >= t->n)
		parent_sc = 0;
	if (t->n >= PO_MAX_GROUPS || t->g[parent_sc].depth + 1 > PO_MAX_DEPTH) {
		/* Defined degradation: the element joins its nearest ancestor context
		 * as ordinary in-flow content. Layering is wrong, nothing is unsafe. */
		t->overflowed = 1;
		return parent_sc;
	}
	id = t->n++;
	t->g[id].parent = parent_sc;
	t->g[id].z = z;
	t->g[id].depth = t->g[parent_sc].depth + 1;
	t->g[id].is_sc = is_sc;
	t->g[id].own_idx = -1;
	return id;
}

void po_set_own(po_table *t, int g, int idx)
{
	if (g > 0 && g < t->n && t->g[g].own_idx < 0)
		t->g[g].own_idx = idx;
}

/*
 * The appendix-E level of one CHILD of the common-ancestor context, or of an
 * item that belongs to the ancestor directly (l < 0).
 */
static int po_level(const po_table *t, int l, int idx, int anc)
{
	int z;
	if (l < 0)
		return (idx == t->g[anc].own_idx) ? 0 : 2;
	z = t->g[l].z;
	if (z < 0) return 1;
	if (z == 0) return 3;
	return 4;
}

int po_cmp(const po_table *t, int ga, int ia, int gb, int ib)
{
	if (ga == gb) {
		/*
		 * SAME GROUP. The appendix-E level rule has to be applied HERE TOO and
		 * not only across groups, or the comparator is NOT TRANSITIVE: with the
		 * context element's own box at item 9 and other content at item 3,
		 * own(9) < negz(5) by level and negz(5) < other(3) by level, while
		 * own(9) > other(3) by index. A fuzz over random trees found exactly
		 * that (see the selftest case). It is unreachable from layout.c, where
		 * the element's own box is the first item its group emits, but a
		 * comparator that is transitive only under an unstated invariant is one
		 * edit away from not being transitive at all.
		 */
		int lva = po_level(t, -1, ia, ga);
		int lvb = po_level(t, -1, ib, ga);
		if (lva != lvb) return lva < lvb ? -1 : 1;
		return ia < ib ? -1 : (ia > ib ? 1 : 0);
	}
	{
		int la = -1, lb = -1, anc, lva, lvb;
		/*
		 * Lift the deeper chain to the shallower one, remembering the child
		 * (la / lb) we came up THROUGH: that child is the paint unit whose rank
		 * decides. A chain that was never lifted keeps -1, which means the item
		 * belongs to the common ancestor context directly, i.e. it is in-flow
		 * content of that context (or the context element's own box).
		 *
		 * Only ONE of the two while loops can run, because they test opposite
		 * inequalities on the same pair of depths.
		 */
		while (t->g[ga].depth > t->g[gb].depth) { la = ga; ga = t->g[ga].parent; }
		while (t->g[gb].depth > t->g[ga].depth) { lb = gb; gb = t->g[gb].parent; }
		while (ga != gb) {
			la = ga; ga = t->g[ga].parent;
			lb = gb; gb = t->g[gb].parent;
		}
		anc = ga;
		lva = po_level(t, la, ia, anc);
		lvb = po_level(t, lb, ib, anc);
		if (lva != lvb) return lva < lvb ? -1 : 1;
		if (la >= 0 && lb >= 0) {
			int za = t->g[la].z, zb = t->g[lb].z;
			if (za != zb) return za < zb ? -1 : 1;
			/* Same level, same z: DOCUMENT ORDER of the two units. Group ids are
			 * allocated at element OPEN in a pre-order walk, so a lower id is
			 * strictly earlier in the document. That is why the tie-break is the
			 * group id and not the item index: a unit paints atomically, so an
			 * earlier unit's LAST item still paints below a later unit's FIRST. */
			if (la != lb) return la < lb ? -1 : 1;
		}
	}
	return ia < ib ? -1 : (ia > ib ? 1 : 0);
}

void po_sort(const po_table *t, po_key *k, po_key *scratch, int n)
{
	int width;
	po_key *src = k, *dst = scratch;
	if (n < 2) return;
	for (width = 1; width < n; width *= 2) {
		int i;
		for (i = 0; i < n; i += 2 * width) {
			int l = i, lend = i + width;
			int r = i + width, rend = i + 2 * width, o = i;
			if (lend > n) lend = n;
			if (r > n) r = n;
			if (rend > n) rend = n;
			while (l < lend && r < rend) {
				/* Take from the LEFT unless the right is STRICTLY earlier:
				 * that is what makes the sort stable. */
				if (po_cmp(t, src[r].pg, src[r].idx, src[l].pg, src[l].idx) < 0)
					dst[o++] = src[r++];
				else
					dst[o++] = src[l++];
			}
			while (l < lend) dst[o++] = src[l++];
			while (r < rend) dst[o++] = src[r++];
		}
		{ po_key *tmp = src; src = dst; dst = tmp; }
	}
	if (src != k) {
		int i;
		for (i = 0; i < n; i++) k[i] = src[i];
	}
}

/* ------------------------------------------------------------------ */
/* The self-test. Runs in the browser on every launch (see main.c).    */
/* ------------------------------------------------------------------ */

static char po_line[160];

static void po_say(void (*report)(const char *), int ok, const char *name)
{
	static const char *pass = "PASS: ";
	static const char *failw = "FAIL: ";
	const char *v = ok ? pass : failw;
	int i = 0, j;
	for (j = 0; v[j] && i < 150; j++) po_line[i++] = v[j];
	for (j = 0; name[j] && i < 150; j++) po_line[i++] = name[j];
	po_line[i] = 0;
	if (report) report(po_line);
}

static int po_chk(void (*report)(const char *), const char *name, int got, int want)
{
	int ok = (got == want);
	po_say(report, ok, name);
	return ok ? 0 : 1;
}

/*
 * Assert (ga,ia) paints BEFORE (gb,ib), and that the reverse comparison says
 * the opposite. One case, two assertions: a comparator that is not
 * antisymmetric makes the merge sort's output depend on the input permutation,
 * which is a bug that hides until a page happens to hit it.
 */
static int po_chk_before(void (*report)(const char *), const char *name,
		const po_table *t, int ga, int ia, int gb, int ib)
{
	int f = po_cmp(t, ga, ia, gb, ib);
	int r = po_cmp(t, gb, ib, ga, ia);
	int ok = (f < 0 && r > 0);
	po_say(report, ok, name);
	return ok ? 0 : 1;
}

int po_selftest(void (*report)(const char *line))
{
	int fail = 0;
	po_table t;

	/* ---- the promotion predicate ------------------------------------- */
	fail += po_chk(report, "static is NEVER a paint group",
			po_promote(PO_POS_STATIC, 0), PO_PROMOTE_NONE);
	fail += po_chk(report, "static with a z-index is still NOT a paint group",
			po_promote(PO_POS_STATIC, 1), PO_PROMOTE_NONE);
	fail += po_chk(report, "absolute with z-index:auto is an atomic UNIT",
			po_promote(PO_POS_ABSOLUTE, 0), PO_PROMOTE_UNIT);
	fail += po_chk(report, "absolute with an explicit z-index is a CONTEXT",
			po_promote(PO_POS_ABSOLUTE, 1), PO_PROMOTE_CTX);
	fail += po_chk(report, "relative with an explicit z-index is a CONTEXT",
			po_promote(PO_POS_RELATIVE, 1), PO_PROMOTE_CTX);
	fail += po_chk(report, "DEFERRED: relative with z-index:auto is NOT promoted",
			po_promote(PO_POS_RELATIVE, 0), PO_PROMOTE_NONE);

	/* ---- the INERT-AT-ZERO tripwires --------------------------------- */
	/*
	 * These two look tautological and are not: they are tripwires on a layout
	 * decision that costs a whole class of bug when it is got wrong. libcss
	 * puts CSS_POSITION_INHERIT at 0x0 and CSS_POSITION_STATIC at 0x1, so a raw
	 * libcss position in a zeroed struct reads as INHERIT and any test against
	 * it arms on EVERY page. paintorder.h owns its own constants precisely so
	 * the inert value sits at 0; anyone reordering them, or giving 0 to a new
	 * value, fails here on the next browser launch instead of shipping a path
	 * that runs on every page.
	 */
	fail += po_chk(report, "TRIPWIRE: PO_POS_STATIC is 0, so a zeroed position is the INERT one",
			PO_POS_STATIC, 0);
	fail += po_chk(report, "TRIPWIRE: PO_PROMOTE_NONE is 0, so a zeroed promotion is NOT promoted",
			PO_PROMOTE_NONE, 0);
	{
		/* The behavioural version of the same property: an item straight out of
		 * item_new()'s memset carries pg == 0, which must mean the ROOT context
		 * and must order by document position alone. */
		po_reset(&t);
		fail += po_chk(report, "a memset item (pg==0) belongs to the ROOT context and orders by document position",
				po_cmp(&t, 0, 3, 0, 9) < 0, 1);
	}

	/* ---- the z-index clamp ------------------------------------------- */
	fail += po_chk(report, "a plausible z-index passes the clamp unchanged",
			po_zclamp(100), 100);
	fail += po_chk(report, "a negative z-index passes the clamp unchanged",
			po_zclamp(-3), -3);
	fail += po_chk(report, "the clamp accepts exactly PO_Z_LIMIT",
			po_zclamp(PO_Z_LIMIT), PO_Z_LIMIT);
	fail += po_chk(report, "a z-index past the limit clamps high",
			po_zclamp(2147483647), PO_Z_LIMIT);
	fail += po_chk(report, "a z-index past the negative limit clamps low",
			po_zclamp(-2147483647), -PO_Z_LIMIT);
	/*
	 * THE UNIT CHECK. 1024 is what libcss's int32_t out-param really hands
	 * back for `z-index: 1`, because it stores a css_fixed. A caller that
	 * forgot to convert would reach the clamp with 1024, and this case says
	 * out loud that 1024 is a z-index of 1024 as far as this file is
	 * concerned, so the conversion belongs to the caller. It is the one thing
	 * a pure function CAN assert about a mistake it cannot see.
	 */
	fail += po_chk(report, "po_zclamp treats its argument as a PLAIN INTEGER (1024 is z-index 1024, not 1)",
			po_zclamp(1024), 1024);

	/* ---- an empty table is INERT ------------------------------------- */
	po_reset(&t);
	fail += po_chk(report, "a page with no positioned box has ONE group (the sort is skipped)",
			t.n, 1);
	fail += po_chk(report, "an item compared with itself is EQUAL",
			po_cmp(&t, 0, 5, 0, 5), 0);
	fail += po_chk_before(report, "two in-flow items order by document order",
			&t, 0, 5, 0, 9);

	/* ---- a positioned UNIT paints above in-flow content -------------- */
	{
		int u;
		po_reset(&t);
		u = po_group_new(&t, 0, 0, 0);
		fail += po_chk_before(report, "in-flow content paints BELOW a positioned unit",
				&t, 0, 3, u, 7);
		fail += po_chk_before(report,
				"in-flow content paints below a positioned unit even when it comes LATER in the document",
				&t, 0, 20, u, 7);
	}

	/* ---- negative z paints below in-flow content of the same context -- */
	{
		int n;
		po_reset(&t);
		n = po_group_new(&t, 0, -1, 1);
		fail += po_chk_before(report, "a z-index:-1 context paints BELOW the in-flow content of its context",
				&t, n, 7, 0, 3);
	}

	/* ---- THE NESTING FIX --------------------------------------------- */
	{
		int p, q, e;
		po_reset(&t);
		p = po_group_new(&t, 0, 1, 1);      /* parent, z-index:1 */
		q = po_group_new(&t, p, 100, 1);    /* its child, z-index:100 */
		e = po_group_new(&t, 0, 2, 1);      /* a sibling of the parent, z-index:2 */
		fail += po_chk_before(report,
				"NESTED: z-index:100 inside z-index:1 paints BELOW a z-index:2 sibling of the parent",
				&t, q, 7, e, 3);
		fail += po_chk_before(report, "NESTED: the parent context itself paints below the z-index:2 sibling",
				&t, p, 5, e, 3);
		fail += po_chk_before(report, "NESTED: a child context paints above its own parent's content",
				&t, p, 5, q, 7);
	}

	/* ---- level 0: the context element's own background ---------------- */
	{
		int s, n;
		po_reset(&t);
		s = po_group_new(&t, 0, 0, 1);
		po_set_own(&t, s, 5);
		n = po_group_new(&t, s, -1, 1);
		fail += po_chk_before(report,
				"the context element's OWN background paints below its z-index:-1 child",
				&t, s, 5, n, 9);
		fail += po_chk_before(report,
				"a z-index:-1 child paints below the context's other in-flow content",
				&t, n, 9, s, 7);
		fail += po_chk(report, "po_set_own does not overwrite an already-recorded own item",
				(po_set_own(&t, s, 42), t.g[s].own_idx), 5);
	}

	/* ---- step 8: z:auto units and z:0 contexts interleave by tree order */
	{
		int u1, c0, u2;
		po_reset(&t);
		u1 = po_group_new(&t, 0, 0, 0);
		c0 = po_group_new(&t, 0, 0, 1);
		u2 = po_group_new(&t, 0, 0, 0);
		fail += po_chk_before(report,
				"step 8: an earlier z:auto unit paints below a later z-index:0 context, whatever the item indices",
				&t, u1, 50, c0, 10);
		fail += po_chk_before(report, "step 8: a z-index:0 context paints below a later z:auto unit",
				&t, c0, 50, u2, 10);
	}

	/* ---- positive z ordering, and the tie-break ----------------------- */
	{
		int a, b;
		po_reset(&t);
		a = po_group_new(&t, 0, 3, 1);
		b = po_group_new(&t, 0, 1, 1);
		fail += po_chk_before(report, "z-index:1 paints below z-index:3 regardless of document order",
				&t, b, 10, a, 5);
	}
	{
		int a, b;
		po_reset(&t);
		a = po_group_new(&t, 0, 2, 1);
		b = po_group_new(&t, 0, 2, 1);
		fail += po_chk_before(report, "equal z-index breaks on DOCUMENT ORDER of the units, not item index",
				&t, a, 99, b, 1);
	}

	/* ---- a UNIT is a LEAF, which is what makes this case expressible -- */
	{
		int u, n;
		po_reset(&t);
		u = po_group_new(&t, 0, 0, 0);   /* position:absolute, z-index:auto */
		n = po_group_new(&t, 0, -1, 1);  /* a DESCENDANT of it with z-index:-1 */
		fail += po_chk_before(report,
				"a z-index:-1 descendant of a z:auto positioned box paints BELOW it (the unit is not a context)",
				&t, n, 20, u, 5);
	}

	/* ---- the float-bucket approximation, asserted POSITIVELY ---------- */
	{
		int u;
		po_reset(&t);
		u = po_group_new(&t, 0, 0, 0);
		fail += po_chk_before(report,
				"DEFERRED: in-flow items of one group order by document order (no float bucket)",
				&t, u, 3, u, 4);
	}

	/* ---- defined degradation at the caps ----------------------------- */
	{
		int i, last = 0;
		po_reset(&t);
		for (i = 1; i < PO_MAX_GROUPS; i++)
			last = po_group_new(&t, 0, 0, 1);
		fail += po_chk(report, "the group table fills to exactly PO_MAX_GROUPS",
				t.n, PO_MAX_GROUPS);
		fail += po_chk(report, "no overflow was reported while the table had room",
				t.overflowed, 0);
		fail += po_chk(report, "a group past the cap FOLDS INTO ITS PARENT rather than failing",
				po_group_new(&t, last, 5, 1), last);
		fail += po_chk(report, "the cap overflow is REPORTED", t.overflowed, 1);
	}
	{
		int i, g = 0, prev;
		po_reset(&t);
		for (i = 0; i < PO_MAX_DEPTH; i++)
			g = po_group_new(&t, g, 1, 1);
		fail += po_chk(report, "stacking contexts nest to exactly PO_MAX_DEPTH",
				t.g[g].depth, PO_MAX_DEPTH);
		fail += po_chk(report, "no overflow was reported while the depth had room",
				t.overflowed, 0);
		prev = g;
		fail += po_chk(report, "a context past the depth ceiling FOLDS INTO ITS PARENT",
				po_group_new(&t, g, 1, 1), prev);
		fail += po_chk(report, "the depth overflow is REPORTED", t.overflowed, 1);
	}

	/* ---- po_cmp is a STRICT TOTAL ORDER, checked on TRIPLES ---------- */
	/*
	 * Every case above is a PAIR, and no pair can assert TRANSITIVITY. That
	 * matters because po_sort is a merge sort and a merge sort's output is only
	 * well defined for a transitive comparator: without it the painted order
	 * would depend on the order the walk happened to emit items in.
	 *
	 * THIS CASE EXISTS BECAUSE IT FAILED. A host-side fuzz over random group
	 * trees found that the level rule was applied across groups but not within
	 * one, making own(9) < negz(5) < other(3) while own(9) > other(3). A
	 * deterministic, bounded version of that fuzz runs here so the property is
	 * asserted on the target and not only on a developer's host.
	 */
	{
		unsigned long rs = 88172645463325252UL;
		int trial, bad = 0;
		/*
		 * 128 TRIALS, AND THE NUMBER IS MEASURED. At 24 and at 64 this loop
		 * reported 0 failing against the intransitive comparator it exists to
		 * catch; at 128 it reports 1. The explicit case below is the gate for
		 * THAT bug; this loop's job is the shapes nobody thought of, and it can
		 * only do that job at a size where it has been seen to fail at all.
		 */
#define PO_RND(n) (rs = rs * 6364136223846793005UL + 1442695040888963407UL, \
                   (int) ((rs >> 33) % (unsigned long) (n)))
		for (trial = 0; trial < 128; trial++) {
			int pg[10], i, j, k, ng, ni = 10;
			po_reset(&t);
			ng = 1 + PO_RND(5);
			for (i = 0; i < ng; i++) {
				int par = 0, tries;
				for (tries = 0; tries < 6; tries++) {
					int c = PO_RND(t.n);
					if (t.g[c].is_sc) { par = c; break; }
				}
				{
					/* Two PO_RND in one argument list is an UNSEQUENCED
					 * modification of rs; gcc -Wsequence-point rejects it. */
					int rz = PO_RND(7) - 3;
					int rsc = PO_RND(4) != 0;
					po_group_new(&t, par, rz, rsc);
				}
			}
			for (i = 1; i < t.n; i++)
				if (PO_RND(2)) po_set_own(&t, i, PO_RND(ni));
			for (i = 0; i < ni; i++) pg[i] = PO_RND(t.n);
			for (i = 0; i < ni; i++) {
				if (po_cmp(&t, pg[i], i, pg[i], i) != 0) bad++;
				for (j = 0; j < ni; j++) {
					int ab = po_cmp(&t, pg[i], i, pg[j], j);
					int ba = po_cmp(&t, pg[j], j, pg[i], i);
					if (ab < 0 && ba <= 0) bad++;
					if (ab > 0 && ba >= 0) bad++;
					if (ab == 0 && i != j) bad++;
					if (ab >= 0) continue;
					for (k = 0; k < ni; k++)
						if (po_cmp(&t, pg[j], j, pg[k], k) < 0 &&
								po_cmp(&t, pg[i], i, pg[k], k) >= 0)
							bad++;
				}
			}
		}
#undef PO_RND
		fail += po_chk(report,
				"po_cmp is a STRICT TOTAL ORDER over random trees: irreflexive, antisymmetric, no ties between distinct items, TRANSITIVE",
				bad, 0);
	}

	/*
	 * THE MINIMAL REPRODUCER, explicit and deterministic, for the intransitivity
	 * the host fuzz found. The appendix-E level rule was applied when comparing
	 * across groups and not within one, so with the context element's own box at
	 * a HIGHER item index than its other content:
	 *
	 *     own(9) < negz(5)     level 0 < level 1
	 *     negz(5) < other(3)   level 1 < level 2
	 *     own(9) > other(3)    same group, straight to the index, 9 > 3
	 *
	 * layout.c cannot produce own_idx > another index in the same group, because
	 * the element's own box is the first item its group emits. This case is
	 * therefore about the COMPARATOR being a total order unconditionally, which
	 * is what po_sort's merge requires, not about a page that exists today.
	 */
	{
		int G, N;
		po_reset(&t);
		G = po_group_new(&t, 0, 0, 1);
		po_set_own(&t, G, 9);
		N = po_group_new(&t, G, -1, 1);
		fail += po_chk(report,
				"TRANSITIVE: own < negz and negz < other implies own < other, inside ONE group",
				(po_cmp(&t, G, 9, N, 5) < 0 && po_cmp(&t, N, 5, G, 3) < 0 &&
				 po_cmp(&t, G, 9, G, 3) < 0), 1);
	}

	/* ---- end to end through the sort, which also exercises the merge -- */
	{
		po_key k[5], scratch[5];
		int s, n, ok;
		po_reset(&t);
		s = po_group_new(&t, 0, 0, 1);
		po_set_own(&t, s, 5);
		n = po_group_new(&t, s, -1, 1);
		/* document order: root in-flow 0, the context's own box 5, its in-flow
		 * 7, its negative-z child 9, root in-flow 11 */
		k[0].pg = 0; k[0].idx = 0;
		k[1].pg = s; k[1].idx = 5;
		k[2].pg = s; k[2].idx = 7;
		k[3].pg = n; k[3].idx = 9;
		k[4].pg = 0; k[4].idx = 11;
		po_sort(&t, k, scratch, 5);
		ok = (k[0].idx == 0 && k[1].idx == 11 && k[2].idx == 5 &&
				k[3].idx == 9 && k[4].idx == 7);
		fail += po_chk(report,
				"po_sort: root in-flow, then the z:0 context's own box, its z:-1 child, then its in-flow",
				ok, 1);
	}
	{
		/* Stability: keys that compare EQUAL in group terms must keep document
		 * order. Nine in-flow items in the root, handed in reverse, must come
		 * back ascending, which only a correct comparator plus a correct merge
		 * can do. */
		po_key k[9], scratch[9];
		int i, ok = 1;
		po_reset(&t);
		for (i = 0; i < 9; i++) { k[i].pg = 0; k[i].idx = 9 - i; }
		po_sort(&t, k, scratch, 9);
		for (i = 0; i < 9; i++) if (k[i].idx != i + 1) ok = 0;
		fail += po_chk(report, "po_sort orders nine reversed in-flow items by document order",
				ok, 1);
	}

	return fail;
}
