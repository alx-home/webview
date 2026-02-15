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

#include "../errors.h"
#include "engine_base.h"
#include "utils/Nonce.h"
#include "utils/Scoped.h"

#include <json/exceptions.h>
#include <json/json.inl>
#include <promise/promise.h>

#include <exception>
#include <format>
#include <memory>
#include <shared_mutex>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace webview {
template <class PROMISE>
Webview::Promises::Cleaner::Cleaner(
  std::string_view                 name,
  std::unique_ptr<PROMISE>         promise,
  std::shared_ptr<promise::Reject> reject
)
   : name_{name}
   , promise_(std::move(promise))
   , reject_{std::move(reject)} {}

template <class EXCEPTION, class... ARGS>
void
Webview::Promises::Cleaner::Reject(ARGS&&... args) {
   assert(!detached_);
   if (reject_) {
      reject_->Apply<EXCEPTION>(std::forward<ARGS>(args)...);
   }
}

template <class PROMISE>
PROMISE&
Webview::Promises::Cleaner::Promise() {
   return static_cast<PROMISE&>(*promise_);
}

template <class PROMISE, class... ARGS>
auto
Webview::MakeWrapper(PROMISE&& promise, std::string_view id, ARGS&&... args) {
   using return_t = promise::return_t<promise::return_t<PROMISE>>;

   return
     [this, &promise, &id, &args...]() constexpr {
        if constexpr (std::is_void_v<return_t>) {
           return MakePromise(promise, std::forward<ARGS>(args)...)
             .Then([id = std::string{id}, this]() constexpr {
                Dispatch([id = js::Stringify(id), this]() {
                   Eval(R"(window.__webview__.onReply({}, false, undefined, "{}"))", id, nonce_);
                });
             });
        } else {
           return MakePromise(promise, std::forward<ARGS>(args)...)
             .Then([id{std::string{id}}, this](return_t const& result) constexpr {
                Dispatch([id = js::Stringify(id), result = js::Stringify(result), this]() {
                   Eval(
                     R"(window.__webview__.onReply({}, false, {}, "{}"))",
                     id,
                     js::Stringify(result),
                     nonce_
                   );
                });
             });
        }
     }()
       .Catch([id = std::string{id}, this](js::SerializableException const& exc) constexpr {
          Dispatch([id = js::Stringify(id), exception = exc.Stringify(), this]() {
             Eval(
               R"(window.__webview__.onReply({}, true, {}, "{}"))",
               id,
               js::Stringify(exception),
               nonce_
             );
          });
       })
       .Catch([id = std::string{id}, this](std::exception const& exc) constexpr {
          Dispatch([id        = js::Stringify(id),
                    exception = js::Stringify(std::string_view{exc.what()}),
                    this]() {
             Eval(
               R"(window.__webview__.onReply({}, true, {}, "{}"))",
               id,
               js::Stringify(exception),
               nonce_
             );
          });
       })
       .Catch([id = std::string{id}, this](std::exception_ptr) constexpr {
          Dispatch([id = js::Stringify(id), this]() {
             Eval(R"(window.__webview__.onReply({}, true, "unknown exception", "{}"))", id, nonce_);
          });
       })
       .Then([this, id = std::string{id}]() constexpr {
          // Cleanup

          Dispatch([this, id]() constexpr {
             assert(promises_);
             auto elem = promises_->handles_.find("bind_" + id);

             if (elem != promises_->handles_.end()) {
                // Detach the promise, as there is a slight chance that dispatch
                // might be executed before the promise completes
                std::move(elem->second).Detach();

                promises_->handles_.erase(elem);
             } else {
                assert(false);
             }
          });
       });
}

template <class PROMISE>
void
Webview::Bind(std::string_view name, PROMISE&& promise) {
   if (!bindings_
          .emplace(
            name,
            std::make_shared<binding_t>([this,
                                         name    = std::string{name},
                                         promise = std::forward<PROMISE>(promise
                                         )](std::string_view id, std::string_view js_args) {
               using args_t = promise::args_t<decltype(promise)>;

               if (stop_) {
                  return Dispatch([id = js::Stringify(id), this]() {
                     Eval(
                       R"(window.__webview__.onReply({}, true, {}, "{}"))",
                       id,
                       js::Stringify(std::string_view{"Terminated webview !"}),
                       nonce_
                     );
                  });
               }

               assert(promises_);

               try {
                  auto args = [&]() constexpr {
                     if constexpr (std::tuple_size_v<args_t>) {
                        return js::Parse<args_t>(js_args);
                     } else {
                        return std::tuple{};
                     }
                  }();

                  WPromise<void> wrapper{
                    std::apply(
                      [&]<class... ARGS>(ARGS&&... args) constexpr {
                         return MakeWrapper(promise, id, std::forward<ARGS>(args)...);
                      },
                      args
                    ),
                  };

#ifndef NDEBUG
                  auto const& [_, emplaced] =
#endif  // !NDEBUG
                    promises_->handles_.emplace(
                      "bind_" + std::string{id},
                      Promises::Cleaner{name, std::make_unique<WPromise<void>>(std::move(wrapper))}
                    );
                  assert(emplaced);

               } catch (js::SerializableException const& exc) {
                  Dispatch([id = js::Stringify(id), exception = exc.Stringify(), this]() {
                     Eval(
                       R"(window.__webview__.onReply({}, true, {}, "{}"))",
                       id,
                       js::Stringify(exception),
                       nonce_
                     );
                  });
               } catch (std::exception const& exc) {
                  Dispatch([id        = js::Stringify(id),
                            exception = js::Stringify(std::string_view{exc.what()}),
                            this]() {
                     Eval(
                       R"(window.__webview__.onReply({}, true, {}, "{}"))",
                       id,
                       js::Stringify(exception),
                       nonce_
                     );
                  });
               } catch (...) {
                  Dispatch([id = js::Stringify(id), this]() {
                     Eval(
                       R"(window.__webview__.onReply({}, true, "unknown exception", "{}"))",
                       id,
                       nonce_
                     );
                  });
               }
            })
          )
          .second) {
      throw Exception(error_t::WEBVIEW_ERROR_DUPLICATE, name);
   }

   ReplaceBindScript();

   // Notify that a binding was created if the init script has already
   // set things up.
   Eval(
     R"(if (window.__webview__) {{
       window.__webview__.onBind({}, "{}")
   }})",
     js::Stringify(name),
     nonce_
   );
}

template <class RETURN, class... ARGS>
auto&
Webview::Call(std::string_view name, ARGS&&... args) {
   std::shared_lock lock{mutex_};

   if (stop_) {
      // Webview terminated : reject may not be called as Dispatch won't be
      // ever depleted
      throw Exception(error_t::WEBVIEW_ERROR_CANCELED, "Webview is terminating");
   }
   assert(promises_);

   auto [promise, resolve, reject] = promise::Pure<RETURN>();

   auto const id = std::to_string(++next_id_);

   auto  promise_ptr = std::make_unique<std::remove_cvref_t<decltype(promise)>>(std::move(promise));
   auto& promise_ref = *promise_ptr;
   Dispatch([this,
             id,
             arguments = std::move(std::tuple{std::forward<ARGS>(args)...}),
             name      = std::string{name},
             reject,
             resolve,
             promise_holder =
               std::make_shared<decltype(promise_ptr)>(std::move(promise_ptr))]() constexpr {
      auto const binding = std::make_shared<reverse_binding_t>(
        [this, reject, resolve, id](bool error, std::string_view result) {
           ScopeExit _{[&]() constexpr {
              assert(promises_);
              auto elem = promises_->handles_.find("call_" + id);

              if (elem != promises_->handles_.end()) {
                 // Detach the promise, as there is a slight chance that dispatch
                 // might be executed before the promise completes
                 std::move(elem->second).Detach();

                 promises_->handles_.erase(elem);
              } else {
                 assert(false);
              }
           }};

           if (error) {
              reject->template Apply<Exception>(error_t::WEBVIEW_ERROR_REJECT, result);
           } else {
              if constexpr (std::is_void_v<std::remove_cvref_t<RETURN>>) {
                 (*resolve)();
              } else {
                 (*resolve)(js::Parse<RETURN>(result));
              }
           }
        }
      );

      reverse_bindings_.emplace(id, std::move(binding));

      Promises::Cleaner cleaner{name, std::move(*promise_holder), reject};
      [[maybe_unused]] auto const& [_, emplaced] =
        promises_->handles_.emplace("call_" + id, std::move(cleaner));
      assert(emplaced);

      Eval(
        R"(if (window.__webview__) {{
        window.__webview__.reverseCall({}, "{}", "{}", {})
    }})",
        js::Stringify(name),
        id,
        nonce_,
        js::Stringify(arguments)
      );  // Todo use callback form of Eval to avoid posting another task to get the result of Eval
   });

   return promise_ref;
}

#if _MSC_VER == 1929
template <class... ARGS>
void
Webview::Eval(std::string_view js, ARGS&&... args) {
   Eval(std::format(std::move(js), std::forward<ARGS>(args)...));
}
#else
template <class... ARGS>
constexpr void
Webview::Eval(std::format_string<ARGS...> js, ARGS&&... args) {
   return Eval(std::format(std::move(js), std::forward<ARGS>(args)...));
}
#endif

}  // namespace webview
