// Version and copyright, shared by resources/resource.rc and the About
// page. Preprocessor definitions only: the resource compiler reads this
// file too.

#pragma once

#define KEYVOK_VERSION_MAJOR 2
#define KEYVOK_VERSION_MINOR 0
#define KEYVOK_VERSION_PATCH 0

#define KEYVOK_STRINGIFY_(x) #x
#define KEYVOK_STRINGIFY(x) KEYVOK_STRINGIFY_(x)

// "MAJOR.MINOR.PATCH"
#define KEYVOK_VERSION_STRING                                                  \
  KEYVOK_STRINGIFY(KEYVOK_VERSION_MAJOR)                                       \
  "." KEYVOK_STRINGIFY(KEYVOK_VERSION_MINOR) "." KEYVOK_STRINGIFY(             \
      KEYVOK_VERSION_PATCH)

#define KEYVOK_COPYRIGHT_YEAR "2026"
#define KEYVOK_COMPANY "Radiant Software LLC"
