# webview

[![Build Status](https://img.shields.io/github/actions/workflow/status/webview/webview/ci.yaml?branch=master)](https://github.com/webview/webview/actions)
[![Quality Gate Status](https://sonarqube.alex-home.fr/api/project_badges/measure?project=alx-home_webview_c41a971b-2ff0-4598-8196-4c0a1216e2cf&metric=alert_status)](https://sonarqube.alex-home.fr/dashboard?id=alx-home_webview_c41a971b-2ff0-4598-8196-4c0a1216e2cf)

Lightweight C++ webview wrapper used by the `msfs2024-vfrnav-efb` workspace.

This fork keeps the same public include path (`<webview/webview.h>`) while exposing extra integration points used by this project, including URL resource handlers, async bindings backed by `promise`, and user script lifecycle helpers.

## What this module provides

- A platform-selected engine alias: `webview::webview`
- Shared base API in `webview::Webview` for navigation, bindings, script injection, and window control
- Current production backend in this workspace: Win32 + Edge WebView2 (`webview::detail::Win32EdgeEngine`)
- CMake target alias: `alx-home::webview`

## Build and link

This submodule builds a library target named `alx-home_webview` and exports the alias target `alx-home::webview`.

### Option A: add as subdirectory

```cmake
add_subdirectory(submodules/webview)
target_link_libraries(my_app PRIVATE alx-home::webview)
```

### Option B: FetchContent

```cmake
include(FetchContent)

FetchContent_Declare(
	webview
	GIT_REPOSITORY https://github.com/alx-home/webview
	GIT_TAG master
)

FetchContent_MakeAvailable(webview)
target_link_libraries(my_app PRIVATE alx-home::webview)
```

## Windows backend notes

- WebView2 headers are resolved through CMake.
- If `WEBVIEW_USE_BUILTIN_MSWEBVIEW2` is ON (default), the build can fetch Microsoft WebView2 SDK (`WEBVIEW_MSWEBVIEW2_VERSION`).
- Extra dependencies are linked automatically (`advapi32`, `ole32`, `shell32`, `shlwapi`, `user32`, `version`).

## Minimal usage example (Win32)

```cpp
#include <webview/webview.h>

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
	try {
		auto const options = [] {
			auto opts = webview::detail::Win32EdgeEngine::MakeOptions();
			webview::detail::Win32EdgeEngine::SetSchemesOption({"app", "coui"}, opts);
			return opts;
		}();

		webview::webview w{
			false,      // debug
			nullptr,    // parent HWND, nullptr to create own window
			options,
			std::nullopt
		};

		w.SetTitle("webview example");
		w.SetSize(960, 640, webview::Hint::NONE);
		w.Navigate("https://example.com");
		w.Run();
	} catch (const webview::Exception& e) {
		MessageBoxA(nullptr, e.what(), "webview error", MB_OK | MB_ICONERROR);
		return 1;
	}

	return 0;
}
```

## Runtime integration patterns used in this workspace

- Register custom protocol handlers with `RegisterUrlHandler(...)`, then call `InstallResourceHandler()`.
- Expose host functions to JavaScript with `Bind(...)`.
- Inject one-time startup script with `Init(...)`.
- Inject/replace tracked scripts with `AddUserScript(...)`.
- Evaluate JS and receive async results via `Eval(js, callback)`.

See in-repo examples:

- `installer/main.cpp`
- `server/Main/Main.cpp`

## License

MIT (see `LICENSE`).
