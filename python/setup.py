#!/usr/bin/env python3
"""
Setup script for the HTTP/2 client Python binding.
Mirrors nodejs/package.json.
"""

from setuptools import setup

setup(
    name="http2-client",
    version="1.0.0",
    description="High-performance HTTP/2 client with native C implementation",
    author="Intuition",
    license="Apache-2.0",
    # map import name "http2client" to the python/ source directory
    packages=["http2client"],
    package_dir={"http2client": "."},
    package_data={
        "http2client": ["*.pyi", "*.py"],
    },
    python_requires=">=3.8",
    install_requires=["cffi>=1.0.0"],
    keywords=["http2", "client", "native", "performance"],
    classifiers=[
        "Programming Language :: Python :: 3",
        "Programming Language :: C",
        "Topic :: Internet :: WWW/HTTP",
        "Operating System :: MacOS",
        "Operating System :: POSIX :: Linux",
    ],
)
