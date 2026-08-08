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
- [x] **Calculate Content-Length in C** — Calculate it in C, and compare with existing one, if different, log and return error
- [x] **Order Headers** — Via HTTP Header X-HeaderOrderKey if present, and check if a header doesn't exist, return error.  
- [x] **PSK Share** — Diff proxy, diff PSK
- [x] **Proxy 407 Reuse** — Disable reuse if 407
- [x] **CONNECT Timeout** — Sometimes CONNECT cost too much time