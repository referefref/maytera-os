// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// langinfo.c - see langinfo.h for why this exists and why "UTF-8" is honest.

#include "langinfo.h"

char *nl_langinfo(nl_item item) {
    (void)item;
    static char codeset[] = "UTF-8";
    return codeset;
}
