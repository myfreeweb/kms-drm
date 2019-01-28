/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2017-2018 Intel Corporation
 */

#include "../i915_selftest.h"
#include "i915_random.h"

#include "igt_flush_test.h"
#include "mock_gem_device.h"
#include "mock_timeline.h"

struct __igt_sync {
	const char *name;
	u32 seqno;
	bool expected;
	bool set;
};

static int __igt_sync(struct i915_timeline *tl,
		      u64 ctx,
		      const struct __igt_sync *p,
		      const char *name)
{
	int ret;

	if (__i915_timeline_sync_is_later(tl, ctx, p->seqno) != p->expected) {
		pr_err("%s: %s(ctx=%llu, seqno=%u) expected passed %s but failed\n",
		       name, p->name, ctx, p->seqno, yesno(p->expected));
		return -EINVAL;
	}

	if (p->set) {
		ret = __i915_timeline_sync_set(tl, ctx, p->seqno);
		if (ret)
			return ret;
	}

	return 0;
}

static int igt_sync(void *arg)
{
	const struct __igt_sync pass[] = {
		{ "unset", 0, false, false },
		{ "new", 0, false, true },
		{ "0a", 0, true, true },
		{ "1a", 1, false, true },
		{ "1b", 1, true, true },
		{ "0b", 0, true, false },
		{ "2a", 2, false, true },
		{ "4", 4, false, true },
		{ "INT_MAX", INT_MAX, false, true },
		{ "INT_MAX-1", INT_MAX-1, true, false },
		{ "INT_MAX+1", (u32)INT_MAX+1, false, true },
		{ "INT_MAX", INT_MAX, true, false },
		{ "UINT_MAX", UINT_MAX, false, true },
		{ "wrap", 0, false, true },
		{ "unwrap", UINT_MAX, true, false },
		{},
	}, *p;
	struct i915_timeline tl;
	int order, offset;
	int ret = -ENODEV;

	mock_timeline_init(&tl, 0);
	for (p = pass; p->name; p++) {
		for (order = 1; order < 64; order++) {
			for (offset = -1; offset <= (order > 1); offset++) {
				u64 ctx = BIT_ULL(order) + offset;

				ret = __igt_sync(&tl, ctx, p, "1");
				if (ret)
					goto out;
			}
		}
	}
	mock_timeline_fini(&tl);

	mock_timeline_init(&tl, 0);
	for (order = 1; order < 64; order++) {
		for (offset = -1; offset <= (order > 1); offset++) {
			u64 ctx = BIT_ULL(order) + offset;

			for (p = pass; p->name; p++) {
				ret = __igt_sync(&tl, ctx, p, "2");
				if (ret)
					goto out;
			}
		}
	}

out:
	mock_timeline_fini(&tl);
	return ret;
}

static unsigned int random_engine(struct rnd_state *rnd)
{
	return i915_prandom_u32_max_state(I915_NUM_ENGINES, rnd);
}

static int bench_sync(void *arg)
{
	struct rnd_state prng;
	struct i915_timeline tl;
	unsigned long end_time, count;
	u64 prng32_1M;
	ktime_t kt;
	int order, last_order;

	mock_timeline_init(&tl, 0);

	/* Lookups from cache are very fast and so the random number generation
	 * and the loop itself becomes a significant factor in the per-iteration
	 * timings. We try to compensate the results by measuring the overhead
	 * of the prng and subtract it from the reported results.
	 */
	prandom_seed_state(&prng, i915_selftest.random_seed);
	count = 0;
	kt = ktime_get();
	end_time = jiffies + HZ/10;
	do {
		u32 x;

		/* Make sure the compiler doesn't optimise away the prng call */
		WRITE_ONCE(x, prandom_u32_state(&prng));

		count++;
	} while (!time_after(jiffies, end_time));
	kt = ktime_sub(ktime_get(), kt);
	pr_debug("%s: %lu random evaluations, %lluns/prng\n",
		 __func__, count, (long long)div64_ul(ktime_to_ns(kt), count));
	prng32_1M = div64_ul(ktime_to_ns(kt) << 20, count);

	/* Benchmark (only) setting random context ids */
	prandom_seed_state(&prng, i915_selftest.random_seed);
	count = 0;
	kt = ktime_get();
	end_time = jiffies + HZ/10;
	do {
		u64 id = i915_prandom_u64_state(&prng);

		__i915_timeline_sync_set(&tl, id, 0);
		count++;
	} while (!time_after(jiffies, end_time));
	kt = ktime_sub(ktime_get(), kt);
	kt = ktime_sub_ns(kt, (count * prng32_1M * 2) >> 20);
	pr_info("%s: %lu random insertions, %lluns/insert\n",
		__func__, count, (long long)div64_ul(ktime_to_ns(kt), count));

	/* Benchmark looking up the exact same context ids as we just set */
	prandom_seed_state(&prng, i915_selftest.random_seed);
	end_time = count;
	kt = ktime_get();
	while (end_time--) {
		u64 id = i915_prandom_u64_state(&prng);

		if (!__i915_timeline_sync_is_later(&tl, id, 0)) {
			mock_timeline_fini(&tl);
			pr_err("Lookup of %llu failed\n", id);
			return -EINVAL;
		}
	}
	kt = ktime_sub(ktime_get(), kt);
	kt = ktime_sub_ns(kt, (count * prng32_1M * 2) >> 20);
	pr_info("%s: %lu random lookups, %lluns/lookup\n",
		__func__, count, (long long)div64_ul(ktime_to_ns(kt), count));

	mock_timeline_fini(&tl);
	cond_resched();

	mock_timeline_init(&tl, 0);

	/* Benchmark setting the first N (in order) contexts */
	count = 0;
	kt = ktime_get();
	end_time = jiffies + HZ/10;
	do {
		__i915_timeline_sync_set(&tl, count++, 0);
	} while (!time_after(jiffies, end_time));
	kt = ktime_sub(ktime_get(), kt);
	pr_info("%s: %lu in-order insertions, %lluns/insert\n",
		__func__, count, (long long)div64_ul(ktime_to_ns(kt), count));

	/* Benchmark looking up the exact same context ids as we just set */
	end_time = count;
	kt = ktime_get();
	while (end_time--) {
		if (!__i915_timeline_sync_is_later(&tl, end_time, 0)) {
			pr_err("Lookup of %lu failed\n", end_time);
			mock_timeline_fini(&tl);
			return -EINVAL;
		}
	}
	kt = ktime_sub(ktime_get(), kt);
	pr_info("%s: %lu in-order lookups, %lluns/lookup\n",
		__func__, count, (long long)div64_ul(ktime_to_ns(kt), count));

	mock_timeline_fini(&tl);
	cond_resched();

	mock_timeline_init(&tl, 0);

	/* Benchmark searching for a random context id and maybe changing it */
	prandom_seed_state(&prng, i915_selftest.random_seed);
	count = 0;
	kt = ktime_get();
	end_time = jiffies + HZ/10;
	do {
		u32 id = random_engine(&prng);
		u32 seqno = prandom_u32_state(&prng);

		if (!__i915_timeline_sync_is_later(&tl, id, seqno))
			__i915_timeline_sync_set(&tl, id, seqno);

		count++;
	} while (!time_after(jiffies, end_time));
	kt = ktime_sub(ktime_get(), kt);
	kt = ktime_sub_ns(kt, (count * prng32_1M * 2) >> 20);
	pr_info("%s: %lu repeated insert/lookups, %lluns/op\n",
		__func__, count, (long long)div64_ul(ktime_to_ns(kt), count));
	mock_timeline_fini(&tl);
	cond_resched();

	/* Benchmark searching for a known context id and changing the seqno */
	for (last_order = 1, order = 1; order < 32;
	     ({ int tmp = last_order; last_order = order; order += tmp; })) {
		unsigned int mask = BIT(order) - 1;

		mock_timeline_init(&tl, 0);

		count = 0;
		kt = ktime_get();
		end_time = jiffies + HZ/10;
		do {
			/* Without assuming too many details of the underlying
			 * implementation, try to identify its phase-changes
			 * (if any)!
			 */
			u64 id = (u64)(count & mask) << order;

			__i915_timeline_sync_is_later(&tl, id, 0);
			__i915_timeline_sync_set(&tl, id, 0);

			count++;
		} while (!time_after(jiffies, end_time));
		kt = ktime_sub(ktime_get(), kt);
		pr_info("%s: %lu cyclic/%d insert/lookups, %lluns/op\n",
			__func__, count, order,
			(long long)div64_ul(ktime_to_ns(kt), count));
		mock_timeline_fini(&tl);
		cond_resched();
	}

	return 0;
}

int i915_timeline_mock_selftests(void)
{
	static const struct i915_subtest tests[] = {
		SUBTEST(igt_sync),
		SUBTEST(bench_sync),
	};

	return i915_subtests(tests, NULL);
}

static int emit_ggtt_store_dw(struct i915_request *rq, u32 addr, u32 value)
{
	u32 *cs;

	cs = intel_ring_begin(rq, 4);
	if (IS_ERR(cs))
		return PTR_ERR(cs);

	if (INTEL_GEN(rq->i915) >= 8) {
		*cs++ = MI_STORE_DWORD_IMM_GEN4 | MI_USE_GGTT;
		*cs++ = addr;
		*cs++ = 0;
		*cs++ = value;
	} else if (INTEL_GEN(rq->i915) >= 4) {
		*cs++ = MI_STORE_DWORD_IMM_GEN4 | MI_USE_GGTT;
		*cs++ = 0;
		*cs++ = addr;
		*cs++ = value;
	} else {
		*cs++ = MI_STORE_DWORD_IMM | MI_MEM_VIRTUAL;
		*cs++ = addr;
		*cs++ = value;
		*cs++ = MI_NOOP;
	}

	intel_ring_advance(rq, cs);

	return 0;
}

static u32 hwsp_address(const struct i915_timeline *tl)
{
	return i915_ggtt_offset(tl->hwsp_ggtt) + tl->hwsp_offset;
}

static struct i915_request *
tl_write(struct i915_timeline *tl, struct intel_engine_cs *engine, u32 value)
{
	struct i915_request *rq;
	int err;

	lockdep_assert_held(&tl->i915->drm.struct_mutex); /* lazy rq refs */

	err = i915_timeline_pin(tl);
	if (err) {
		rq = ERR_PTR(err);
		goto out;
	}

	rq = i915_request_alloc(engine, engine->i915->kernel_context);
	if (IS_ERR(rq))
		goto out_unpin;

	err = emit_ggtt_store_dw(rq, hwsp_address(tl), value);
	i915_request_add(rq);
	if (err)
		rq = ERR_PTR(err);

out_unpin:
	i915_timeline_unpin(tl);
out:
	if (IS_ERR(rq))
		pr_err("Failed to write to timeline!\n");
	return rq;
}

static struct i915_timeline *
checked_i915_timeline_create(struct drm_i915_private *i915)
{
	struct i915_timeline *tl;

	tl = i915_timeline_create(i915, "live", NULL);
	if (IS_ERR(tl))
		return tl;

	if (*tl->hwsp_seqno != tl->seqno) {
		pr_err("Timeline created with incorrect breadcrumb, found %x, expected %x\n",
		       *tl->hwsp_seqno, tl->seqno);
		i915_timeline_put(tl);
		return ERR_PTR(-EINVAL);
	}

	return tl;
}

static int live_hwsp_engine(void *arg)
{
#define NUM_TIMELINES 4096
	struct drm_i915_private *i915 = arg;
	struct i915_timeline **timelines;
	struct intel_engine_cs *engine;
	enum intel_engine_id id;
	intel_wakeref_t wakeref;
	unsigned long count, n;
	int err = 0;

	/*
	 * Create a bunch of timelines and check we can write
	 * independently to each of their breadcrumb slots.
	 */

	timelines = kvmalloc_array(NUM_TIMELINES * I915_NUM_ENGINES,
				   sizeof(*timelines),
				   GFP_KERNEL);
	if (!timelines)
		return -ENOMEM;

	mutex_lock(&i915->drm.struct_mutex);
	wakeref = intel_runtime_pm_get(i915);

	count = 0;
	for_each_engine(engine, i915, id) {
		if (!intel_engine_can_store_dword(engine))
			continue;

		for (n = 0; n < NUM_TIMELINES; n++) {
			struct i915_timeline *tl;
			struct i915_request *rq;

			tl = checked_i915_timeline_create(i915);
			if (IS_ERR(tl)) {
				err = PTR_ERR(tl);
				goto out;
			}

			rq = tl_write(tl, engine, count);
			if (IS_ERR(rq)) {
				i915_timeline_put(tl);
				err = PTR_ERR(rq);
				goto out;
			}

			timelines[count++] = tl;
		}
	}

out:
	if (igt_flush_test(i915, I915_WAIT_LOCKED))
		err = -EIO;

	for (n = 0; n < count; n++) {
		struct i915_timeline *tl = timelines[n];

		if (!err && *tl->hwsp_seqno != n) {
			pr_err("Invalid seqno stored in timeline %lu, found 0x%x\n",
			       n, *tl->hwsp_seqno);
			err = -EINVAL;
		}
		i915_timeline_put(tl);
	}

	intel_runtime_pm_put(i915, wakeref);
	mutex_unlock(&i915->drm.struct_mutex);

	kvfree(timelines);

	return err;
#undef NUM_TIMELINES
}

static int live_hwsp_alternate(void *arg)
{
#define NUM_TIMELINES 4096
	struct drm_i915_private *i915 = arg;
	struct i915_timeline **timelines;
	struct intel_engine_cs *engine;
	enum intel_engine_id id;
	intel_wakeref_t wakeref;
	unsigned long count, n;
	int err = 0;

	/*
	 * Create a bunch of timelines and check we can write
	 * independently to each of their breadcrumb slots with adjacent
	 * engines.
	 */

	timelines = kvmalloc_array(NUM_TIMELINES * I915_NUM_ENGINES,
				   sizeof(*timelines),
				   GFP_KERNEL);
	if (!timelines)
		return -ENOMEM;

	mutex_lock(&i915->drm.struct_mutex);
	wakeref = intel_runtime_pm_get(i915);

	count = 0;
	for (n = 0; n < NUM_TIMELINES; n++) {
		for_each_engine(engine, i915, id) {
			struct i915_timeline *tl;
			struct i915_request *rq;

			if (!intel_engine_can_store_dword(engine))
				continue;

			tl = checked_i915_timeline_create(i915);
			if (IS_ERR(tl)) {
				err = PTR_ERR(tl);
				goto out;
			}

			rq = tl_write(tl, engine, count);
			if (IS_ERR(rq)) {
				i915_timeline_put(tl);
				err = PTR_ERR(rq);
				goto out;
			}

			timelines[count++] = tl;
		}
	}

out:
	if (igt_flush_test(i915, I915_WAIT_LOCKED))
		err = -EIO;

	for (n = 0; n < count; n++) {
		struct i915_timeline *tl = timelines[n];

		if (!err && *tl->hwsp_seqno != n) {
			pr_err("Invalid seqno stored in timeline %lu, found 0x%x\n",
			       n, *tl->hwsp_seqno);
			err = -EINVAL;
		}
		i915_timeline_put(tl);
	}

	intel_runtime_pm_put(i915, wakeref);
	mutex_unlock(&i915->drm.struct_mutex);

	kvfree(timelines);

	return err;
#undef NUM_TIMELINES
}

static int live_hwsp_recycle(void *arg)
{
	struct drm_i915_private *i915 = arg;
	struct intel_engine_cs *engine;
	enum intel_engine_id id;
	intel_wakeref_t wakeref;
	unsigned long count;
	int err = 0;

	/*
	 * Check seqno writes into one timeline at a time. We expect to
	 * recycle the breadcrumb slot between iterations and neither
	 * want to confuse ourselves or the GPU.
	 */

	mutex_lock(&i915->drm.struct_mutex);
	wakeref = intel_runtime_pm_get(i915);

	count = 0;
	for_each_engine(engine, i915, id) {
		IGT_TIMEOUT(end_time);

		if (!intel_engine_can_store_dword(engine))
			continue;

		do {
			struct i915_timeline *tl;
			struct i915_request *rq;

			tl = checked_i915_timeline_create(i915);
			if (IS_ERR(tl)) {
				err = PTR_ERR(tl);
				goto out;
			}

			rq = tl_write(tl, engine, count);
			if (IS_ERR(rq)) {
				i915_timeline_put(tl);
				err = PTR_ERR(rq);
				goto out;
			}

			if (i915_request_wait(rq,
					      I915_WAIT_LOCKED,
					      HZ / 5) < 0) {
				pr_err("Wait for timeline writes timed out!\n");
				i915_timeline_put(tl);
				err = -EIO;
				goto out;
			}

			if (*tl->hwsp_seqno != count) {
				pr_err("Invalid seqno stored in timeline %lu, found 0x%x\n",
				       count, *tl->hwsp_seqno);
				err = -EINVAL;
			}

			i915_timeline_put(tl);
			count++;

			if (err)
				goto out;

			i915_timelines_park(i915); /* Encourage recycling! */
		} while (!__igt_timeout(end_time, NULL));
	}

out:
	if (igt_flush_test(i915, I915_WAIT_LOCKED))
		err = -EIO;
	intel_runtime_pm_put(i915, wakeref);
	mutex_unlock(&i915->drm.struct_mutex);

	return err;
}

int i915_timeline_live_selftests(struct drm_i915_private *i915)
{
	static const struct i915_subtest tests[] = {
		SUBTEST(live_hwsp_recycle),
		SUBTEST(live_hwsp_engine),
		SUBTEST(live_hwsp_alternate),
	};

	return i915_subtests(tests, i915);
}
