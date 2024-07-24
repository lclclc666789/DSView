#include <string.h>

#ifdef _WIN32
    #include <ctype.h>
    #define strcasecmp _stricmp
#else
    #include <strings.h>
#endif