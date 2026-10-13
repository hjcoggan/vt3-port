#ifndef VT3_SELFTEST_H
#define VT3_SELFTEST_H

/* Runs the unit checks, and the ADX check when adx_path is non-NULL. Returns 0, 1, or 77. */
int vt3_selftest_run(const char *adx_path);

#endif /* VT3_SELFTEST_H */
