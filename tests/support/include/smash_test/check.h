/*
 * check.h — minimal assertion helpers for the C/C++ native suites.
 * Header-only: safe in single-TU test binaries.
 *
 * SMASH_CHECK(cond)  — counted check; logs every failure, lets the suite
 *                      continue so one regression reports all symptoms.
 * SMASH_EXPECT(cond) — fatal check; aborts immediately when later checks
 *                      would read garbage (failed decode, OOB buffer).
 * SMASH_TEST_REPORT(name) — call at the end of main; returns 0/1 exit code.
 */
#ifndef SMASH_TEST_CHECK_H
#define SMASH_TEST_CHECK_H

#include <stdio.h>
#include <stdlib.h>

static int smash_test_checks = 0;
static int smash_test_failures = 0;

#define SMASH_CHECK(cond) \
	do { \
		++smash_test_checks; \
		if (!(cond)) { \
			++smash_test_failures; \
			fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); \
		} \
	} while (0)

#define SMASH_EXPECT(cond) \
	do { \
		++smash_test_checks; \
		if (!(cond)) { \
			fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); \
			exit(1); \
		} \
	} while (0)

static int smash_test_report(const char *name)
{
	if (smash_test_failures != 0) {
		fprintf(stderr, "%s: %d/%d checks FAILED\n", name, smash_test_failures, smash_test_checks);
		return 1;
	}
	printf("%s: %d checks passed\n", name, smash_test_checks);
	return 0;
}

#endif /* SMASH_TEST_CHECK_H */
