// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors. Part of the native office suite (officelib).
// officeui_priv.h - PRIVATE hooks between the shell (officeui.c) and the
// toolkit's blocking modals (omodal.c). Not part of the app-facing API.
#ifndef OFFICE_OFFICEUI_PRIV_H
#define OFFICE_OFFICEUI_PRIV_H

#include "../include/officeui.h"

// A nested (modal) event loop saw EVENT_RESIZE: tell the shell the window's
// new content size so oapp_size()/oapp_content_rect() and the next
// oapp_repaint() agree with the compositor.
void oapp__note_resize(office_app *a, int w, int h);

#endif // OFFICE_OFFICEUI_PRIV_H
