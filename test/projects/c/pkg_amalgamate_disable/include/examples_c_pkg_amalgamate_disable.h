#ifndef EXAMPLES_C_PKG_AMALGAMATE_DISABLE_H
#define EXAMPLES_C_PKG_AMALGAMATE_DISABLE_H

/**
 * @file examples_c_pkg_amalgamate_disable.h
 * @brief Header used to verify amalgamation cleanup.
 */

#include "detail/constants.h"

#ifndef EXAMPLES_CUSTOM_BUILD
#define EXAMPLES_FLAG_OFF
#define EXAMPLES_DEBUG_INFO
#endif

#ifdef EXAMPLES_FEATURE_REMOVED
int examples_c_pkg_amalgamate_disable_removed_decl(void);
#endif

#ifdef EXAMPLES_FLAG_OFF
int examples_c_pkg_amalgamate_disable_flag_only(void);
#endif

#if defined(EXAMPLES_FLAG_OFF)
#define EXAMPLES_C_PKG_AMALGAMATE_DISABLE_MODE (1)
#else
#define EXAMPLES_C_PKG_AMALGAMATE_DISABLE_MODE (2)
#endif

#ifndef EXAMPLES_FEATURE_REMOVED
int examples_c_pkg_amalgamate_disable_kept_decl(void);
#endif

#ifdef EXAMPLES_KEEP_THIS
int examples_c_pkg_amalgamate_disable_other_decl(void);
#endif

#define EXAMPLES_C_PKG_AMALGAMATE_DISABLE_WRAP(x)\
    {\
        (void)(x);\
    }\

#define EXAMPLES_C_PKG_AMALGAMATE_DISABLE_INC(x) ((x) + 1)

int examples_c_pkg_amalgamate_disable_value(void);

#endif
