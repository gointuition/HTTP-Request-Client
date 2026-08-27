"""
High-performance HTTP client with native C implementation.

Usage:
    from python import httpClient

    httpClient.init()
    result = httpClient.request({...})
    httpClient.cleanup()
"""

from .http_client import HttpClient

# Singleton instance (mirrors nodejs/index.js pattern)
httpClient = HttpClient()

__all__ = ["HttpClient", "httpClient"]
__version__ = "1.0.0"
