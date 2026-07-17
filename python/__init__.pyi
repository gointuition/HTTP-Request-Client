"""
Type stubs for the HTTP/2 client Python binding.
Mirrors nodejs/index.d.ts.
"""

from typing import TypedDict, Optional, List, Union


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


class HttpClient:
    def init(self) -> "HttpClient": ...
    def request(self, config: Union[HttpRequestConfig, str]) -> str: ...
    def cleanup(self) -> None: ...


httpClient: HttpClient
