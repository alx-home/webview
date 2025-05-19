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
#include <tuple>
#include <utility>
#include <variant>
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
   Eval(
     R"(if (window.__webview__) {{
    window.__webview__.onUnbind({}, "{}")
}})",
     js::Stringify(name),
     nonce_
   );
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

      Webview_.prototype.post = function(message, nonce) {{
         return ({0})(message, nonce);
      }};

      Webview_.prototype.call = function(method, nonce) {{
         if (nonce != "{1}") {{
            throw new Error('Invalid nonce \"' + nonce + '\"');
         }}

         var _id = generateId();
         var _params = Array.prototype.slice.call(arguments, 2);
         var promise = new Promise(function(resolve, reject) {{
            _promises[_id] = {{ resolve, reject }};
         }});

         this.post(JSON.stringify({{
               nonce: nonce,
               reverse: false,
               id: _id,
               method: method,
               params: JSON.stringify(_params)
            }}),
            nonce);

         return promise;
      }};

      Webview_.prototype.reverseCall = function(method, _id, nonce, _params) {{
         if (nonce != "{1}") {{
            throw new Error('Invalid nonce \"' + nonce + '\"');
         }}

         if (!window.hasOwnProperty(method)) {{
            this.post(JSON.stringify({{
                  nonce: nonce,
                  reverse: true,
                  id: _id,
                  method: method,
                  error: true,
                  result: JSON.stringify('Property \"' + method + '\" doesn\'t exists')
               }}),
               nonce);
         }} else {{
            window[method].apply(null, _params).then((result) => {{
               this.post(JSON.stringify({{
                     nonce: nonce,
                     reverse: true,
                     id: _id,
                     method: method,
                     error: false,
                     result: JSON.stringify(result)
                  }}),
                  nonce);
            }}).catch((error) => {{
               this.post(JSON.stringify({{
                     nonce: nonce,
                     reverse: true,
                     id: _id,
                     method: method,
                     error: true,
                     result: JSON.stringify(error)
                  }}),
                  nonce);
            }});
         }}
      }}

      Webview_.prototype.onReply = function(id, error, result, nonce) {{
         if (nonce != "{1}") {{
            throw new Error('Invalid nonce \"' + nonce + '\"');
         }}

         var promise = _promises[id];
         if (result !== undefined) {{
            try {{
               result = JSON.parse(result);
            }} catch (e) {{
               promise.reject(new Error("Failed to Parse binding result as JSON"));
               return;
            }}
         }}

         if (error) {{
            promise.reject(result);
         }} else {{
            promise.resolve(result);
         }}
      }};

      Webview_.prototype.onBind = function(name, nonce) {{
         if (nonce != "{1}") {{
            throw new Error('Invalid nonce \"' + nonce + '\"');
         }}

         if (window.hasOwnProperty(name)) {{
            throw new Error('Property \"' + name + '\" already Exists');
         }}

         window[name] = (function() {{
            var params = [name, nonce].concat(Array.prototype.slice.call(arguments));
            return Webview_.prototype.call.apply(this, params);
         }}).bind(this);
      }};

      Webview_.prototype.onUnbind = function(name, nonce) {{
         if (nonce != "{1}") {{
            throw new Error('Invalid nonce \"' + nonce + '\"');
         }}
         if (!window.hasOwnProperty(name)) {{
            throw new Error('Property \"' + name + '\" does not exist');
         }}

         delete window[name];
      }};

      return Webview_;
   }})();
  
   window.__webview__ = new Webview();
}})())",
     post_fn,
     nonce_
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
      js_names += js::Stringify(binding.first);
   }
   js_names += "]";

   return std::format(
     R"((function() {{
    'use strict';
    var methods = {};

    methods.forEach(function(name) {{
        window.__webview__.onBind(name, "{}");
    }});
}})())",
     js_names,
     nonce_
   );
}

struct Header {
   std::string nonce_;
   bool        reverse_;
   std::string id_;
   std::string name_;

   static constexpr js::Proto PROTOTYPE{
     js::_{"nonce", &Header::nonce_},
     js::_{"reverse", &Header::reverse_},
     js::_{"id", &Header::id_},
     js::_{"method", &Header::name_},
   };
};

struct ReplyMessage : Header {
   std::string params_;

   static constexpr js::Proto PROTOTYPE{
     js::Extend{
       Header::PROTOTYPE,
       js::_{"params", &ReplyMessage::params_},
     },
   };
};

struct ReverseMessage : Header {
   bool                       error_;
   std::optional<std::string> result_;

   static constexpr js::Proto PROTOTYPE{
     js::Extend{
       Header::PROTOTYPE,
       js::_{"error", &ReverseMessage::error_},
       js::_{"result", &ReverseMessage::result_},
     },
   };
};

using Message = std::variant<ReverseMessage, ReplyMessage>;

void
Webview::OnMessage(std::string_view msg_) {
   auto const check_header = [this](auto msg) constexpr {
      if (msg.nonce_ != nonce_) {
         std::cerr << "Invalid nonce !" << std::endl;
         // ignoring
         return false;
      }
      return true;
   };

   auto vmsg = js::Parse<Message>(msg_);

   if (std::holds_alternative<ReplyMessage>(vmsg)) {
      auto const& msg = std::get<ReplyMessage>(vmsg);
      if (check_header(msg)) {
         assert(!msg.reverse_);

         auto const& create_promise = bindings_.at(std::string{msg.name_});

         Dispatch([create_promise, id = std::string{msg.id_}, params = std::string{msg.params_}]() {
            (*create_promise)(id, params);
         });
      }
   } else {
      auto const& msg = std::get<ReverseMessage>(vmsg);
      if (check_header(msg)) {
         assert(msg.reverse_);

         if (auto elem = reverse_bindings_.find(std::string{msg.id_});
             elem != reverse_bindings_.end()) {
            auto make_reply = std::move(elem->second);
            reverse_bindings_.erase(elem);

            Dispatch([make_reply,
                      error  = msg.error_,
                      result = std::string{msg.result_ ? *msg.result_ : ""}]() {
               (*make_reply)(error, result);
            });
         }
      }
   }
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

std::string_view
Webview::GetNonce() const {
   return nonce_;
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

bool
Webview::Promises::Cleaner::await_ready() {
   if (detached_) {
      return true;
   } else {
      return promise_->VAwait().await_ready();
   }
}
void
Webview::Promises::Cleaner::await_suspend(std::coroutine_handle<> h) {
   if (!detached_) {
      promise_->VAwait().await_suspend(h);
   }
}
void
Webview::Promises::Cleaner::await_resume() {
   if (!detached_) {
      promise_->VAwait().await_resume();
   }
}

void
Webview::Promises::Cleaner::Detach() && {
   assert(!detached_);
   detached_    = true;
   reject_      = nullptr;
   auto promise = std::move(promise_);
   std::move(*promise).VDetach();
}

Webview::PromisesCleaner::PromisesCleaner(std::unique_ptr<Promises> promises)
   : promises_(std::move(promises))
   , prom_waiter_(MakePromise([this]() -> Promise<void> {
      struct Done {
         std::condition_variable& cv_;
         std::mutex&              mutex_;
         bool&                    done_;

         ~Done() {
            std::unique_lock lock{mutex_};
            done_ = true;
            cv_.notify_all();
         }
      } _{.cv_ = cv_, .mutex_ = mutex_, .done_ = done_};

      for (auto& handle : promises_->handles_) {
         try {
            handle.second.Reject<Exception>(
              error_t::WEBVIEW_ERROR_CANCELED, "Webview is terminating"
            );
            co_await handle.second;
         } catch (std::exception const& e) {
            std::cerr << e.what() << std::endl;
         }
      }

      co_return;
   })) {}

Webview::PromisesCleaner::~PromisesCleaner() {
   std::unique_lock lock{mutex_};
   if (!done_) {
      cv_.wait(lock);
   }

   assert(prom_waiter_.Done());
   assert(!prom_waiter_.Exception());
}

Webview::PromisesCleaner
Webview::CleanPromises() {
   assert(promises_);
   return PromisesCleaner{std::move(promises_)};
}

}  // namespace webview
