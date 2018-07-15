#if !defined(_VC4_TRACE_FREEBSD_H_) || defined(TRACE_HEADER_MULTI_READ)
#define _VC4_TRACE_FREEBSD_H_

#include <drm/drmP.h>

static inline void
trace_vc4_wait_for_seqno_begin(void* dev, uint64_t seqno, uint64_t timeout) {
	CTR3(KTR_DRM, "vc4_wait_for_seqno_begin %p %lu %lu", dev, seqno, timeout);
}

static inline void
trace_vc4_wait_for_seqno_end(void* dev, uint64_t seqno) {
	CTR2(KTR_DRM, "vc4_wait_for_seqno_end %p %lu", dev, seqno);
}

#endif
