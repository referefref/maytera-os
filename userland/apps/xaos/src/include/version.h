/* version.h - MayteraOS port (#745). Replaces the configure-substituted
 * version.h.in (PACKAGE_VERSION comes from autoconf, not present here). Pinned
 * to the upstream tag this port is built from; see userland/apps/xaos/README.md
 * and ATTRIBUTION.md. */
#ifndef XAOS_VERSION_H
#define XAOS_VERSION_H
#define PACKAGE_VERSION "3.6"
#define XaoS_VERSION PACKAGE_VERSION
#endif
