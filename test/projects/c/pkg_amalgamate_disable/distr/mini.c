#include "mini.h"

#if defined(EXAMPLES_DEBUG_INFO) && defined(EXAMPLES_FLAG_OFF)
int examples_c_pkg_amalgamate_disable_use_flag(void) {
    return examples_c_pkg_amalgamate_disable_flag_only();
}
#endif

int examples_c_pkg_amalgamate_disable_kept_decl(void) {
    return EXAMPLES_C_PKG_AMALGAMATE_DISABLE_BIAS;
}

int examples_c_pkg_amalgamate_disable_macro_use(void) {
    EXAMPLES_C_PKG_AMALGAMATE_DISABLE_WRAP(3);
    return EXAMPLES_C_PKG_AMALGAMATE_DISABLE_INC(4);
}

int examples_c_pkg_amalgamate_disable_value(void) {
    return EXAMPLES_C_PKG_AMALGAMATE_DISABLE_MODE +
        EXAMPLES_C_PKG_AMALGAMATE_DISABLE_BIAS;
}

