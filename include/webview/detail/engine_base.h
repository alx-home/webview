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
#include "user_script.h"

#include <unordered_map>
#include <atomic>
#include <functional>
#include <string>

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

using url_handler_t = std::function<http::response_t(http::request_t const& request)>;
using binding_t     = std::function<void(std::string_view id, std::string_view args)>;

class Webview {
public:
   Webview(std::function<void()> on_terminate = []() constexpr {});
   virtual ~Webview() = default;

   void Navigate(std::string_view url);

   virtual void RegisterUrlHandler(std::string const& filter, url_handler_t&& handler) = 0;

   template <class PROMISE> void Bind(std::string_view name, PROMISE&& promise);
   void                          Unbind(std::string_view name);

   virtual void Run()                               = 0;
   virtual void Terminate()                         = 0;
   virtual void Dispatch(std::function<void()>&& f) = 0;
   virtual void SetTitle(std::string_view title)    = 0;

   virtual void SetSize(int width, int height, Hint hints) = 0;
   virtual void SetPos(int x, int y)                       = 0;
   virtual void SetHtml(std::string_view html)             = 0;

   virtual int Width() const  = 0;
   virtual int Height() const = 0;

   virtual Size   GetSize() const   = 0;
   virtual Pos    GetPos() const    = 0;
   virtual Bounds GetBounds() const = 0;

   virtual void ToForeground() = 0;

   virtual void Hide()    = 0;
   virtual void Restore() = 0;
   virtual void Show()    = 0;

   virtual void SetTitleBarColor(uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha) = 0;
   virtual void SetBackgroung(uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha)    = 0;
   virtual void SetTopMost()                                                              = 0;

   void Init(std::string_view js);

   virtual void Eval(std::string_view js) = 0;
   virtual void OpenDevTools()            = 0;
   virtual void InstallResourceHandler()  = 0;

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

private:
   static std::atomic_uint& WindowRefCount();
   static unsigned int      IncWindowCount();
   static unsigned int      DecWindowCount();

   using bindings_t = std::unordered_map<std::string, std::shared_ptr<binding_t>>;
   bindings_t             bindings_{};
   user_script*           bind_script_{};
   std::list<user_script> user_scripts_;
   std::function<void()>  on_terminate_;
};

}  // namespace webview

#include "engine_base.inl"