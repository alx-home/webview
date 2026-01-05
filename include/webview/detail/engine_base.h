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

#pragma once

#include "../http.h"
#include "promise/promise.h"
#include "user_script.h"
#include "utils/Nonce.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <format>
#include <functional>
#include <list>
#include <memory>
#include <shared_mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>

namespace webview {

struct Pos {
   int x_;
   int y_;
};

struct Size {
   int width_;
   int height_;
};

struct Bounds
   : Pos
   , Size {
   bool Contains(Pos const& pos) const;
};

/// Window size hints
enum class Hint {
   /// Width and height are default size.
   NONE,
   /// Width and height are minimum bounds.
   MIN,
   /// Width and height are maximum bounds.
   MAX,
   /// Window size can not be changed by a user.
   FIXED,
   /// Window without frame, user can't change size & position.
   STATIC
};

struct MakeDeferred {
   virtual ~MakeDeferred() = default;

   virtual void operator()()               = 0;
   virtual void Complete(http::response_t) = 0;
};
using url_handler_t = std::function<
  std::optional<http::response_t>(http::request_t const& request, std::unique_ptr<MakeDeferred>)>;
using binding_t         = std::function<void(std::string_view id, std::string_view args)>;
using reverse_binding_t = std::function<void(bool error, std::string_view result)>;

class Webview {
public:
   Webview(std::function<void()> on_terminate = []() constexpr {});
   virtual ~Webview() = default;

   void Navigate(std::string_view url);

   virtual void RegisterUrlHandler(std::string_view filter, url_handler_t handler) = 0;
   virtual void
   RegisterUrlHandlers(std::vector<std::string_view> const& filters, url_handler_t handler) = 0;

   template <class PROMISE>
   void Bind(std::string_view name, PROMISE&& promise);
   void Unbind(std::string_view name);

   template <class RETURN, class... ARGS>
   auto& Call(std::string_view name, ARGS&&... args);

   virtual void Run()                             = 0;
   virtual void Terminate()                       = 0;
   virtual void Dispatch(std::function<void()> f) = 0;
   virtual void SetTitle(std::string_view title)  = 0;

   virtual void SetSize(int width, int height, Hint hints) = 0;
   virtual void SetPos(int x, int y)                       = 0;
   virtual void SetHtml(std::string_view html)             = 0;

   virtual int Width() const  = 0;
   virtual int Height() const = 0;

   virtual Size   GetSize() const   = 0;
   virtual Pos    GetPos() const    = 0;
   virtual Bounds GetBounds() const = 0;

   virtual void ToForeground() = 0;

   virtual void Hide() const    = 0;
   virtual bool Hidden() const  = 0;
   virtual void Restore() const = 0;
   virtual void Show() const    = 0;

   virtual void SetTitleBarColor(uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha) = 0;
   virtual void SetBackgroung(uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha)    = 0;
   virtual void SetTopMost()                                                              = 0;

   void Init(std::string_view js);

#if _MSC_VER == 1929
   template <class... ARGS>
   void Eval(std::string_view js, ARGS&&... args);
#else
   template <class... ARGS>
   constexpr void Eval(std::format_string<ARGS...> js, ARGS&&... args);
#endif
   virtual void Eval(std::string_view js) = 0;

   virtual void OpenDevTools()           = 0;
   virtual void InstallResourceHandler() = 0;

   virtual user_script* AddUserScript(std::string_view js);

protected:
   virtual void NavigateImpl(std::string_view url) = 0;

   virtual user_script AddUserScriptImpl(std::string_view js) = 0;
   virtual user_script*
                ReplaceUserScript(user_script const& old_script, std::string_view new_script_code);
   virtual void RemoveAllUserScript(std::list<user_script> const& scripts)               = 0;
   virtual bool AreUserScriptsEqual(user_script const& first, user_script const& second) = 0;

   void        ReplaceBindScript();
   void        AddInitScript(std::string_view post_fn);
   std::string CreateInitScript(std::string_view post_fn);
   std::string CreateBindScript();

   virtual void OnMessage(std::string_view msg);
   virtual void OnWindowCreated();

   virtual void OnWindowDestroyed(bool skip_termination = false);

   std::string_view GetNonce() const;

private:
   struct Promises;

protected:
   using SLock = std::unique_lock<std::shared_mutex>;
   SLock Lock();
   void  CleanPromises(SLock&& lock);

   bool stop_{false};

private:
   static std::atomic_uint& WindowRefCount();
   static unsigned int      IncWindowCount();
   static unsigned int      DecWindowCount();

   template <class PROMISE, class... ARGS>
   auto MakeWrapper(PROMISE&& promise, std::string_view id, ARGS&&... args);

   using bindings_t = std::unordered_map<std::string, std::shared_ptr<binding_t>>;
   bindings_t bindings_{};
   using reverse_bindings_t = std::unordered_map<std::string, std::shared_ptr<reverse_binding_t>>;
   reverse_bindings_t reverse_bindings_{};

   user_script*           bind_script_{};
   std::list<user_script> user_scripts_{};
   std::function<void()>  on_terminate_{};

   std::string nonce_{utils::Nonce() + utils::Nonce()};
   std::size_t next_id_{0};

   struct Promises {
      using Id = std::string;

      struct Cleaner {
         template <class PROMISE>
         Cleaner(
           std::string_view name,
           std::unique_ptr<PROMISE>,
           std::shared_ptr<promise::Reject> reject = nullptr
         );

         bool await_ready();
         void await_suspend(std::coroutine_handle<>);
         void await_resume();

         void Detach() &&;

         template <class EXCEPTION, class... ARGS>
         void Reject(ARGS&&... args);

         template <class PROMISE>
         PROMISE& Promise();

      private:
         std::string                        name_{};
         std::unique_ptr<promise::VPromise> promise_{};
         std::shared_ptr<promise::Reject>   reject_{nullptr};
         bool                               detached_{false};

         promise::VPromise::Awaitable* awaitable_{nullptr};
      };

      std::unordered_map<Id, Cleaner> handles_{};
   };

   std::shared_mutex         mutex_{};
   std::unique_ptr<Promises> promises_{std::make_unique<Promises>()};
};

}  // namespace webview

#include "engine_base.inl"