/*
 * fontmap.c - CSS font-family -> installed TTF face. See fontmap.h.
 *
 * No allocation, no I/O, no libm. The table is built once from the registry's
 * family + subfamily strings and then answered from memory.
 */
#include "fontmap.h"

#define FM_MAX_FACES 96
#define FM_NAME_MAX  48

typedef struct {
	char fam[FM_NAME_MAX];   /* lowercased family, e.g. "ibm plex mono" */
	int  idx;                /* registry face index */
	int  bold;               /* subfamily says bold/black/heavy */
	int  italic;             /* subfamily says italic/oblique */
	int  weight;             /* 100..900, parsed from the subfamily */
} fm_face;

static fm_face g_faces[FM_MAX_FACES];
static int g_n;
static int g_ready;
static int g_face0_fam = -1;   /* index into g_faces of the system UI face */

int fontmap_face_count(void) { return g_n; }

/* ---- tiny string helpers (freestanding; no libc string.h dependency) ---- */

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int fm_len(const char *s) { int n = 0; while (s[n]) n++; return n; }

static int fm_eq(const char *a, const char *b)
{
	int i = 0;
	while (a[i] && b[i]) { if (a[i] != b[i]) return 0; i++; }
	return a[i] == 0 && b[i] == 0;
}

/* Does haystack contain needle? Both already lowercase. */
static int fm_has(const char *h, const char *n)
{
	int i, j;
	for (i = 0; h[i]; i++) {
		for (j = 0; n[j] && h[i + j] == n[j]; j++) { }
		if (n[j] == 0) return 1;
	}
	return 0;
}

/*
 * Copy `in` into `out` lowercased, with runs of whitespace collapsed to one
 * space and the ends trimmed. CSS writes family names with arbitrary spacing
 * and quoting; the registry writes them as the font's own name-table string.
 * Normalising both through this is what lets "IBM  Plex Mono" match.
 */
static void fm_norm(const char *in, char *out, int cap)
{
	int o = 0, i = 0, sp = 0;
	if (cap <= 0) return;
	while (in[i] == ' ' || in[i] == '\t' || in[i] == '"' || in[i] == '\'') i++;
	for (; in[i] && o < cap - 1; i++) {
		char c = in[i];
		if (c == '"' || c == '\'') continue;
		if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { sp = 1; continue; }
		if (sp && o > 0) { out[o++] = ' '; if (o >= cap - 1) break; }
		sp = 0;
		out[o++] = lc(c);
	}
	out[o] = 0;
}

/*
 * Weight from a subfamily string. The registry hands back what the font itself
 * calls the style ("Regular", "Book", "Semibold Italic", "Bold Oblique",
 * "Black", "Thin", "Light", "Medium"), which is a richer signal than a bold
 * flag: MayteraOS ships FOUR weights of Lato and Source Code Pro, and picking
 * "Thin" for an unstyled paragraph because it happened to be enumerated first
 * would be a very visible wrong answer.
 */
static int fm_weight_of(const char *style_lc)
{
	if (fm_has(style_lc, "black") || fm_has(style_lc, "heavy")) return 900;
	if (fm_has(style_lc, "extrabold") || fm_has(style_lc, "ultrabold")) return 800;
	if (fm_has(style_lc, "semibold") || fm_has(style_lc, "demibold")) return 600;
	if (fm_has(style_lc, "bold")) return 700;
	if (fm_has(style_lc, "medium")) return 500;
	if (fm_has(style_lc, "light")) return 300;
	if (fm_has(style_lc, "thin") || fm_has(style_lc, "hairline")) return 100;
	return 400;   /* Regular, Book, Roman, Italic, Oblique, or unnamed */
}

void fontmap_init(void)
{
	int n, i;
	if (g_ready) return;
	g_ready = 1;
	g_n = 0;
	n = fm_plat_font_count();
	if (n > FM_MAX_FACES) n = FM_MAX_FACES;
	for (i = 0; i < n; i++) {
		char raw[FM_NAME_MAX], sraw[FM_NAME_MAX], sl[FM_NAME_MAX];
		fm_face *f;
		raw[0] = 0; sraw[0] = 0;
		/* A zero-length name is a removed slot, not the end of the list:
		 * indices are stable and may contain holes. Skip, do not stop. */
		if (fm_plat_font_name(i, raw, (int) sizeof raw) <= 0) continue;
		fm_plat_font_style(i, sraw, (int) sizeof sraw);
		f = &g_faces[g_n];
		fm_norm(raw, f->fam, FM_NAME_MAX);
		if (f->fam[0] == 0) continue;
		fm_norm(sraw, sl, FM_NAME_MAX);
		f->idx = i;
		f->weight = fm_weight_of(sl);
		f->bold = f->weight >= 600;
		f->italic = (fm_has(sl, "italic") || fm_has(sl, "oblique")) ? 1 : 0;
		if (i == 0) g_face0_fam = g_n;
		g_n++;
	}
}

/*
 * Score a candidate against what was asked for. Lower is better.
 *
 * Weight dominates: drawing body prose in Black because Black was the only
 * face whose italic flag matched is far worse than drawing it upright. Italic
 * mismatch is next, and is recoverable by the rasteriser's shear. The residual
 * is the distance in weight, so a request for bold lands on Bold (700) ahead of
 * Semibold (600) ahead of Black (900).
 */
static int fm_score(const fm_face *f, int want_w, int want_i)
{
	int d = f->weight - want_w;
	if (d < 0) d = -d;
	return d + (f->italic != want_i ? 2000 : 0);
}

static int fm_pick_in_family(const char *fam_lc, int bold, int italic,
		int *face, int *style)
{
	int want_w = bold ? 700 : 400;
	int best = -1, best_s = 0, i;
	for (i = 0; i < g_n; i++) {
		int s;
		if (!fm_eq(g_faces[i].fam, fam_lc)) continue;
		s = fm_score(&g_faces[i], want_w, italic);
		if (best < 0 || s < best_s) { best = i; best_s = s; }
	}
	if (best < 0) return 0;
	*face = g_faces[best].idx;
	*style = FM_STYLE_NORMAL;
	/* Only ask for SYNTHETIC weight/slant the family could not supply.
	 * Emboldening an already-bold face draws mud, and the faked advance
	 * would then disagree with nothing, because measure and draw share it. */
	if (bold && !g_faces[best].bold) *style |= FM_STYLE_BOLD;
	if (italic && !g_faces[best].italic) *style |= FM_STYLE_ITALIC;
	return 1;
}

/*
 * Web-safe names that are NOT installed families but which every real
 * stylesheet lists, mapped to the generic they stand for. maytera.net's own
 * --mono is `"IBM Plex Mono", ui-monospace, "SF Mono", Menlo, Consolas,
 * monospace`: without this table the first four entries all miss and the
 * fallthrough is fine, but a sheet that omits the final generic keyword (very
 * common) would get the default sans for its code blocks.
 */
static int fm_alias_generic(const char *n)
{
	if (fm_eq(n, "ui-monospace") || fm_eq(n, "sf mono") || fm_eq(n, "menlo") ||
			fm_eq(n, "consolas") || fm_eq(n, "monaco") ||
			fm_eq(n, "courier") || fm_eq(n, "courier new") ||
			fm_eq(n, "liberation mono") || fm_eq(n, "dejavu sans mono") ||
			fm_eq(n, "andale mono") || fm_eq(n, "lucida console") ||
			fm_eq(n, "cascadia code") || fm_eq(n, "cascadia mono") ||
			fm_eq(n, "roboto mono") || fm_eq(n, "fira code") ||
			fm_eq(n, "fira mono") || fm_eq(n, "jetbrains mono") ||
			fm_eq(n, "monospace"))
		return FM_GENERIC_MONOSPACE;
	if (fm_eq(n, "ui-serif") || fm_eq(n, "times") || fm_eq(n, "times new roman") ||
			fm_eq(n, "georgia") || fm_eq(n, "garamond") ||
			fm_eq(n, "palatino") || fm_eq(n, "cambria") ||
			fm_eq(n, "book antiqua") || fm_eq(n, "serif"))
		return FM_GENERIC_SERIF;
	if (fm_eq(n, "system-ui") || fm_eq(n, "ui-sans-serif") ||
			fm_eq(n, "-apple-system") || fm_eq(n, "blinkmacsystemfont") ||
			fm_eq(n, "segoe ui") || fm_eq(n, "helvetica") ||
			fm_eq(n, "helvetica neue") || fm_eq(n, "arial") ||
			fm_eq(n, "roboto") || fm_eq(n, "verdana") ||
			fm_eq(n, "tahoma") || fm_eq(n, "ubuntu") ||
			fm_eq(n, "cantarell") || fm_eq(n, "noto sans ui") ||
			fm_eq(n, "sans-serif"))
		return FM_GENERIC_SANS;
	return FM_GENERIC_NONE;
}

/*
 * Preference order per generic, restricted to families MayteraOS actually
 * ships. The sans list starts with whatever face 0 is (the system UI font), so
 * a page's body text matches the rest of the desktop rather than picking a
 * second sans at random.
 */
static const char *FM_MONO[] = {
	"ibm plex mono", "dejavu sans mono", "source code pro",
	"inconsolata sugar", 0
};
static const char *FM_SERIF[] = {
	"noto serif", "source serif pro", "dejavu serif", "linus libertinus", 0
};
static const char *FM_SANS[] = {
	"noto sans", "lato", "source sans pro", "dejavu sans", "catamaran", 0
};

int fontmap_generic(int generic, int bold, int italic, int *face, int *style)
{
	const char **list;
	int i;

	fontmap_init();
	switch (generic) {
	case FM_GENERIC_MONOSPACE: list = FM_MONO;  break;
	case FM_GENERIC_SERIF:     list = FM_SERIF; break;
	default:                   list = FM_SANS;  break;
	}
	/* Sans (and the cursive/fantasy that fall through to it) prefer the
	 * system UI face, so page body text and desktop chrome agree. */
	if (list == FM_SANS && g_face0_fam >= 0 &&
			fm_pick_in_family(g_faces[g_face0_fam].fam, bold, italic, face, style))
		return 1;
	for (i = 0; list[i]; i++)
		if (fm_pick_in_family(list[i], bold, italic, face, style)) return 1;

	/* Nothing matched: face 0 with fully synthetic styling. Never fails. */
	*face = 0;
	*style = (bold ? FM_STYLE_BOLD : 0) | (italic ? FM_STYLE_ITALIC : 0);
	return 1;
}

int fontmap_synth_for(int face, int bold, int italic)
{
	int i, st = 0;
	fontmap_init();
	for (i = 0; i < g_n; i++) {
		if (g_faces[i].idx != face) continue;
		if (bold && !g_faces[i].bold) st |= FM_STYLE_BOLD;
		if (italic && !g_faces[i].italic) st |= FM_STYLE_ITALIC;
		return st;
	}
	return (bold ? FM_STYLE_BOLD : 0) | (italic ? FM_STYLE_ITALIC : 0);
}

int fontmap_family(const char *family, int bold, int italic, int *face, int *style)
{
	char n[FM_NAME_MAX];
	int gen;

	fontmap_init();
	if (!family || !family[0]) return 0;
	fm_norm(family, n, FM_NAME_MAX);
	if (!n[0]) return 0;
	if (fm_pick_in_family(n, bold, italic, face, style)) return 1;
	gen = fm_alias_generic(n);
	if (gen != FM_GENERIC_NONE) return fontmap_generic(gen, bold, italic, face, style);
	(void) fm_len;
	return 0;
}
