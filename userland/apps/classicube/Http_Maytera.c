/*
 * Http_Maytera.c - ClassiCube HTTP backend for MayteraOS (task #28).
 *
 * WHAT THIS REPLACES. Upstream builds the HTTP layer one of two ways:
 *   - Http_Worker.c (CC_NET_BACKEND_BUILTIN): a worker THREAD that opens its
 *     own TCP socket, speaks HTTP/1.1 by hand and drives BearSSL through
 *     SSL.c/Certs.c for https://.
 *   - webclient/Http_Web.c: no sockets and no TLS at all; it hands the URL to
 *     an asynchronous host API and is driven by completion callbacks.
 * MayteraOS is the second shape. The kernel already owns a complete HTTP and
 * HTTPS client (kernel/net/wget.c and kernel/net/https.c, with TLS and the
 * cert store in kernel/net/tls/), exposed to Ring 3 as a START/POLL/READ job
 * API. Reusing it means this port ships NO second TLS stack, no cert bundle
 * and no HTTP parser of its own, which is the whole point of the project rule
 * about reusing the existing primitive instead of forking a private copy.
 *
 * THE SYSCALLS THIS USES (numbers live in userland/libc/syscall.h; this file
 * calls the wrappers, it never hardcodes a number):
 *   http_fetch_start(url)                 SYS_HTTP_FETCH_START    255
 *   http_fetch_poll(id, &status, &len)    SYS_HTTP_FETCH_POLL     256
 *   http_fetch_read(id, buf, max)         SYS_HTTP_FETCH_READ     257
 *   http_fetch_cancel(id)                 SYS_HTTP_FETCH_CANCEL   258
 *   http_fetch_progress(id, ...)          SYS_HTTP_FETCH_PROGRESS 368
 *   http_post_start/poll/read/cancel      SYS_HTTP_POST_*         265-268
 * POLL returns 0 = still running, 1 = done, 2 = error, -1 = not a live job.
 * READ copies the body out AND frees the kernel job slot, so it is called
 * exactly once per job. There are only ASYNC_FETCH_MAX = 6 job slots for the
 * WHOLE system (kernel/proc/syscall.c), which is why MHTTP_MAX_LIVE below is
 * deliberately small: ClassiCube must not starve the browser, the App Store
 * and the updater of every slot.
 *
 * NO THREAD, NO BUSY-WAIT. Every wait in this file is "come back next frame".
 * MaytHttp_Pump() only ever issues non-blocking syscalls and returns; it is
 * driven from two independent places so a wake can never be lost:
 *   1. a ScheduledTask2 registered in Http_Init (the game loop), and
 *   2. the public Http_GetResult/Http_CheckProgress/Http_GetCurrent entry
 *      points, which the Launcher's own loop (LWeb_Tick) calls even before
 *      the game loop exists.
 * That redundancy is the pattern CLAUDE.md asks for (a second always-armed
 * wake source), not belt-and-braces for its own sake: the Launcher does not
 * tick ScheduledTask2, and the in-game texture pack overlay does not always
 * call Http_CheckProgress. Neither path spins and neither path sleeps.
 *
 * NET_ERR_FAULTY (-3) IS RETRYABLE, NOT FATAL. The kernel keeps a global
 * failure-streak breaker; while it is tripped, sys_http_fetch_start() refuses
 * every fetch with NET_ERR_FAULTY instead of the generic -1. The breaker
 * clears itself on the first fetch that completes, so treating -3 as fatal
 * would be exactly the "the gate suppresses the evidence that clears the
 * gate" bug recorded in blame.md for #549. Here a -3 leaves the request AT
 * THE HEAD OF THE QUEUE and re-tries it on a paced backoff; only after
 * MHTTP_MAX_START_ATTEMPTS does the request finish with an error, and even
 * then only THAT request fails. A failed texture pack fetch must never take
 * the game down: TexturePack.c already treats a failed request as "keep the
 * current pack", so the failure path here is a normal completed-with-error
 * request, never an abort.
 *
 * KNOWN LIMITS OF THE KERNEL FETCHER, stated plainly because each one changes
 * behaviour a caller can see:
 *   - No request headers on the GET path. sys_http_fetch_start() takes a URL
 *     and nothing else, so If-Modified-Since / If-None-Match / Cookie are not
 *     sent on GETs. Consequence: no 304 revalidation, so a cached texture
 *     pack is re-downloaded rather than confirmed. Correct, just not optimal.
 *     (SYS_HTTP_FETCH_HDR 302 does take headers but is BLOCKING, and blocking
 *     the render thread is the one thing this port must never do.)
 *   - No response headers come back. Only the status code and the body length
 *     cross the syscall boundary, so ETag, Last-Modified and Set-Cookie are
 *     never populated. Consequence: the classicube.net sign-in flow, which
 *     needs the session cookie out of Set-Cookie, cannot work through this
 *     backend. It fails cleanly with a described error rather than hanging.
 *   - No HEAD. The kernel client only issues GET, so REQUEST_TYPE_HEAD is
 *     answered with an error immediately rather than silently downloading the
 *     whole body twice. The one caller (TexPackOverlay) degrades to
 *     "Download size: Unknown" and still lets the user accept the download.
 *
 * BUILD INTEGRATION (for the Makefile lane): compile this file INSTEAD OF
 * Http_Worker.c, SSL.c and Certs.c. It needs -I<ClassiCube>/src (for
 * _HttpBase.h and friends) and -I<userland>/libc (for syscall.h), and
 * -Wno-unused-function, because _HttpBase.h defines several static helpers
 * that only the socket-level backend uses (upstream's own web backend has the
 * same property).
 */
#include "_HttpBase.h"
#include "Errors.h"

/* MayteraOS userland libc. Provides the http_fetch_* / http_post_* inline
 * wrappers and NET_ERR_FAULTY. */
#include "syscall.h"

/* Concurrent kernel fetch jobs this app will hold. The kernel table is 6
 * slots for the entire system; 2 leaves room for everything else while still
 * letting a skin download overlap a texture pack download. */
#define MHTTP_MAX_LIVE 2

/* How many times a start may be refused before the request itself fails.
 * With MHTTP_BACKOFF_FAULTY_MS below this is ~20 s of patience, which spans
 * the kernel breaker's own paced re-probe interval. */
#define MHTTP_MAX_START_ATTEMPTS 10
#define MHTTP_BACKOFF_FAULTY_MS  2000  /* interface latched FAULTY: pace hard */
#define MHTTP_BACKOFF_BUSY_MS     500  /* no free kernel job slot: pace gently */

/* Errors this backend reports through cc_result. Distinct values so
 * MaytHttp_DescribeError can say which one happened; the 0x4D48 prefix is
 * 'M','H' and does not collide with the 0xCCDED0xx ClassiCube range or with
 * a plain errno. */
#define MHTTP_ERR_START_REFUSED 0x4D480001UL /* start failed, generic */
#define MHTTP_ERR_NET_FAULTY    0x4D480002UL /* start refused, NET_ERR_FAULTY */
#define MHTTP_ERR_JOB_FAILED    0x4D480003UL /* kernel job reported state 2 */
#define MHTTP_ERR_NO_HEAD       0x4D480004UL /* HEAD unsupported by the kernel */
#define MHTTP_ERR_LOST_JOB      0x4D480005UL /* poll says the job vanished */
#define MHTTP_ERR_READ_FAILED   0x4D480006UL /* READ returned an error */

static struct RequestList queuedReqs;   /* accepted, not started yet */
static struct RequestList workingReqs;  /* started, kernel job in flight */

/* One live kernel job. Keyed by the ClassiCube request id rather than by an
 * index into workingReqs, because RequestList_RemoveAt shuffles entries. */
struct MaytJob {
	int reqID;
	int jobID;    /* kernel job slot from http_fetch_start / http_post_start */
	int isPost;   /* which family of POLL/READ/CANCEL calls to use */
};
static struct MaytJob live[MHTTP_MAX_LIVE];
static int liveCount;

/* Paced retry gate for a refused start. */
static cc_uint64 backoffStart;
static int  backoffMS;    /* 0 = no gate armed */
static int  startFails;   /* consecutive refusals of startFailID */
static int  startFailID;

static struct ScheduledTask2 http_pumpTask;


/*########################################################################################################################*
*-------------------------------------------------------Job helpers-------------------------------------------------------*
*#########################################################################################################################*/
static int Live_Find(int reqID) {
	int i;
	for (i = 0; i < liveCount; i++)
	{
		if (live[i].reqID == reqID) return i;
	}
	return -1;
}

static void Live_RemoveAt(int i) {
	for (; i < liveCount - 1; i++) { live[i] = live[i + 1]; }
	liveCount--;
}

/* Releases the kernel job slot. POLL/READ/CANCEL all refuse an id whose slot
 * is already free, so this is safe to call on any path. */
static void Job_Release(struct MaytJob* job) {
	if (job->isPost) {
		http_post_cancel(job->jobID);
	} else {
		http_fetch_cancel(job->jobID);
	}
}


/*########################################################################################################################*
*----------------------------------------------------Starting requests----------------------------------------------------*
*#########################################################################################################################*/
static const cc_string url_rewrite_srcs[] = {
	#define URL_REMAP_FUNC(src_base, src_host, dst_base, dst_host) String_FromConst(src_base),
	#include "_HttpUrlMap.h"
};
static const char* const url_rewrite_dsts[] = {
	#undef  URL_REMAP_FUNC
	#define URL_REMAP_FUNC(src_base, src_host, dst_base, dst_host) dst_base,
	#include "_HttpUrlMap.h"
};

/* Converts e.g. "http://dl.dropbox.com/xyZ" into
 * "https://dl.dropboxusercontent.com/xyZ", same as every other backend. */
static void GetFinalUrl(struct HttpRequest* req, cc_string* dst) {
	cc_string url = String_FromRawArray(req->url);
	cc_string resource;
	int i;

	for (i = 0; i < (int)Array_Elems(url_rewrite_srcs); i++)
	{
		if (!String_CaselessStarts(&url, &url_rewrite_srcs[i])) continue;

		resource = String_UNSAFE_SubstringAt(&url, url_rewrite_srcs[i].length);
		String_Format2(dst, "%c%s", url_rewrite_dsts[i], &resource);
		return;
	}
	String_Copy(dst, &url);
}

/* Builds the extra request headers for a POST. The kernel supplies Host,
 * Content-Length and (when the caller does not) Content-Type, so only the
 * caller-specific lines belong here. Each line MUST end with CRLF. */
static void BuildPostHeaders(struct HttpRequest* req, cc_string* dst) {
	cc_string str;
	int i;

	String_AppendConst(dst, "Content-Type: application/x-www-form-urlencoded\r\n");

	if (req->cookies && req->cookies->count) {
		String_AppendConst(dst, "Cookie: ");
		for (i = 0; i < req->cookies->count; i++)
		{
			if (i) String_AppendConst(dst, "; ");
			str = StringsBuffer_UNSAFE_Get(req->cookies, i);
			String_AppendString(dst, &str);
		}
		String_AppendConst(dst, "\r\n");
	}
}

/* Arms the paced retry gate after a refused start. */
static void ArmBackoff(int reqID, int ms) {
	if (startFailID != reqID) { startFailID = reqID; startFails = 0; }
	startFails++;

	backoffStart = Stopwatch_Measure();
	backoffMS    = ms;
}

/* true when a start is allowed right now. */
static cc_bool BackoffElapsed(void) {
	if (!backoffMS) return true;
	if (Stopwatch_ElapsedMS(backoffStart, Stopwatch_Measure()) < backoffMS) return false;

	backoffMS = 0;
	return true;
}

/* Moves a request out of the queue and finishes it with the given error. */
static void FailQueuedHead(cc_result res) {
	struct HttpRequest* req = &queuedReqs.entries[0];
	struct HttpRequest tmp;

	req->result = res;
	HttpRequest_Copy(&tmp, req);
	RequestList_RemoveAt(&queuedReqs, 0);
	Http_FinishRequest(&tmp);

	backoffMS   = 0;
	startFails  = 0;
	startFailID = 0;
}

/* Static rather than stack: a MayteraOS user process gets a small stack (see
 * user.ld / the 16 KB stacks noted in user-mode.md), and this function is
 * reached from inside the render loop, so a multi-kilobyte frame here is a
 * real risk. Single-threaded backend, so a shared scratch buffer is safe. */
static char post_hdrBuffer[1024];
static char post_bodyBuffer[2048];
/* CP437 -> UTF8, NUL terminated, which is what the syscall wants. Worst case
 * is 4 bytes per source character. */
static char url_utf8[URL_MAX_SIZE * 4 + 1];

static void StartNextRequest(void) {
	char urlBuffer[URL_MAX_SIZE];      cc_string url;
	cc_string headers;
	struct HttpRequest* req;
	int id;

	if (liveCount >= MHTTP_MAX_LIVE) return;
	if (!queuedReqs.count)           return;
	if (!BackoffElapsed())           return;

	req = &queuedReqs.entries[0];

	/* The kernel client only issues GET. Answering HEAD with a full GET would
	 * download the whole body just to learn its length and then download it
	 * again, which is worse than admitting we cannot do it. */
	if (req->requestType == REQUEST_TYPE_HEAD) {
		FailQueuedHead(MHTTP_ERR_NO_HEAD);
		return;
	}

	String_InitArray(url, urlBuffer);
	GetFinalUrl(req, &url);
	Platform_Log1("Fetching %s", &url);
	String_EncodeUtf8(url_utf8, &url);

	if (req->requestType == REQUEST_TYPE_POST) {
		String_InitArray_NT(headers, post_hdrBuffer);
		BuildPostHeaders(req, &headers);
		post_hdrBuffer[headers.length] = '\0';

		/* The syscall takes NUL terminated strings, so an over-long body is
		 * rejected here rather than silently truncated on the way in. */
		if (req->size >= sizeof(post_bodyBuffer)) {
			FailQueuedHead(ERR_INVALID_ARGUMENT);
			return;
		}
		if (req->size) Mem_Copy(post_bodyBuffer, req->data, req->size);
		post_bodyBuffer[req->size] = '\0';

		id = http_post_start(url_utf8, post_hdrBuffer, post_bodyBuffer);
	} else {
		id = http_fetch_start(url_utf8);
	}

	if (id < 0) {
		/* NET_ERR_FAULTY is the kernel's failure-streak breaker refusing to
		 * touch the wire. It clears itself on the first fetch that completes,
		 * so this must stay retryable. */
		int isFaulty = (id == NET_ERR_FAULTY);
		ArmBackoff(req->id, isFaulty ? MHTTP_BACKOFF_FAULTY_MS : MHTTP_BACKOFF_BUSY_MS);

		if (startFails >= MHTTP_MAX_START_ATTEMPTS) {
			FailQueuedHead(isFaulty ? MHTTP_ERR_NET_FAULTY : MHTTP_ERR_START_REFUSED);
		}
		return;
	}

	/* Started. For a POST the request body has been copied into the kernel,
	 * so free it now: req->data is reused to hold the RESPONSE. */
	if (req->requestType == REQUEST_TYPE_POST) HttpRequest_Free(req);

	req->progress = HTTP_PROGRESS_MAKING_REQUEST;
	live[liveCount].reqID  = req->id;
	live[liveCount].jobID  = id;
	live[liveCount].isPost = req->requestType == REQUEST_TYPE_POST;
	liveCount++;

	RequestList_Append(&workingReqs, req, false);
	RequestList_RemoveAt(&queuedReqs, 0);

	backoffMS   = 0;
	startFails  = 0;
	startFailID = 0;
}


/*########################################################################################################################*
*----------------------------------------------------Polling requests-----------------------------------------------------*
*#########################################################################################################################*/
/* Finishes a working request, removing it from both lists. */
static void FinishWorking(int idx, int liveIdx, cc_result res, int status) {
	struct HttpRequest tmp;
	struct HttpRequest* req = &workingReqs.entries[idx];

	req->result     = res;
	req->statusCode = status;
	req->progress   = HTTP_PROGRESS_NOT_WORKING_ON;

	HttpRequest_Copy(&tmp, req);
	RequestList_RemoveAt(&workingReqs, idx);
	if (liveIdx >= 0) Live_RemoveAt(liveIdx);
	Http_FinishRequest(&tmp);
}

/* Copies a completed body out of the kernel. NOTE: http_fetch_read frees the
 * kernel job slot whether or not it succeeds, so the slot must not be
 * released again afterwards. */
static cc_result ReadBody(struct MaytJob* job, struct HttpRequest* req, cc_uint32 len) {
	int n;

	req->contentLength = len;
	if (!len) return 0;

	if (!Http_BufferExpand(req, len)) return ERR_OUT_OF_MEMORY;

	n = job->isPost ? http_post_read(job->jobID, (char*)req->data + req->size, len)
					: http_fetch_read(job->jobID, (char*)req->data + req->size, len);
	if (n < 0) return MHTTP_ERR_READ_FAILED;

	Http_BufferExpanded(req, (cc_uint32)n);
	return 0;
}

static void PollJob(int liveIdx) {
	struct MaytJob* job = &live[liveIdx];
	unsigned int len = 0, recv = 0, total = 0;
	int status = 0, state, idx;
	struct HttpRequest* req;
	cc_result res;

	idx = RequestList_Find(&workingReqs, job->reqID);
	if (idx < 0) {
		/* Cancelled out from under us: drop the kernel job too. */
		Job_Release(job);
		Live_RemoveAt(liveIdx);
		return;
	}
	req = &workingReqs.entries[idx];

	state = job->isPost ? http_post_poll(job->jobID, &status, &len)
						: http_fetch_poll(job->jobID, &status, &len);

	if (state < 0) { FinishWorking(idx, liveIdx, MHTTP_ERR_LOST_JOB, status); return; }

	if (state == 0) {
		/* Still running. Turn the kernel's real byte counter into the 0-100
		 * ClassiCube progress value; GETs only, the POST path has no
		 * progress syscall. */
		req->progress = HTTP_PROGRESS_FETCHING_DATA;
		if (!job->isPost && http_fetch_progress(job->jobID, NULL, &recv, &total) == 0) {
			req->contentLength = total;
			if (total) req->progress = (int)((100ULL * recv) / total);
		}
		return;
	}

	if (state == 2) {
		/* The job ran and failed. Releasing the slot is still required. */
		Job_Release(job);
		FinishWorking(idx, liveIdx, MHTTP_ERR_JOB_FAILED, status);
		return;
	}

	/* state == 1: completed. */
	res = ReadBody(job, req, len);
	FinishWorking(idx, liveIdx, res, status);
}

/* The single pump. Non-blocking from end to end: it issues POLL/READ/START
 * syscalls that all return immediately, and never loops waiting on anything. */
static void MaytHttp_Pump(void) {
	int i;
	/* Backwards, because PollJob can remove the entry it is looking at. */
	for (i = liveCount - 1; i >= 0; i--)
	{
		if (i < liveCount) PollJob(i);
	}

	StartNextRequest();
}

static cc_bool MaytHttp_PumpTask(struct ScheduledTask2* task) {
	(void)task;
	MaytHttp_Pump();
	return true;
}


/*########################################################################################################################*
*----------------------------------------------------Http public api------------------------------------------------------*
*#########################################################################################################################*/
cc_bool Http_GetResult(int reqID, struct HttpRequest* item) {
	int i;
	MaytHttp_Pump();

	i = RequestList_Find(&processedReqs, reqID);
	if (i >= 0) HttpRequest_Copy(item, &processedReqs.entries[i]);
	if (i >= 0) RequestList_RemoveAt(&processedReqs, i);
	return i >= 0;
}

cc_bool Http_GetCurrent(int* reqID, int* progress) {
	int idx;
	MaytHttp_Pump();

	if (!liveCount) { *reqID = 0; *progress = HTTP_PROGRESS_NOT_WORKING_ON; return false; }

	*reqID = live[0].reqID;
	idx    = RequestList_Find(&workingReqs, live[0].reqID);
	*progress = idx >= 0 ? workingReqs.entries[idx].progress : HTTP_PROGRESS_MAKING_REQUEST;
	return true;
}

int Http_CheckProgress(int reqID) {
	int idx;
	MaytHttp_Pump();

	idx = RequestList_Find(&workingReqs, reqID);
	if (idx < 0) return HTTP_PROGRESS_NOT_WORKING_ON;
	return workingReqs.entries[idx].progress;
}

void Http_ClearPending(void) {
	int i;
	/* Every in-flight kernel job must be released, or the 6 system-wide job
	 * slots leak on every disconnect and eventually nothing can fetch. */
	for (i = 0; i < liveCount; i++) { Job_Release(&live[i]); }
	liveCount = 0;

	RequestList_Free(&queuedReqs);
	RequestList_Free(&workingReqs);
}

void Http_TryCancel(int reqID) {
	int i = Live_Find(reqID);
	if (i >= 0) { Job_Release(&live[i]); Live_RemoveAt(i); }

	RequestList_TryFree(&queuedReqs,    reqID);
	RequestList_TryFree(&workingReqs,   reqID);
	RequestList_TryFree(&processedReqs, reqID);
}


/*########################################################################################################################*
*-----------------------------------------------------Maytera backend-----------------------------------------------------*
*#########################################################################################################################*/
static cc_bool HttpBackend_DescribeError(cc_result res, cc_string* dst) {
	switch (res) {
	case MHTTP_ERR_START_REFUSED:
		String_AppendConst(dst, "The network stack refused to start the request");
		return true;
	case MHTTP_ERR_NET_FAULTY:
		String_AppendConst(dst, "Networking is temporarily disabled after repeated failures");
		return true;
	case MHTTP_ERR_JOB_FAILED:
		String_AppendConst(dst, "The download failed (no response, or the connection dropped)");
		return true;
	case MHTTP_ERR_NO_HEAD:
		String_AppendConst(dst, "HEAD requests are not supported on MayteraOS");
		return true;
	case MHTTP_ERR_LOST_JOB:
		String_AppendConst(dst, "The download job disappeared");
		return true;
	case MHTTP_ERR_READ_FAILED:
		String_AppendConst(dst, "Failed to read the downloaded data");
		return true;
	}
	return false;
}

static void HttpBackend_Add(struct HttpRequest* req, cc_uint8 flags) {
	RequestList_Append(&queuedReqs, req, flags);
	/* Start it immediately when a slot is free, so a request issued from a
	 * one-shot UI action does not wait for the next scheduled pump. */
	StartNextRequest();
}


/*########################################################################################################################*
*-----------------------------------------------------Http component------------------------------------------------------*
*#########################################################################################################################*/
static void Http_Init(void) {
	Http_InitCommon();

	RequestList_Init(&queuedReqs);
	RequestList_Init(&workingReqs);
	RequestList_Init(&processedReqs);

	/* Http_FinishRequest (in _HttpBase.h) takes this, so it must exist even
	 * though this backend has no worker thread to contend with. */
	if (!processedMutex) processedMutex = Mutex_Create("HTTP processed");

	/* Second, independent wake source for the pump: the game loop. The public
	 * API calls above cover the Launcher, which does not tick these. */
	http_pumpTask.interval = 1.0f / 20.0f;
	http_pumpTask.callback = MaytHttp_PumpTask;
	ScheduledTask2_Add(&http_pumpTask);
}
