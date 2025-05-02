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

#include "detail/engine_base.h"
#include "detail/user_script.h"
#include "promise/promise.h"

#include "errors.h"

#include <condition_variable>
#include <exception>
#include <format>
#include <mutex>
#include <utility>
#include <json/json.h>

namespace webview {

bool
Bounds::Contains(Pos const& pos) const {
   return (pos.x_ >= x_) && (pos.x_ <= x_ + width_) && (pos.y_ >= y_) && (pos.y_ <= y_ + height_);
}

Webview::Webview(std::function<void()> on_terminate)
   : on_terminate_(std::move(on_terminate)) {}

void
Webview::Navigate(std::string_view url) {
   if (url.empty()) {
      return NavigateImpl("about:blank");
   }

   return NavigateImpl(url);
}

void
Webview::Unbind(std::string_view name) {
   if (bindings_.erase(std::string{name}) != 1) {
      throw Exception(ErrorInfo{
        error_t::WEBVIEW_ERROR_NOT_FOUND,
        std::string{"trying to unbind undefined binding "} + std::string{name}
      });
   }

   ReplaceBindScript();

   // Notify that a binding was created if the init script has already
   // set things up.
   Eval(std::format(
     R"(if (window.__webview__) {{
    window.__webview__.onUnbind({})
}})",
     js::Serialize(name)
   ));
}

void
Webview::Init(std::string_view js) {
   AddUserScript(js);
}

user_script*
Webview::AddUserScript(std::string_view js) {
   return std::addressof(*user_scripts_.emplace(user_scripts_.end(), AddUserScriptImpl(js)));
}

user_script*
Webview::ReplaceUserScript(const user_script& old_script, std::string_view new_script_code) {
   RemoveAllUserScript(user_scripts_);
   user_script* old_script_ptr{};
   for (auto& script : user_scripts_) {
      auto is_old_script = AreUserScriptsEqual(script, old_script);
      script             = AddUserScriptImpl(is_old_script ? new_script_code : script.GetCode());
      if (is_old_script) {
         old_script_ptr = std::addressof(script);
      }
   }
   return old_script_ptr;
}

void
Webview::ReplaceBindScript() {
   if (bind_script_) {
      bind_script_ = ReplaceUserScript(*bind_script_, CreateBindScript());
   } else {
      bind_script_ = AddUserScript(CreateBindScript());
   }
}

void
Webview::AddInitScript(std::string_view post_fn) {
   AddUserScript(CreateInitScript(post_fn));
}

std::string
Webview::CreateInitScript(std::string_view post_fn) {
   return std::format(
     R"(
(function() {{
   'use strict';

   function generateId() {{
      var crypto = window.crypto || window.msCrypto;
      var bytes = new Uint8Array(16);
      crypto.getRandomValues(bytes);
        
      return Array.prototype.slice.call(bytes).map(function(n) {{
         var s = n.toString(16);
         return ((s.length % 2) == 1 ? '0' : '') + s;
      }}).join('');
   }}
    
   var Webview = (function() {{
      var _promises = {{}};
      function Webview_() {{}}

      Webview_.prototype.post = function(message) {{
         return ({})(message);
      }};

      Webview_.prototype.call = function(method) {{
         var _id = generateId();
         var _params = Array.prototype.slice.call(arguments, 1);
         var promise = new Promise(function(resolve, reject) {{
            _promises[_id] = {{ resolve, reject }};
         }});

         this.post(JSON.stringify({{
            id: _id,
            method: method,
            params: JSON.stringify(_params)
         }}));

         return promise;
      }};

      Webview_.prototype.onReply = function(id, status, result) {{
         var promise = _promises[id];
         if (result !== undefined) {{
            try {{
               result = JSON.parse(result);
            }} catch (e) {{
               promise.reject(new Error("Failed to Parse binding result as JSON"));
               return;
            }}
         }}

         if (status === 0) {{
            promise.resolve(result);
         }} else {{
            promise.reject(result);
         }}
      }};

      Webview_.prototype.onBind = function(name) {{
         if (window.hasOwnProperty(name)) {{
            throw new Error('Property \"' + name + '\" already Exists');
         }}

         window[name] = (function() {{
            var params = [name].concat(Array.prototype.slice.call(arguments));
            return Webview_.prototype.call.apply(this, params);
         }}).bind(this);
      }};

      Webview_.prototype.onUnbind = function(name) {{
         if (!window.hasOwnProperty(name)) {{
            throw new Error('Property \"' + name + '\" does not exist');
         }}
         delete window[name];
      }};

      return Webview_;
   }})();
  
   window.__webview__ = new Webview();
}})())",
     post_fn
   );
}

std::string
Webview::CreateBindScript() {
   std::string js_names = "[";
   bool        first    = true;
   for (const auto& binding : bindings_) {
      if (first) {
         first = false;
      } else {
         js_names += ",";
      }
      js_names += js::Serialize(binding.first);
   }
   js_names += "]";

   return std::format(
     R"((function() {{
    'use strict';
    var methods = {};

    methods.forEach(function(name) {{
        window.__webview__.onBind(name);
    }});
}})())",
     js_names
   );
}

struct Message {
   std::string id_;
   std::string name_;
   std::string params_;

   static constexpr js::Proto PROTOTYPE{
     js::_{"id", &Message::id_},
     js::_{"method", &Message::name_},
     js::_{"params", &Message::params_}
   };
};

void
Webview::OnMessage(std::string_view msg_) {
   auto msg = js::Parse<Message>(msg_);

   auto const& create_promise = bindings_.at(std::string{msg.name_});

   Dispatch([create_promise, msg = std::move(msg)]() { (*create_promise)(msg.id_, msg.params_); });
}

void
Webview::OnWindowCreated() {
   IncWindowCount();
}

void
Webview::OnWindowDestroyed(bool skip_termination) {
   if ((DecWindowCount() <= 0) && !skip_termination) {
      Terminate();
   }

   on_terminate_();
}

std::atomic_uint&
Webview::WindowRefCount() {
   static std::atomic_uint s__ref_count{0};
   return s__ref_count;
}

unsigned int
Webview::IncWindowCount() {
   return ++WindowRefCount();
}

unsigned int
Webview::DecWindowCount() {
   auto& count = WindowRefCount();
   if (count > 0) {
      return --count;
   }
   return 0;
}

Webview::Promises::~Promises() {
   std::unique_lock _{done_mutex_};

   auto handles = [this]() constexpr {
      std::unique_lock lock{mutex_};
      done_        = true;
      auto handles = std::move(handles_);
      lock.unlock();

      return handles;
   }();

   std::condition_variable cv{};
   std::mutex              mutex;
   bool                    done = false;

   // await all unhandled promises
   auto prom_waiter = MakePromise([&handles, &cv, &done, &mutex]() -> Promise<void> {
      struct Done {
         std::condition_variable& cv_;
         std::mutex&              mutex_;
         bool&                    done_;

         ~Done() {
            std::unique_lock lock{mutex_};
            done_ = true;
            cv_.notify_all();
         }
      } _{.cv_ = cv, .mutex_ = mutex, .done_ = done};

      for (auto& handle : handles) {
         try {
            co_await handle.second->Await();
         } catch (std::exception const& e) {
            std::cerr << e.what() << std::endl;
         }
      }

      co_return;
   });

   std::unique_lock lock{mutex};
   if (!done) {
      cv.wait(lock);
   }

   assert(prom_waiter.Done());
   assert(!prom_waiter.Exception());
}
}  // namespace webview
