# netc++ starter project

A ready-to-build app that uses [netc++](https://github.com/Unblonded/netcpp).

## Open it in your editor

- **VS Code:** open this folder, accept the recommended CMake Tools extension, pick the `Debug` preset and press **F7** to build. Run with **Shift+F5**.
- **Visual Studio 2022:** use *File → Open → Folder…*, pick `Debug` in the toolbar, then *Build → Build All* and select `app.exe` as the startup item.
- **CLion:** open the folder and enable the `debug` profile when prompted.
- **Command line:**

  ```bash
  cmake --preset debug
  cmake --build --preset debug
  ./build/debug/app        # Windows: build\debug\Debug\app.exe
  ```

The first configure downloads netc++ automatically. On Windows it also builds libcurl, so it takes a minute.
Linux needs `libcurl4-openssl-dev` (Debian/Ubuntu) or `libcurl-devel` (Fedora).

Edit `main.cpp`, where everything is used through `->`:

```cpp
auto api = netcpp::api("https://api.example.com");
auto res = api->bearer(token)->post("/items", netcpp::Body::json(R"({"name":"x"})"));
```
