#ifndef COMPAT_SOCK_H
#define COMPAT_SOCK_H

#include_next <net/sock.h>

/* Include the autogenerated header file */
#include "../../compat/config.h"

#ifndef HAVE_SK_WAIT_DATA_3_PARAMS
#define sk_wait_data(a,b,c) sk_wait_data(a,b)
#endif

#endif /* COMPAT_SOCK_H */
