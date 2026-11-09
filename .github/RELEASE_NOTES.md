netc++ is a cross-platform C++17 network library for Windows, macOS and Linux. Everything is used through the arrow operator.

```cpp
auto api = netcpp::api("https://api.example.com");
auto res = api->bearer(token)->timeout(10s)->post("/users", netcpp::Body::json(R"({"name":"bob"})"));
```

### What's inside
- **HTTP/HTTPS:** `get`, `post`, `put`, `patch`, `del`, `head`, `options` and any verb. Supports JSON, form, multipart and file bodies, auth, cookies, redirects, retries, proxies, HTTP/2, async requests, downloads and uploads with progress, and streaming.
- **WebSocket:** `ws://` and `wss://`.
- **Raw sockets:** TCP client and server, TLS client, UDP.
- **DNS:** forward and reverse lookups.
- **Utilities:** port scanner, TCP ping, local interfaces, public IP, URL, Base64 and SHA-1 helpers.

### Getting started (pick one)
1. **Starter project:** download `netcpp-starter-*.zip`, unzip it and open the folder in VS Code, Visual Studio or CLion, then build.
2. **Add to your CMake project:**
   ```cmake
   include(FetchContent)
   FetchContent_Declare(netcpp GIT_REPOSITORY https://github.com/Unblonded/netcpp.git GIT_TAG v1.0.0)
   FetchContent_MakeAvailable(netcpp)
   target_link_libraries(your_app PRIVATE netcpp::netcpp)
   ```
3. **Prebuilt binaries:** unzip the package for your OS and point CMake at it with `-DCMAKE_PREFIX_PATH=<folder>`, then `find_package(netcpp)`.
   - **Windows:** a DLL with libcurl built in (Schannel TLS) and no other dependencies. Copy `bin/netcpp.dll` next to your exe.
   - **macOS:** a universal binary (arm64 + x86_64) that uses the system libcurl.
   - **Linux (x64):** built on Ubuntu 22.04; needs `libcurl4` at runtime.
