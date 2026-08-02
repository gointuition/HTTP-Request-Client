#!/usr/bin/env python3
"""
Setup script for the HTTP/2 client Python binding.
Mirrors nodejs/package.json.
"""

from setuptools import setup, find_packages

# The native shared library (libhttp2client.{so,dylib,dll}) is placed under
# python/lib/ at build time by CI / build.sh, then shipped inside each platform's
# wheel via package_data, so wheels are self-contained (no compiler needed).
setup(
    name="http2-client",
    version="1.0.0",
    description="High-performance HTTP/2 client with native C implementation",
    author="Intuition",
    license="Apache-2.0",
    packages=find_packages(include=["python", "python.*"]),
    package_data={"python": ["lib/*"]},
    include_package_data=True,
    python_requires=">=3.8",
    install_requires=["cffi>=1.0.0"],
    keywords=["http2", "client", "native", "performance"],
    classifiers=[
        "Programming Language :: Python :: 3",
        "Programming Language :: C",
        "Topic :: Internet :: WWW/HTTP",
        "Operating System :: MacOS",
        "Operating System :: Microsoft :: Windows",
        "Operating System :: POSIX :: Linux",
    ],
)
