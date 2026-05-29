// Comment out this line when using as DLL
#define examples_c_pkg_amalgamate_disable_STATIC
#ifndef EXAMPLES_C_PKG_AMALGAMATE_DISABLE_H
#define EXAMPLES_C_PKG_AMALGAMATE_DISABLE_H

#ifndef EXAMPLES_C_PKG_AMALGAMATE_DISABLE_CONSTANTS_H
#define EXAMPLES_C_PKG_AMALGAMATE_DISABLE_CONSTANTS_H

#define EXAMPLES_C_PKG_AMALGAMATE_DISABLE_BIAS (10)

#endif

#ifndef EXAMPLES_CUSTOM_BUILD
#define EXAMPLES_DEBUG_INFO
#endif

#define EXAMPLES_C_PKG_AMALGAMATE_DISABLE_MODE (2)

int examples_c_pkg_amalgamate_disable_kept_decl(void);

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

