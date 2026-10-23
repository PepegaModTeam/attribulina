#pragma once
#include "build_number.h"

#define APP_MAKE_STR(x) #x
#define APP_MAKE_STR_EXPAND(x) APP_MAKE_STR(x)

#define APP_NAME "Attribulina"
#define APP_BRANCH BRANCH_NAME
#define APP_VERSION     VERSION_MAJOR.VERSION_MINOR.VERSION_REVISION
#define APP_BASIC_VERSION_STR APP_MAKE_STR_EXPAND(APP_VERSION)
#define APP_VERSION_STR APP_BASIC_VERSION_STR " (" COMMIT_ID ")" " (" __DATE__ ", " __TIME__ ")"
#define APP_VERSION_STR_WNAME APP_NAME "-" APP_BRANCH " v" APP_VERSION_STR

#ifdef _DEBUG
inline constexpr const char* AppVersionString = APP_VERSION_STR_WNAME " DEBUG";
#else
inline constexpr const char* AppVersionString = APP_VERSION_STR_WNAME;
#endif
