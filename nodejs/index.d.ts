// nodejs/index.d.ts
export interface HttpRequestConfig {
    method: string;
    url: string;
    connectTimeoutInMilliseconds?: number;
    responseReadingTimeoutInMilliseconds?: number;
    decompress?: number;
    headers?: Record<string, string>;
    session?: {
        expirationInMilliseconds?: number;
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
    cleanup(): void;
}

declare const client: HttpClient;
export default client;
