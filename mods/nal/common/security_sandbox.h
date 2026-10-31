#ifndef MLB_SECURITY_SANDBOX_H
#define MLB_SECURITY_SANDBOX_H
/* Best-effort Linux child sandbox. Return bits for controls installed. */
enum { MLB_SANDBOX_SECCOMP = 1, MLB_SANDBOX_NAMESPACES = 2, MLB_SANDBOX_ERROR = 4 };
int mlb_sandbox_apply(void);
#endif
