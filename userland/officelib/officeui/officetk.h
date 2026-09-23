// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors. Part of the native office suite (officelib).
// officetk.h - umbrella header for the shared office UI toolkit
// (docs/OFFICE_UI_DESIGN.md section 8). Include AFTER gui.h and gui_menu.h:
//
//     #include "../../libc/gui.h"
//     #include "../../libc/gui_menu.h"
//     #include "../../officelib/include/officeui.h"
//     #include "../../officelib/officeui/officetk.h"
//
// The widget headers embed libc widget types (gui_list_t, textfield_t), which
// is why they live here beside their .c files and not under officelib/include/
// (whose headers stay raw-C-type self-sufficient). See officeui/example/ for
// a complete app that uses every widget.
#ifndef OFFICE_OFFICETK_H
#define OFFICE_OFFICETK_H

#include "oicon.h"
#include "ocombo.h"
#include "otoolbar.h"
#include "omodal.h"
#include "otabs.h"
#include "othumbs.h"
#include "oruler.h"
#include "ofxbar.h"

#endif // OFFICE_OFFICETK_H
