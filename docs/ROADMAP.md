# Future Development Roadmap

- [x] **More HTTP Methods** — Full support for PUT / PATCH / DELETE (enums defined, logic pending)
- [x] **Concurrent Multiplexing** — Thread-safe shared connection with a per-connection reader thread; concurrent same-host requests are multiplexed on separate streams. Exposed to Node.js via the Promise-based `requestAsync`.
- [ ] **Streaming Response** — Callback-based streaming for large files / long-lived connections
- [x] **API Versioning** — Shared library soname version control
- [x] **CI/CD** — GitHub Actions for automated build and test
- [ ] **HTTP/1.1** — Protocol downgrade / fallback compatibility support
- [ ] **Other Browser Fingerprints** — TLS/HTTP2 fingerprint profiles for Firefox, Safari, Edge (enums defined, fingerprint data pending)
- [ ] **Other Platforms** — Support for Windows / macOS / iOS / Android (enums defined, logic pending)
- [x] **Different Versions** — Support for different browser versions, default auto 
- [x] **Fixed Third-Party Libraries** — Pin each auto-cloned dependency to a fixed revision for reproducible builds
- [x] **Client HelloId** — Read Client HelloId from request
- [x] **Log Per Request** — Not global
- [ ] **Calculate Content-Length in C** — Diff from NodeJS sometimes
- [ ] **PSK Share** — Diff proxy, diff PSK
- [ ] **Proxy 407 Reusage** — Disable reusage if 407
- [ ] **CONNECT Timeout** — Sometimes CONNECT cost too much time