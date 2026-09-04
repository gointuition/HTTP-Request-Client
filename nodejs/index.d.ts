// nodejs/index.d.ts
export interface HttpRequestConfig {
    method: string;
    url: string;
    connectTimeoutInMilliseconds?: number;
    responseReadingTimeoutInMilliseconds?: number;
    decompress?: number;
    /** 0 = blocking (default), 1 = non-blocking socket I/O */
    'non-blocking'?: number;
    headers?: Record<string, string>;
    session?: {
        expirationInMilliseconds?: number;
        // uTLS fingerprint profile: hellochrome_auto | hellochrome_152 | hellochrome_150 | hellocrios_auto | hellocrios_150
        clientHelloId?: string;
        // force the session protocol: "h2" (default) or "http/1.1" (downgrade)
        protocol?: string;
    };
    proxy?: {
        scheme: string;
        host: string;
        port: string;
        authorization?: string;
    };
}

export interface HttpResponse {
    statusCode?: number;
    headers?: string[];
    payload?: string;
    /** 1 when the body went to the streaming callbacks (payload stays empty), 0 when buffered. */
    streamed?: number;
    contentEncoding?: string;
    payloadEncoding?: string;
    payloadSize?: number;
}

export interface HttpError {
    code?: string;
    message?: string;
}

export interface HttpResult {
    url?: string;
    method?: string;
    request?: {
        headers?: string[];
        payload?: string;
    };
    response?: HttpResponse;
    error?: HttpError;
    session?: {
        creationTime?: number;
        streamId?: number;
        expirationInMilliseconds?: number;
        clientHelloId?: string;
    };
}

export interface RequestResponse {
    status: number;
    data?: string;
    error?: string;
}

/**
 * Streaming callbacks: the decoded body goes to onData chunk by chunk and the
 * response JSON then reports "streamed": 1 with an empty payload. The bundle is
 * all or nothing — a hole in it would leave the body with neither a consumer nor
 * a place in the response, so a partial one throws before the request goes out.
 * Without a bundle the library collects the body itself and the JSON carries it
 * in response.payload.
 *
 * Only requestAsync() and requestNonBlocking() can stream: the synchronous
 * request() blocks the event loop, so no callback could ever be delivered.
 */
export interface StreamCallbacks {
    /** Response headers as an object (":status" first), delivered once. */
    onHeaders: (headers: Record<string, string>) => void;
    /** One decoded body chunk; return true to stop the response. */
    onData: (chunk: Buffer) => boolean | void;
    /** Delivered once: null when the body ended cleanly, otherwise the error. */
    onComplete: (error: HttpError | null) => void;
}

export class HttpClient {
    init(): this;
    request(config: HttpRequestConfig | string): HttpResult;
    requestAsync(config: HttpRequestConfig | string, callbacks?: StreamCallbacks): Promise<string>;
    requestNonBlocking(config: HttpRequestConfig | string, pollIntervalMs?: number,
                       callbacks?: StreamCallbacks): Promise<string>;
    cleanup(): void;
}

declare const client: HttpClient;
export default client;
