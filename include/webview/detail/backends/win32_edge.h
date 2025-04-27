/*
 * MIT License
 *
 * Copyright (c) 2017 Serge Zaitsev
 * Copyright (c) 2022 Steffen André Langnes
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef WEBVIEW_BACKENDS_WIN32_EDGE_HH
#define WEBVIEW_BACKENDS_WIN32_EDGE_HH

#include "../../macros.h"

#include "../../http.h"
#include <map>
#include <string>

#if defined(__cplusplus) && !defined(WEBVIEW_HEADER)

#   if defined(WEBVIEW_PLATFORM_WINDOWS) && defined(WEBVIEW_EDGE)

//
// ====================================================================
//
// This implementation uses Win32 API to create a native window. It
// uses Edge/Chromium webview2 backend as a browser engine.
//
// ====================================================================
//

#      include "../engine_base.h"
#      include "../platform/windows/com_init_wrapper.h"
#      include "../platform/windows/webview2/loader.h"
#      include "../user_script.h"

#      include <Windows.h>
#      include <atomic>
#      include <functional>
#      include <list>

#      ifndef WIN32_LEAN_AND_MEAN
#         define WIN32_LEAN_AND_MEAN
#      endif

#      include <objbase.h>
#      include <ShlObj.h>
#      include <Shlwapi.h>
#      include <wrl.h>
#      include <intsafe.h>
#      include <windef.h>
#      include <wrl/client.h>
#      include <wrl/implements.h>
#      include <WebView2EnvironmentOptions.h>

#      ifdef _MSC_VER
#         pragma comment(lib, "ole32.lib")
#         pragma comment(lib, "shell32.lib")
#         pragma comment(lib, "shlwapi.lib")
#         pragma comment(lib, "user32.lib")
#         pragma comment(lib, "version.lib")
#      endif

namespace webview {
class user_script::impl {
public:
   impl(std::wstring id, std::wstring code);

   impl(const impl&)            = delete;
   impl& operator=(const impl&) = delete;
   impl(impl&&)                 = delete;
   impl& operator=(impl&&)      = delete;

   const std::wstring& GetId() const { return id_; }
   const std::wstring& GetCode() const { return code_; }

private:
   std::wstring id_;
   std::wstring code_;
};

namespace detail {

using msg_cb_t = std::function<void(const std::string)>;

class Webview2ComHandler
   : public ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler
   , public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler
   , public ICoreWebView2WebMessageReceivedEventHandler
   , public ICoreWebView2PermissionRequestedEventHandler {
   using webview2_com_handler_cb_t =
      std::function<void(ICoreWebView2Controller*, ICoreWebView2* webview)>;

public:
   Webview2ComHandler(msg_cb_t msgCb, webview2_com_handler_cb_t cb);

   virtual ~Webview2ComHandler()                                  = default;
   Webview2ComHandler(const Webview2ComHandler& other)            = delete;
   Webview2ComHandler& operator=(const Webview2ComHandler& other) = delete;
   Webview2ComHandler(Webview2ComHandler&& other)                 = delete;
   Webview2ComHandler& operator=(Webview2ComHandler&& other)      = delete;

   ULONG STDMETHODCALLTYPE   AddRef() override;
   ULONG STDMETHODCALLTYPE   Release() override;
   HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, LPVOID* ppv) override;
   HRESULT STDMETHODCALLTYPE Invoke(HRESULT res, ICoreWebView2Environment* env) override;
   HRESULT STDMETHODCALLTYPE Invoke(HRESULT res, ICoreWebView2Controller* controller) override;
   HRESULT STDMETHODCALLTYPE
   Invoke(ICoreWebView2* /*sender*/, ICoreWebView2WebMessageReceivedEventArgs* args) override;
   HRESULT STDMETHODCALLTYPE
   Invoke(ICoreWebView2* /*sender*/, ICoreWebView2PermissionRequestedEventArgs* args) override;

   // Set the function that will perform the initiating logic for creating
   // the WebView2 environment.
   void SetAttemptHandler(std::function<HRESULT()>&& attempt_handler) noexcept;

   // Retry creating a WebView2 environment.
   // The initiating logic for creating the environment is defined by the
   // caller of SetAttemptHandler().
   void TryCreateEnvironment() noexcept;

   void HandleWindow(HWND window);

private:
   HWND                      window_{};
   msg_cb_t                  msg_cb_{};
   webview2_com_handler_cb_t cb_{};
   std::atomic<ULONG>        ref_count_{1};
   std::function<HRESULT()>  attempt_handler_{};
   unsigned int              max_attempts_ = 5;
   unsigned int              attempts_     = 0;
};

class UserScriptHandler : public ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler {
public:
   using callback_fn = std::function<void(HRESULT errorCode, LPCWSTR id)>;

   explicit UserScriptHandler(callback_fn const& cb);

   virtual ~UserScriptHandler()                                 = default;
   UserScriptHandler(const UserScriptHandler& other)            = delete;
   UserScriptHandler& operator=(const UserScriptHandler& other) = delete;
   UserScriptHandler(UserScriptHandler&& other)                 = delete;
   UserScriptHandler& operator=(UserScriptHandler&& other)      = delete;

   ULONG STDMETHODCALLTYPE AddRef();
   ULONG STDMETHODCALLTYPE Release();

   HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, LPVOID* ppv);
   HRESULT STDMETHODCALLTYPE Invoke(HRESULT res, LPCWSTR id);

private:
   callback_fn        cb_;
   std::atomic<ULONG> ref_count_{1};
};

class Win32EdgeEngine final : public Webview {
public:
   static auto MakeOptions() { return Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>(); }

   static void SetSchemesOption(
      std::vector<std::string> const&                         schemes,
      Microsoft::WRL::ComPtr<ICoreWebView2EnvironmentOptions> options
   );

   using WebviewOptions = Microsoft::WRL::ComPtr<ICoreWebView2EnvironmentOptions>;
   Win32EdgeEngine(
      bool                  debug,
      HWND                  window,
      WebviewOptions        options       = nullptr,
      std::string_view      user_data_dir = "",
      DWORD                 style         = WS_OVERLAPPEDWINDOW,
      DWORD                 exStyle       = 0,
      std::function<void()> on_terminate  = []() constexpr {}
   );

   ~Win32EdgeEngine() final;

   Win32EdgeEngine(const Win32EdgeEngine& other)            = delete;
   Win32EdgeEngine& operator=(const Win32EdgeEngine& other) = delete;
   Win32EdgeEngine(Win32EdgeEngine&& other)                 = delete;
   Win32EdgeEngine& operator=(Win32EdgeEngine&& other)      = delete;

   HWND                     Window() const;
   HWND                     Widget() const;
   ICoreWebView2Controller* BrowserController() const;

   void RegisterUrlHandler(std::string const& filter, url_handler_t&& handler) final;
   void InstallResourceHandler() final;

   void SetTitle(std::string_view title) final;
   void SetSize(int width, int height, Hint hints) final;
   void SetPos(int x, int y) final;

   int Width() const final;
   int Height() const final;

   Size   GetSize() const final;
   Pos    GetPos() const final;
   Bounds GetBounds() const final;

   void Hide() final;
   void Show() final;
   void Restore() final;

   void ToForeground() final;

   void Run() final;
   void Terminate() final;

   void Eval(std::string_view js) final;
   void SetHtml(std::string_view html) final;

   void OpenDevTools() final;

private:
   void Dispatch(std::function<void()>&& f) final;

   void NavigateImpl(std::string_view url) final;

   std::multimap<std::wstring, url_handler_t, std::less<>> handlers_;

   //---------------------------------------------------------------------------------------------------------------------
   Microsoft::WRL::ComPtr<ICoreWebView2WebResourceResponse>
   MakeResponse(http::response_t const& responseData, HRESULT& result);

   //---------------------------------------------------------------------------------------------------------------------
   http::request_t MakeRequest(
      std::string const& uri,
      COREWEBVIEW2_WEB_RESOURCE_CONTEXT,
      ICoreWebView2WebResourceRequest* webViewRequest
   );

   user_script AddUserScriptImpl(std::string_view js) final;
   void        RemoveAllUserScript(std::list<user_script> const& scripts) final;
   bool        AreUserScriptsEqual(user_script const& first, user_script const& second) final;

private:
   void Embed(bool debug, msg_cb_t cb);

   void ResizeWidget();
   void ResizeWebview();
   void FocusWebview();

   bool Webview2Available() const noexcept;

   void OnDpiChanged(int dpi);

   SIZE  GetSizeImpl() const;
   POINT GetPosImpl() const;
   SIZE  GetScaledSize(int from_dpi, int to_dpi) const;

   void OnSystemSettingsChange(const wchar_t* area);

   // Blocks while depleting the run loop of events.
   void DepleteRunLoopEventQueue();

   // The app is expected to call CoInitializeEx before
   // CreateCoreWebView2EnvironmentWithOptions.
   // Source:
   // https://docs.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/webview2-idl#createcorewebview2environmentwithoptions
   com_init_wrapper         com_init_;
   HWND                     window_         = nullptr;
   HWND                     widget_         = nullptr;
   HWND                     message_window_ = nullptr;
   POINT                    minsz_          = POINT{0, 0};
   POINT                    maxsz_          = POINT{0, 0};
   DWORD                    main_thread_    = GetCurrentThreadId();
   ICoreWebView2*           webview_        = nullptr;
   ICoreWebView2Controller* controller_     = nullptr;
   Webview2ComHandler*      com_handler_    = nullptr;
   mswebview2::loader       webview2_loader_{};
   std::wstring             wuser_data_dir_{};
   using WebviewOptions = Microsoft::WRL::ComPtr<ICoreWebView2EnvironmentOptions>;
   WebviewOptions options_{};
   int            dpi_{};
   bool           owns_window_{};
};

}  // namespace detail

using browser_engine = detail::Win32EdgeEngine;

}  // namespace webview

#   endif  // defined(WEBVIEW_PLATFORM_WINDOWS) && defined(WEBVIEW_EDGE)
#endif     // defined(__cplusplus) && !defined(WEBVIEW_HEADER)
#endif     // WEBVIEW_BACKENDS_WIN32_EDGE_HH
