/*
 * env.h — portable environment mutation for native tests.
 * POSIX setenv/unsetenv wrapped for MSVC (_putenv_s). Tests exercising the
 * SSB64_* hooks (docs/development/env_vars.md) use these instead of
 * #ifdef-ing on the platform.
 */
#ifndef SMASH_TEST_ENV_H
#define SMASH_TEST_ENV_H

#ifdef __cplusplus
extern "C" {
#endif

int smash_test_setenv(const char *name, const char *value);
int smash_test_unsetenv(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* SMASH_TEST_ENV_H */
