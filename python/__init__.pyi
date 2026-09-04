"""
Type stubs for the HTTP client Python binding.
Mirrors nodejs/index.d.ts.
"""

from typing import TypedDict, Optional, List, Union, Callable, Tuple


class HttpRequestConfig(TypedDict, total=False):
    method: str
    url: str
    connectTimeoutInMilliseconds: int
    responseReadingTimeoutInMilliseconds: int
    decompress: int
    headers: dict[str, str]
    payload: dict
    proxy: "ProxyConfig"
    session: "SessionConfig"


class ProxyConfig(TypedDict):
    scheme: str
    host: str
    port: str
    authorization: Optional[str]


class SessionConfig(TypedDict, total=False):
    expirationInMilliseconds: int
    # uTLS fingerprint profile: hellochrome_auto | hellochrome_152 | hellochrome_150 | hellocrios_auto | hellocrios_150
    clientHelloId: str


class HttpResponse(TypedDict, total=False):
    statusCode: int
    headers: List[str]
    payload: str
    contentEncoding: str
    payloadEncoding: str
    payloadSize: int


class HttpError(TypedDict, total=False):
    code: str
    message: str


class HttpResult(TypedDict, total=False):
    url: str
    method: str
    request: dict
    response: HttpResponse
    error: HttpError
    session: dict


# Streaming callbacks: giving all three switches the request to streaming, so
# response.payload stays empty and "streamed" is 1. Giving none keeps the
# buffered response; giving only some raises ValueError, since a missing
# on_data would leave the body with neither a consumer nor a place in the result.
# on_headers: response headers as a dict, ":status" first, delivered once.
# on_data: one decoded body chunk; return True to stop the response early.
# on_complete: None when the body ended cleanly, otherwise the error dict.
OnHeaders = Callable[[dict], None]
OnData = Callable[[bytes], Optional[bool]]
OnComplete = Callable[[Optional[HttpError]], None]


class HttpClient:
    def init(self) -> "HttpClient": ...
    def request(self, config: Union[HttpRequestConfig, str],
                on_headers: Optional[OnHeaders] = None,
                on_data: Optional[OnData] = None,
                on_complete: Optional[OnComplete] = None) -> str: ...
    def start_request(self, config: Union[HttpRequestConfig, str],
                      on_headers: Optional[OnHeaders] = None,
                      on_data: Optional[OnData] = None,
                      on_complete: Optional[OnComplete] = None) -> int: ...
    def poll_request(self, request_id: int) -> Tuple[int, Optional[str]]: ...
    def request_non_blocking(self, config: Union[HttpRequestConfig, str],
                             poll_interval_ms: int = 5,
                             on_headers: Optional[OnHeaders] = None,
                             on_data: Optional[OnData] = None,
                             on_complete: Optional[OnComplete] = None) -> str: ...
    def cleanup(self) -> None: ...


httpClient: HttpClient
