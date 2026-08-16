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
        // uTLS fingerprint profile: hellochrome_auto | hellochrome_150 | hellocrios_auto | hellocrios_150
        clientHelloId?: string;
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

export class HttpClient {
    init(): this;
    request(config: HttpRequestConfig | string): HttpResult;
    requestAsync(config: HttpRequestConfig | string): Promise<string>;
    requestNonBlocking(config: HttpRequestConfig | string, pollIntervalMs?: number): Promise<string>;
    cleanup(): void;
}

declare const client: HttpClient;
export default client;
