#ifndef _LINUX_REFCOUNT_H
#define _LINUX_REFCOUNT_H

#include <linux/atomic.h>
#include <linux/compiler.h>
#include <linux/kernel.h>

static inline unsigned int refcount_read(const atomic_t *r)
{
	return atomic_read(r);
}

static inline void refcount_inc(atomic_t *r)
{
	atomic_inc(r);
}

static bool refcount_inc_not_zero(atomic_t *r)
{
	unsigned int old, new, val = atomic_read(r);

	for (;;) {
		new = val + 1;

		if (!val)
			return false;

		if (unlikely(!new))
			return true;

		old = atomic_cmpxchg(r, val, new);
		if (old == val)
			break;

		val = old;
	}

	WARN(new == UINT_MAX, "refcount_t: saturated; leaking memory.\n");

	return true;
}

static inline bool
refcount_dec_and_test(atomic_t *r)
{

	return atomic_dec_and_test(r);
}

static inline void refcount_dec(atomic_t *r)
{
	atomic_dec(r);
}

static bool refcount_dec_not_one(atomic_t *r)
{
	unsigned int old, new, val = atomic_read(r);

	for (;;) {
		if (unlikely(val == UINT_MAX))
			return true;

		if (val == 1)
			return false;

		new = val - 1;
		if (new > val) {
			WARN(new > val, "refcount_t: underflow; use-after-free.\n");
			return true;
		}

		old = atomic_cmpxchg(r, val, new);
		if (old == val)
			break;

		val = old;
	}

	return true;
}

#endif /* _LINUX_REFCOUNT_H */
