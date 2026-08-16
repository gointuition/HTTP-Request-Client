//
// Created by Intuition on 26-8-15.
//
// Non-blocking (fire-and-forget) request registry. handleRequestAsync() starts
// a request, sends HEADERS/DATA, and returns immediately with a request id; the
// stream keeps accumulating its response in the background reader thread.
// pollRequest() then reaps a finished request's response (or reports in-flight /
// timed-out). This is the async surface that sits on top of the non-blocking
// socket I/O mode selected by the request's "non-blocking" field.
//

#ifndef ASYNCREQUEST_H
#define ASYNCREQUEST_H

#ifdef __cplusplus
extern "C" {
#endif

// Start a request without waiting for its response. Returns a positive request
// id on success, 0 on failure (e.g. parse/session/send error). The caller must
// poll the id with pollRequest() until the request completes, then the id is
// reclaimed and may be reused.
long handleRequestAsync(const char *requestJSONString);

// Poll a previously started async request.
//   outStatus: 0 = still in flight, 1 = completed, -1 = failed/timed out
//   outLen:    length of the returned JSON string (0 when NULL)
// Returns a malloc'd response JSON string on completion (status 1); the caller
// owns and must free() it. Returns NULL otherwise. On completion the request id
// is reclaimed and must not be polled again.
char* pollRequest(long requestId, int *outStatus, int *outLen);

// Reap all in-flight async requests (used at process shutdown).
void cleanupAsyncRequests(void);

#ifdef __cplusplus
}
#endif

#endif //ASYNCREQUEST_H
