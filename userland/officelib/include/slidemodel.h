// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors. Part of the native office suite (officelib).
// See docs/OFFICE_SUITE_ARCHITECTURE.md for the module map and contracts.
// NOTE: officelib headers are SELF-SUFFICIENT (raw C types only). Do NOT add
//       #include "types.h" or any libc header here: libc's bool/off_t/pid_t
//       leak into apps carrying legacy compat typedefs and break the userland.
#ifndef OFFICE_SLIDEMODEL_H
#define OFFICE_SLIDEMODEL_H
// Presentation model shared by Slides + the pptx/odp/.ppt importers.
// Owner: agent 3 (model/slidemodel.c). Reuses doc_para for shape text.
#include "docmodel.h"

typedef enum { SHP_TEXTBOX=0, SHP_IMAGE, SHP_RECT, SHP_TITLE } shape_type;

typedef struct shape {
    shape_type type;
    int        x, y, w, h;      // EMU/px per presentation.units
    doc_para  *paras; int npara; // SHP_TEXTBOX / SHP_TITLE
    doc_image *image;           // SHP_IMAGE
    unsigned int fill;          // SHP_RECT 0xRRGGBB
} shape;

typedef struct slide { char *title; shape *shapes; int nshape; } slide;

typedef struct presentation {
    slide *slides; int nslide;
    int    cx, cy;              // slide size (px); 0 = default 960x720
} presentation;

presentation *pres_new(void);
void          pres_free(presentation *p);
slide        *pres_add_slide(presentation *p);
shape        *slide_add_shape(slide *s, shape_type t);
#endif
