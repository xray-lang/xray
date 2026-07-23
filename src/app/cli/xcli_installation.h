/*
 * xcli_installation.h - Installed payload discovery and reporting.
 */

#ifndef XCLI_INSTALLATION_H
#define XCLI_INSTALLATION_H

#include "../../base/xdefs.h"
#include <stdbool.h>

/* Print the installation identity used by installer/update delegation. */
XR_FUNC int xr_cli_print_installation_info(bool json);

#endif  // XCLI_INSTALLATION_H
