#include "smash_test/env.h"

#include <stdlib.h>

#ifdef _WIN32
int smash_test_setenv(const char *name, const char *value)
{
	return _putenv_s(name, value);
}

int smash_test_unsetenv(const char *name)
{
	/* Empty value unsets on Windows (_putenv_s deletes the entry). */
	return _putenv_s(name, "");
}
#else
int smash_test_setenv(const char *name, const char *value)
{
	return setenv(name, value, 1);
}

int smash_test_unsetenv(const char *name)
{
	return unsetenv(name);
}
#endif
