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

#include "detail/backends/win32_edge.h"
#include "detail/engine_base.h"
#include "detail/platform/windows/dpi.h"
#include "detail/platform/windows/theme.h"

#include <format>
#include <regex>
#include <utility>
#include <intsafe.h>
#include <minwindef.h>
#include <utils/String.h>
#include <winerror.h>
#include <wingdi.h>
#include <winuser.h>

#if defined(WEBVIEW_PLATFORM_WINDOWS) && defined(WEBVIEW_EDGE)

namespace webview {
user_script::impl::impl(std::wstring id, std::wstring code)
   : id_{std::move(id)}
   , code_{std::move(code)} {}

namespace detail {

Webview2ComHandler::Webview2ComHandler(msg_cb_t msgCb, webview2_com_handler_cb_t cb)
   : msg_cb_(std::move(msgCb))
   , cb_(std::move(cb)) {}

ULONG STDMETHODCALLTYPE
Webview2ComHandler::AddRef() {
   return ++ref_count_;
}

ULONG STDMETHODCALLTYPE
Webview2ComHandler::Release() {
   if (ref_count_ > 1) {
      return --ref_count_;
   }
   delete this;
   return 0;
}

HRESULT STDMETHODCALLTYPE
Webview2ComHandler::QueryInterface(REFIID riid, LPVOID* ppv) {
   using namespace mswebview2::cast_info;

   if (!ppv) {
      return E_POINTER;
   }

   // All of the COM interfaces we implement should be added here regardless
   // of whether they are required.
   // This is just to be on the safe side in case the WebView2 Runtime ever
   // requests a pointer to an interface we implement.
   // The WebView2 Runtime must at the very least be able to get a pointer to
   // ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler when we use
   // our custom WebView2 loader implementation, and observations have shown
   // that it is the only interface requested in this case. None have been
   // observed to be requested when using the official WebView2 loader.

   if (cast_if_equal_iid(this, riid, controller_completed, ppv)
       || cast_if_equal_iid(this, riid, environment_completed, ppv)
       || cast_if_equal_iid(this, riid, message_received, ppv)
       || cast_if_equal_iid(this, riid, permission_requested, ppv)) {
      return S_OK;
   }

   return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE
Webview2ComHandler::Invoke(HRESULT res, ICoreWebView2Environment* env) {
   if (SUCCEEDED(res)) {
      return env->CreateCoreWebView2Controller(window_, this);
   }
   TryCreateEnvironment();
   return S_OK;
}

HRESULT STDMETHODCALLTYPE
Webview2ComHandler::Invoke(HRESULT res, ICoreWebView2Controller* controller) {
   if (FAILED(res)) {
      // See TryCreateEnvironment() regarding
      // HRESULT_FROM_WIN32(ERROR_INVALID_STATE).
      // The result is E_ABORT if the parent window has been destroyed already.
      switch (res) {
         case HRESULT_FROM_WIN32(ERROR_INVALID_STATE):
         case E_ABORT:
            return S_OK;
      }
      TryCreateEnvironment();
      return S_OK;
   }

   ICoreWebView2*           webview;
   ::EventRegistrationToken token;
   controller->get_CoreWebView2(&webview);
   webview->add_WebMessageReceived(this, &token);
   webview->add_PermissionRequested(this, &token);

   cb_(controller, webview);
   return S_OK;
}

HRESULT STDMETHODCALLTYPE
Webview2ComHandler::Invoke(
   ICoreWebView2* /*sender*/,
   ICoreWebView2WebMessageReceivedEventArgs* args
) {
   LPWSTR message{};
   auto   res = args->TryGetWebMessageAsString(&message);
   if (SUCCEEDED(res)) {
      msg_cb_(utils::NarrowString(message));
   }

   CoTaskMemFree(message);
   return S_OK;
}

HRESULT STDMETHODCALLTYPE
Webview2ComHandler::Invoke(
   ICoreWebView2* /*sender*/,
   ICoreWebView2PermissionRequestedEventArgs* args
) {
   COREWEBVIEW2_PERMISSION_KIND kind;
   args->get_PermissionKind(&kind);
   if (kind == COREWEBVIEW2_PERMISSION_KIND_CLIPBOARD_READ) {
      args->put_State(COREWEBVIEW2_PERMISSION_STATE_ALLOW);
   }
   return S_OK;
}

// Set the function that will perform the initiating logic for creating
// the WebView2 environment.
void
Webview2ComHandler::SetAttemptHandler(std::function<HRESULT()>&& attempt_handler) noexcept {
   attempt_handler_ = std::move(attempt_handler);
}

// Retry creating a WebView2 environment.
// The initiating logic for creating the environment is defined by the
// caller of SetAttemptHandler().
void
Webview2ComHandler::TryCreateEnvironment() noexcept {
   // WebView creation fails with HRESULT_FROM_WIN32(ERROR_INVALID_STATE) if
   // a running instance using the same user data folder Exists, and the
   // Environment objects have different EnvironmentOptions.
   // Source:
   // https://docs.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2environment?view=webview2-1.0.1150.38
   if (attempts_ < max_attempts_) {
      ++attempts_;
      auto res = attempt_handler_();
      if (SUCCEEDED(res)) {
         return;
      }
      // Not entirely sure if this error code only applies to
      // CreateCoreWebView2Controller so we check here as well.
      if (res == HRESULT_FROM_WIN32(ERROR_INVALID_STATE)) {
         return;
      }
      TryCreateEnvironment();
      return;
   }
   // Give up.
   cb_(nullptr, nullptr);
}

void
Webview2ComHandler::HandleWindow(HWND window) {
   window_ = window;
   TryCreateEnvironment();
}

UserScriptHandler::UserScriptHandler(callback_fn const& cb)
   : cb_{cb} {}

ULONG STDMETHODCALLTYPE
UserScriptHandler::AddRef() {
   return ++ref_count_;
}

ULONG STDMETHODCALLTYPE
UserScriptHandler::Release() {
   if (ref_count_ > 1) {
      return --ref_count_;
   }
   delete this;
   return 0;
}

HRESULT STDMETHODCALLTYPE
UserScriptHandler::QueryInterface(REFIID riid, LPVOID* ppv) {
   using namespace mswebview2::cast_info;

   if (!ppv) {
      return E_POINTER;
   }

   if (cast_if_equal_iid(this, riid, add_script_to_execute_on_document_created_completed, ppv)) {
      return S_OK;
   }

   return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE
UserScriptHandler::Invoke(HRESULT res, LPCWSTR id) {
   cb_(res, id);
   return S_OK;
}

void
Win32EdgeEngine::SetSchemesOption(
   std::vector<std::string> const&                         schemes,
   Microsoft::WRL::ComPtr<ICoreWebView2EnvironmentOptions> options
) {
   Microsoft::WRL::ComPtr<ICoreWebView2EnvironmentOptions4> options4;

   auto const result = options.As(&options4);
   if (result != S_OK) {
      throw Exception{
         error_t::WEBVIEW_ERROR_UNSPECIFIED,
         std::format("Could not set options: {}", std::to_string(result))
      };
   }

   using schemes_t = std::vector<Microsoft::WRL::ComPtr<ICoreWebView2CustomSchemeRegistration>>;
   schemes_t web_schemes{};
   using schemes_registration_t = std::vector<ICoreWebView2CustomSchemeRegistration*>;
   schemes_registration_t data{};

   for (auto const& scheme : schemes) {
      auto const wscheme = utils::WidenString(scheme);
      auto const schemeReg =
         Microsoft::WRL::Make<CoreWebView2CustomSchemeRegistration>(wscheme.c_str());

      std::array<wchar_t const*, 1> origins{L"*"};
      schemeReg->SetAllowedOrigins(static_cast<UINT32>(origins.size()), origins.data());
      schemeReg->put_TreatAsSecure(true);
      schemeReg->put_HasAuthorityComponent(true);

      web_schemes.emplace_back(schemeReg);
      data.emplace_back(schemeReg.Get());
   }

   auto ptr = data.data();
   options4->SetCustomSchemeRegistrations(static_cast<UINT32>(data.size()), ptr);
}

Win32EdgeEngine::Win32EdgeEngine(
   bool                  debug,
   HWND                  window,
   WebviewOptions        options,
   std::string_view      user_data_dir,
   DWORD                 style,
   DWORD                 exStyle,
   std::function<void()> on_terminate
)
   : Webview(std::move(on_terminate))
   , wuser_data_dir_{utils::WidenString(user_data_dir)}
   , options_{std::move(options)}
   , owns_window_{!window} {
   if (!Webview2Available()) {
      throw Exception{error_t::WEBVIEW_ERROR_MISSING_DEPENDENCY, "WebView2 is unavailable"};
   }

   HINSTANCE instance = GetModuleHandle(nullptr);

   if (owns_window_) {
      com_init_ = com_init_wrapper{COINIT_APARTMENTTHREADED};
      EnableDpiAwareness();

      auto icon = static_cast<HICON>(LoadImage(
         instance,
         IDI_APPLICATION,
         IMAGE_ICON,
         GetSystemMetrics(SM_CXICON),
         GetSystemMetrics(SM_CYICON),
         LR_DEFAULTCOLOR
      ));

      // Create a top-level window.
      WNDCLASSEXW wc;
      ZeroMemory(&wc, sizeof(WNDCLASSEX));
      wc.cbSize        = sizeof(WNDCLASSEX);
      wc.hInstance     = instance;
      wc.lpszClassName = L"webview";
      wc.hIcon         = icon;
      wc.lpfnWndProc   = (WNDPROC)(+[](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
         Win32EdgeEngine* w{};

         if (msg == WM_NCCREATE) {
            auto* lpcs{reinterpret_cast<LPCREATESTRUCT>(lp)};
            w          = static_cast<Win32EdgeEngine*>(lpcs->lpCreateParams);
            w->window_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(w));
            enable_non_client_dpi_scaling_if_needed(hwnd);
            ApplyWindowTheme(hwnd);
         } else {
            w = reinterpret_cast<Win32EdgeEngine*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
         }

         if (!w) {
            return DefWindowProcW(hwnd, msg, wp, lp);
         }

         switch (msg) {
            case WM_SIZE:
               w->ResizeWidget();
               break;
            case WM_CLOSE:
               DestroyWindow(hwnd);
               break;
            case WM_DESTROY:
               w->window_ = nullptr;
               SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
               w->OnWindowDestroyed();
               break;
            case WM_GETMINMAXINFO: {
               auto lpmmi = (LPMINMAXINFO)lp;
               if (w->maxsz_.x > 0 && w->maxsz_.y > 0) {
                  lpmmi->ptMaxSize      = w->maxsz_;
                  lpmmi->ptMaxTrackSize = w->maxsz_;
               }
               if (w->minsz_.x > 0 && w->minsz_.y > 0) {
                  lpmmi->ptMinTrackSize = w->minsz_;
               }
            } break;
            case 0x02E4 /*WM_GETDPISCALEDSIZE*/: {
               auto  dpi = static_cast<int>(wp);
               auto* size{reinterpret_cast<SIZE*>(lp)};
               *size = w->GetScaledSize(w->dpi_, dpi);
               return TRUE;
            }
            case 0x02E0 /*WM_DPICHANGED*/: {
               // Windows 10: The size we get here is exactly what we supplied to
               // WM_GETDPISCALEDSIZE. Windows 11: The size we get here is NOT what we supplied to
               // WM_GETDPISCALEDSIZE. Due to this difference, don't use the suggested bounds.
               auto dpi = static_cast<int>(HIWORD(wp));
               w->OnDpiChanged(dpi);
               break;
            }
            case WM_SETTINGCHANGE: {
               auto* area = reinterpret_cast<const wchar_t*>(lp);
               if (area) {
                  w->OnSystemSettingsChange(area);
               }
               break;
            }
            case WM_ACTIVATE:
               if (LOWORD(wp) != WA_INACTIVE) {
                  w->FocusWebview();
               }
               break;
            default:
               return DefWindowProcW(hwnd, msg, wp, lp);
         }
         return 0;
      });
      RegisterClassExW(&wc);

      CreateWindowExW(
         exStyle,
         L"webview",
         L"",
         style,
         CW_USEDEFAULT,
         CW_USEDEFAULT,
         0,
         0,
         nullptr,
         nullptr,
         instance,
         this
      );
      if (!window_) {
         throw Exception{error_t::WEBVIEW_ERROR_INVALID_STATE, "Window is null"};
      }
      OnWindowCreated();

      dpi_ = GetWindowDpi(window_);

      if (style) {
         constexpr const int initial_width  = 640;
         constexpr const int initial_height = 480;
         SetSize(initial_width, initial_height, Hint::NONE);
      }
   } else {
      window_ = window;
      dpi_    = GetWindowDpi(window_);
   }

   // Create a window that WebView2 will be embedded into.
   WNDCLASSEXW widget_wc{};
   widget_wc.cbSize        = sizeof(WNDCLASSEX);
   widget_wc.hInstance     = instance;
   widget_wc.lpszClassName = L"webview_widget";
   widget_wc.lpfnWndProc   = (WNDPROC)(+[](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
      Win32EdgeEngine* w{};

      if (msg == WM_NCCREATE) {
         auto* lpcs{reinterpret_cast<LPCREATESTRUCT>(lp)};
         w          = static_cast<Win32EdgeEngine*>(lpcs->lpCreateParams);
         w->widget_ = hwnd;
         SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(w));
      } else {
         w = reinterpret_cast<Win32EdgeEngine*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
      }

      if (!w) {
         return DefWindowProcW(hwnd, msg, wp, lp);
      }

      switch (msg) {
         case WM_SIZE:
            w->ResizeWebview();
            break;
         case WM_DESTROY:
            w->widget_ = nullptr;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            break;
         default:
            return DefWindowProcW(hwnd, msg, wp, lp);
      }
      return 0;
   });
   RegisterClassExW(&widget_wc);
   CreateWindowExW(
      WS_EX_CONTROLPARENT,
      L"webview_widget",
      nullptr,
      WS_CHILD,
      0,
      0,
      0,
      0,
      window_,
      nullptr,
      instance,
      this
   );
   if (!widget_) {
      throw Exception{error_t::WEBVIEW_ERROR_INVALID_STATE, "Widget window is null"};
   }

   // Create a message-only window for internal messaging.
   WNDCLASSEXW message_wc{};
   message_wc.cbSize        = sizeof(WNDCLASSEX);
   message_wc.hInstance     = instance;
   message_wc.lpszClassName = L"webview_message";
   message_wc.lpfnWndProc   = (WNDPROC)(+[](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
      Win32EdgeEngine* w{};

      if (msg == WM_NCCREATE) {
         auto* lpcs{reinterpret_cast<LPCREATESTRUCT>(lp)};
         w                  = static_cast<Win32EdgeEngine*>(lpcs->lpCreateParams);
         w->message_window_ = hwnd;
         SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(w));
      } else {
         w = reinterpret_cast<Win32EdgeEngine*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
      }

      if (!w) {
         return DefWindowProcW(hwnd, msg, wp, lp);
      }

      switch (msg) {
         case WM_APP:
            if (auto f = reinterpret_cast<std::function<void()>*>(lp)) {
               (*f)();
               delete f;
            }
            break;
         case WM_DESTROY:
            w->message_window_ = nullptr;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            break;
         default:
            return DefWindowProcW(hwnd, msg, wp, lp);
      }
      return 0;
   });
   RegisterClassExW(&message_wc);
   CreateWindowExW(
      0, L"webview_message", nullptr, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, this
   );
   if (!message_window_) {
      throw Exception{error_t::WEBVIEW_ERROR_INVALID_STATE, "Message window is null"};
   }

   if (owns_window_) {
      ShowWindow(window_, SW_SHOW);
      UpdateWindow(window_);
      SetFocus(window_);
   }

   Embed(debug, [this](std::string_view msg) { this->OnMessage(msg); });
}

Win32EdgeEngine::~Win32EdgeEngine() {
   if (com_handler_) {
      com_handler_->Release();
      com_handler_ = nullptr;
   }
   if (webview_) {
      webview_->Release();
      webview_ = nullptr;
   }
   if (controller_) {
      controller_->Release();
      controller_ = nullptr;
   }
   // Replace wndproc to avoid callbacks and other bad things during
   // destruction.
   auto wndproc = reinterpret_cast<LONG_PTR>(+[](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
      return DefWindowProcW(hwnd, msg, wp, lp);
   });
   if (widget_) {
      SetWindowLongPtrW(widget_, GWLP_WNDPROC, wndproc);
   }
   if (window_ && owns_window_) {
      SetWindowLongPtrW(window_, GWLP_WNDPROC, wndproc);
   }
   if (widget_) {
      DestroyWindow(widget_);
      widget_ = nullptr;
   }
   if (window_) {
      if (owns_window_) {
         DestroyWindow(window_);
         OnWindowDestroyed(true);
      }
      window_ = nullptr;
   }
   if (message_window_) {

      if (owns_window_) {
         // Not strictly needed for windows to close immediately but aligns
         // behavior across backends.
         DepleteRunLoopEventQueue();
      }
      // We need the message window in order to deplete the event queue.
      SetWindowLongPtrW(message_window_, GWLP_WNDPROC, wndproc);
      DestroyWindow(message_window_);
   }

   message_window_ = nullptr;
}

void
Win32EdgeEngine::Run() {
   MSG msg;
   while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
   }
}

HWND
Win32EdgeEngine::Window() const {
   if (window_) {
      return window_;
   }

   throw Exception{error_t::WEBVIEW_ERROR_INVALID_STATE};
}

HWND
Win32EdgeEngine::Widget() const {
   if (widget_) {
      return widget_;
   }
   throw Exception{error_t::WEBVIEW_ERROR_INVALID_STATE};
}

ICoreWebView2Controller*
Win32EdgeEngine::BrowserController() const {
   if (controller_) {
      return controller_;
   }
   throw Exception{error_t::WEBVIEW_ERROR_INVALID_STATE};
}

void
Win32EdgeEngine::Terminate() {
   PostQuitMessage(0);
}

void
Win32EdgeEngine::Dispatch(std::function<void()>&& f) {
   PostMessageW(message_window_, WM_APP, 0, (LPARAM) new std::function<void()>(std::move(f)));
}

void
Win32EdgeEngine::SetTitle(std::string_view title) {
   SetWindowTextW(window_, utils::WidenString(title).c_str());
}

void
Win32EdgeEngine::SetSize(int width, int height, Hint hints) {
   auto style = GetWindowLong(window_, GWL_STYLE);

   if (hints == Hint::STATIC) {
      style &= ~(WS_THICKFRAME | WS_CAPTION);
      style |= WS_EX_TOPMOST;
   } else if (hints == Hint::FIXED) {
      style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
   } else {
      style |= (WS_THICKFRAME | WS_MAXIMIZEBOX);
   }

   SetWindowLong(window_, GWL_STYLE, style);

   if ((hints == Hint::MAX) || (hints == Hint::STATIC)) {
      maxsz_.x = width;
      maxsz_.y = height;
   }

   if ((hints == Hint::MIN) || (hints == Hint::STATIC)) {
      minsz_.x = width;
      minsz_.y = height;
   }

   if ((hints != Hint::MAX) && (hints != Hint::MIN)) {
      auto dpi         = GetWindowDpi(window_);
      dpi_             = dpi;
      auto scaled_size = ScaleSize(width, height, get_default_window_dpi(), dpi);
      auto frame_size  = MakeWindowFrameSize(window_, scaled_size.cx, scaled_size.cy, dpi);
      SetWindowPos(
         window_,
         nullptr,
         0,
         0,
         frame_size.cx,
         frame_size.cy,
         SWP_NOACTIVATE | SWP_NOMOVE | SWP_FRAMECHANGED | SWP_NOZORDER
      );
   }
}

void
Win32EdgeEngine::SetPos(int x, int y) {
   SetWindowPos(window_, HWND_TOPMOST, x, y, 0, 0, SWP_NOACTIVATE | SWP_NOSIZE);
}

int
Win32EdgeEngine::Width() const {
   return GetSizeImpl().cx;
}

int
Win32EdgeEngine::Height() const {
   return GetSizeImpl().cy;
}

Size
Win32EdgeEngine::GetSize() const {
   auto const size = GetSizeImpl();
   return {.width_ = size.cx, .height_ = size.cy};
}

Pos
Win32EdgeEngine::GetPos() const {
   auto const size = GetPosImpl();
   return {.x_ = size.x, .y_ = size.y};
}

Bounds
Win32EdgeEngine::GetBounds() const {
   RECT bounds;
   GetWindowRect(window_, &bounds);
   return {
      {.x_ = bounds.left, .y_ = bounds.top},
      {.width_ = bounds.right - bounds.left, .height_ = bounds.bottom - bounds.top}
   };
}

void
Win32EdgeEngine::Hide() {
   ShowWindow(window_, SW_HIDE);
}

void
Win32EdgeEngine::Show() {
   ShowWindow(window_, SW_SHOW);
}

void
Win32EdgeEngine::Restore() {
   ShowWindow(window_, SW_RESTORE);
}

void
Win32EdgeEngine::ToForeground() {
   ::SetForegroundWindow(window_);
}

void
Win32EdgeEngine::NavigateImpl(std::string_view url) {
   auto wurl = utils::WidenString(url);
   webview_->Navigate(wurl.c_str());
}

void
Win32EdgeEngine::OpenDevTools() {
   webview_->OpenDevToolsWindow();
}

void
Win32EdgeEngine::RegisterUrlHandler(std::string const& filter, url_handler_t&& handler) {
   auto wfilter = utils::WidenString(filter);
   auto result  = webview_->AddWebResourceRequestedFilter(
      wfilter.c_str(), COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL
   );

   if (result != S_OK) {
      throw Exception{
         error_t::WEBVIEW_ERROR_UNSPECIFIED,
         std::format(
            "Could not AddWebResourceRequestedFilter: {} for scheme: {}",
            std::to_string(result),
            filter
         )
      };
   }

   auto const reg = [&wfilter]() constexpr {
      std::wstring res;

      for (std::size_t i = 0; i < wfilter.length(); ++i) {
         if ((wfilter[i] == L'*' || wfilter[i] == L'?') && (i == 0 || wfilter[i - 1] != L'\\')) {
            if (i != 0) {
               res.pop_back();
            }
            res += std::wstring{L"."} + wfilter[i];
         } else {
            res = wfilter[i];
         }
      }

      return res;
   }();

   handlers_.emplace(reg, std::move(handler));
}

void
Win32EdgeEngine::InstallResourceHandler() {
   ::EventRegistrationToken token;

   auto const result = webview_->add_WebResourceRequested(
      Microsoft::WRL::Callback<ICoreWebView2WebResourceRequestedEventHandler>(
         [this](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) {
            COREWEBVIEW2_WEB_RESOURCE_CONTEXT resource_context;

            auto result = args->get_ResourceContext(&resource_context);
            if (result != S_OK) {
               return result;
            }

            Microsoft::WRL::ComPtr<ICoreWebView2WebResourceRequest> web_view_request;
            args->get_Request(&web_view_request);

            const auto wuri = [&web_view_request]() {
               LPWSTR uri;
               web_view_request->get_Uri(&uri);
               std::wstring wuri{uri};
               CoTaskMemFree(uri);

               return wuri;
            }();

            for (auto const& handler : handlers_) {
               if (std::regex_match(wuri, std::wregex{handler.first})) {
                  auto const request = MakeRequest(
                     utils::NarrowString(wuri), resource_context, web_view_request.Get()
                  );
                  auto const response = MakeResponse(handler.second(request), result);

                  if (result != S_OK) {
                     return result;
                  }

                  return args->put_Response(response.Get());
               }
            }

            return S_OK;
         }
      ).Get(),
      &token
   );

   if (result != S_OK) {
      throw Exception{
         error_t::WEBVIEW_ERROR_UNSPECIFIED,
         std::format("Could not install resource handler: {}", std::to_string(result))
      };
   }
}

//---------------------------------------------------------------------------------------------------------------------
Microsoft::WRL::ComPtr<ICoreWebView2WebResourceResponse>
Win32EdgeEngine::MakeResponse(http::response_t const& responseData, HRESULT& result) {
   Microsoft::WRL::ComPtr<ICoreWebView2WebResourceResponse> response;
   Microsoft::WRL::ComPtr<ICoreWebView2_2>                  wv22;
#   pragma clang diagnostic push
#   pragma clang diagnostic ignored "-Wlanguage-extension-token"
   result = webview_->QueryInterface(IID_PPV_ARGS(&wv22));
#   pragma clang diagnostic pop

   Microsoft::WRL::ComPtr<ICoreWebView2Environment> environment;
   wv22->get_Environment(&environment);

   if (result != S_OK) {
      return {};
   }

   std::wstring response_headers;
   for (auto const& [key, value] : responseData.headers) {
      response_headers += utils::WidenString(key) + L": " + utils::WidenString(value) + L"\r\n";
   }

   if (!response_headers.empty()) {
      response_headers.pop_back();
      response_headers.pop_back();
   }

   Microsoft::WRL::ComPtr<IStream> stream;
   stream.Attach(SHCreateMemStream(
      reinterpret_cast<const BYTE*>(responseData.body.data()),
      static_cast<UINT>(responseData.body.size())
   ));

   const auto phrase = utils::WidenString(responseData.reasonPhrase);
   result            = environment->CreateWebResourceResponse(
      stream.Get(), responseData.statusCode, phrase.c_str(), response_headers.c_str(), &response
   );

   return response;
}

//---------------------------------------------------------------------------------------------------------------------
http::request_t
Win32EdgeEngine::MakeRequest(
   std::string const& uri,
   COREWEBVIEW2_WEB_RESOURCE_CONTEXT,
   ICoreWebView2WebResourceRequest* webViewRequest
) {
   return http::request_t{
      .getContent =
         [webViewRequest, content_memo = std::string{}]() mutable {
            if (!content_memo.empty()) {
               return content_memo;
            }

            Microsoft::WRL::ComPtr<IStream> stream;
            webViewRequest->get_Content(&stream);

            if (!stream) {
               return content_memo;
            }

            // FIXME: Dont read the whole thing into memory, if possible via streaming.
            ULONG bytes_read = 0;
            do {
               std::array<char, 1024> buffer{};
               stream->Read(buffer.data(), 1024, &bytes_read);
               content_memo.append(buffer.data(), bytes_read);
            } while (bytes_read == 1024);
            return content_memo;
         },
      .uri = uri,
      .method =
         [webViewRequest]() {
            LPWSTR method;
            webViewRequest->get_Method(&method);
            std::wstring method_w{method};
            CoTaskMemFree(method);
            return utils::NarrowString(method_w);
         }(),
      .headers =
         [webViewRequest]() {
            ICoreWebView2HttpRequestHeaders* headers;
            webViewRequest->get_Headers(&headers);

            Microsoft::WRL::ComPtr<ICoreWebView2HttpHeadersCollectionIterator> iterator;
            headers->GetIterator(&iterator);

            std::unordered_multimap<std::string, std::string> headers_map;
            for (BOOL has_current;
                 SUCCEEDED(iterator->get_HasCurrentHeader(&has_current)) && has_current;) {
               LPWSTR name;
               LPWSTR value;
               iterator->GetCurrentHeader(&name, &value);
               std::wstring name_w{name};
               std::wstring value_w{value};
               CoTaskMemFree(name);
               CoTaskMemFree(value);

               headers_map.emplace(utils::NarrowString(name_w), utils::NarrowString(value_w));

               BOOL has_next = FALSE;
               if (FAILED(iterator->MoveNext(&has_next)) || !has_next) {
                  break;
               }
            }
            return headers_map;
         }()
   };
}

void
Win32EdgeEngine::Eval(std::string_view js) {
   // TODO: Skip if no content has begun loading yet. Can't check with
   //       ICoreWebView2::get_Source because it returns "about:blank".
   auto wjs = utils::WidenString(js);
   webview_->ExecuteScript(wjs.c_str(), nullptr);
}

void
Win32EdgeEngine::SetHtml(std::string_view html) {
   webview_->NavigateToString(utils::WidenString(html).c_str());
}

user_script
Win32EdgeEngine::AddUserScriptImpl(std::string_view js) {
   auto              wjs = utils::WidenString(js);
   std::wstring      script_id;
   bool              done{};
   UserScriptHandler handler{[&script_id, &done](HRESULT res, LPCWSTR id) {
      if (SUCCEEDED(res)) {
         script_id = id;
      }
      done = true;
   }};
   auto              res = webview_->AddScriptToExecuteOnDocumentCreated(wjs.c_str(), &handler);
   if (SUCCEEDED(res)) {
      // Sadly we need to pump the even loop in order to get the script ID.
      while (!done) {
         DepleteRunLoopEventQueue();
      }
   }
   // TODO: There's a non-zero chance that we didn't get the script ID.
   //       We need to convey the error somehow.
   return {
      js,
      user_script::impl_ptr{
         new user_script::impl{script_id, wjs}, [](user_script::impl* p) { delete p; }
      }
   };
}

void
Win32EdgeEngine::RemoveAllUserScript(std::list<user_script> const& scripts) {
   for (const auto& script : scripts) {
      const auto& id = script.get_impl().GetId();
      webview_->RemoveScriptToExecuteOnDocumentCreated(id.c_str());
   }
}

bool
Win32EdgeEngine::AreUserScriptsEqual(user_script const& first, user_script const& second) {
   const auto& first_id  = first.get_impl().GetId();
   const auto& second_id = second.get_impl().GetId();
   return first_id == second_id;
}

void
Win32EdgeEngine::Embed(bool debug, msg_cb_t cb) {
   std::atomic_flag flag = ATOMIC_FLAG_INIT;
   flag.test_and_set();

   std::wstring current_exe_path;
   current_exe_path.reserve(MAX_PATH);
   current_exe_path.resize(
      GetModuleFileNameW(nullptr, current_exe_path.data(), current_exe_path.capacity())
   );

   std::wstring current_exe_name{PathFindFileNameW(current_exe_path.c_str())};

   com_handler_ =
      new Webview2ComHandler(cb, [&](ICoreWebView2Controller* controller, ICoreWebView2* webview) {
         if (!controller || !webview) {
            flag.clear();
            return;
         }
         controller->AddRef();
         webview->AddRef();
         controller_ = controller;
         webview_    = webview;
         flag.clear();
      });

   com_handler_->SetAttemptHandler([&] {
      return webview2_loader_.create_environment_with_options(
         nullptr,
         wuser_data_dir_.size() ? wuser_data_dir_.c_str() : nullptr,
         options_.Get(),
         com_handler_
      );
   });
   com_handler_->HandleWindow(widget_);

   // Pump the message loop until WebView2 has finished initialization.
   bool got_quit_msg = false;
   MSG  msg;
   while (flag.test_and_set() && GetMessageW(&msg, nullptr, 0, 0) >= 0) {
      if (msg.message == WM_QUIT) {
         got_quit_msg = true;
         break;
      }
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
   }
   if (got_quit_msg) {
      throw Exception{error_t::WEBVIEW_ERROR_CANCELED};
   }
   if (!controller_ || !webview_) {
      throw Exception{error_t::WEBVIEW_ERROR_INVALID_STATE};
   }
   ICoreWebView2Settings* settings = nullptr;
   auto                   res      = webview_->get_Settings(&settings);
   if (res != S_OK) {
      throw Exception{error_t::WEBVIEW_ERROR_UNSPECIFIED, "get_Settings failed"};
   }
   res = settings->put_AreDevToolsEnabled(debug ? true : false);
   if (res != S_OK) {
      throw Exception{error_t::WEBVIEW_ERROR_UNSPECIFIED, "put_AreDevToolsEnabled failed"};
   }
   res = settings->put_IsStatusBarEnabled(false);
   if (res != S_OK) {
      throw Exception{error_t::WEBVIEW_ERROR_UNSPECIFIED, "put_IsStatusBarEnabled failed"};
   }
   AddInitScript(R"_(function(message) {
   return window.chrome.webview.postMessage(message);
})_");
   ResizeWebview();
   controller_->put_IsVisible(true);
   ShowWindow(widget_, SW_SHOW);
   UpdateWindow(widget_);
   if (owns_window_) {
      FocusWebview();
   }
}

void
Win32EdgeEngine::ResizeWidget() {
   if (widget_) {
      RECT r{};
      if (GetClientRect(GetParent(widget_), &r)) {
         MoveWindow(widget_, r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE);
      }
   }
}

void
Win32EdgeEngine::ResizeWebview() {
   if (widget_ && controller_) {
      RECT bounds{};
      if (GetClientRect(widget_, &bounds)) {
         controller_->put_Bounds(bounds);
      }
   }
}

void
Win32EdgeEngine::FocusWebview() {
   if (controller_) {
      controller_->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
   }
}

bool
Win32EdgeEngine::Webview2Available() const noexcept {
   LPWSTR version_info = nullptr;
   auto   res          = webview2_loader_.GetAvailableBrowserVersionString(nullptr, &version_info);
   // The result will be equal to HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)
   // if the WebView2 runtime is not installed.
   auto ok = SUCCEEDED(res) && version_info;
   if (version_info) {
      CoTaskMemFree(version_info);
   }
   return ok;
}

void
Win32EdgeEngine::OnDpiChanged(int dpi) {
   auto scaled_size = GetScaledSize(dpi_, dpi);
   auto frame_size  = MakeWindowFrameSize(window_, scaled_size.cx, scaled_size.cy, dpi);
   SetWindowPos(
      window_,
      nullptr,
      0,
      0,
      frame_size.cx,
      frame_size.cy,
      SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE | SWP_FRAMECHANGED
   );
   dpi_ = dpi;
}

SIZE
Win32EdgeEngine::GetSizeImpl() const {
   RECT bounds;
   GetClientRect(window_, &bounds);
   auto width  = bounds.right - bounds.left;
   auto height = bounds.bottom - bounds.top;
   return {width, height};
}

POINT
Win32EdgeEngine::GetPosImpl() const {
   RECT bounds;
   GetWindowRect(window_, &bounds);
   return {bounds.left, bounds.top};
}

SIZE
Win32EdgeEngine::GetScaledSize(int from_dpi, int to_dpi) const {
   auto size = GetSizeImpl();
   return ScaleSize(size.cx, size.cy, from_dpi, to_dpi);
}

void
Win32EdgeEngine::OnSystemSettingsChange(const wchar_t* area) {
   // Detect light/dark mode change in system.
   if (lstrcmpW(area, L"ImmersiveColorSet") == 0) {
      ApplyWindowTheme(window_);
   }
}

// Blocks while depleting the run loop of events.
void
Win32EdgeEngine::DepleteRunLoopEventQueue() {
   bool done{};
   Dispatch([&done] { done = true; });
   while (!done) {
      MSG msg;
      if (GetMessageW(&msg, nullptr, 0, 0) > 0) {
         TranslateMessage(&msg);
         DispatchMessageW(&msg);
      }
   }
}

}  // namespace detail
}  // namespace webview

#endif  // defined(WEBVIEW_PLATFORM_WINDOWS) && defined(WEBVIEW_EDGE)
