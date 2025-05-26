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
#include "utils/Nonce.h"
#include "engine_base.h"

#include <json/json.inl>
#include <json/exceptions.h>
#include <promise/promise.h>

#include <exception>
#include <format>
#include <memory>
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
      MakeReject<EXCEPTION>(*reject_, std::forward<ARGS>(args)...);
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
             .Then([id = std::string{id}, this]() -> ::Promise<void> {
                co_return Dispatch([id = js::Stringify(id), this]() {
                   Eval(R"(window.__webview__.onReply({}, false, undefined, "{}"))", id, nonce_);
                });
             });
        } else {
           return MakePromise(promise, std::forward<ARGS>(args)...)
             .Then([id{std::string{id}}, this](return_t const& result) -> ::Promise<void> {
                co_return Dispatch([id = js::Stringify(id), result = js::Stringify(result), this](
                                   ) {
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
       .Catch(
         [id = std::string{id}, this](js::SerializableException const& exc) -> ::Promise<void> {
            co_return Dispatch([id = js::Stringify(id), exception = exc.Stringify(), this]() {
               Eval(
                 R"(window.__webview__.onReply({}, true, {}, "{}"))",
                 id,
                 js::Stringify(exception),
                 nonce_
               );
            });
         }
       )
       .Catch([id = std::string{id}, this](std::exception const& exc) -> ::Promise<void> {
          co_return Dispatch([id        = js::Stringify(id),
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
       .Catch([id = std::string{id}, this](std::exception_ptr) -> ::Promise<void> {
          co_return Dispatch([id = js::Stringify(id), this]() {
             Eval(R"(window.__webview__.onReply({}, true, "unknown exception", "{}"))", id, nonce_);
          });
       })
       .Then([this, id = std::string{id}]() -> ::Promise<void> {
          // Cleanup

          Dispatch([this, id]() constexpr {
             assert(promises_);
             auto elem = promises_->handles_.find("bind_" + id);

             if (elem != promises_->handles_.end()) {
                // Detach the promise, as there is a slight chance that dispatch might be
                // executed before the promise completes
                std::move(elem->second).Detach();

                promises_->handles_.erase(elem);
             } else {
                assert(false);
             }
          });
          co_return;
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

               if (!promises_) {
                  return Dispatch([id = js::Stringify(id), this]() {
                     Eval(
                       R"(window.__webview__.onReply({}, true, {}, "{}"))",
                       id,
                       js::Stringify(std::string_view{"Terminated webview !"}),
                       nonce_
                     );
                  });
               }

               try {
                  auto args = [&]() constexpr {
                     if constexpr (std::tuple_size_v<args_t>) {
                        return js::Parse<args_t>(js_args);
                     } else {
                        return std::tuple{};
                     }
                  }();

                  ::Promise<void> wrapper{
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
                      Promises::Cleaner{name, std::make_unique<::Promise<void>>(std::move(wrapper))}
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
   auto const id = std::to_string(++next_id_);

   auto [ppromise, resolve, reject] = promise::Pure<RETURN>();

   auto done = std::make_shared<bool>(false);

   auto promise = [this, id, &ppromise, done]() constexpr {
      if constexpr (std::is_void_v<std::remove_cvref_t<RETURN>>) {
         return std::move(ppromise).Then([this, id, done = std::move(done)]() -> ::Promise<void> {
            // Cleanup

            Dispatch([this, id, done = std::move(done)]() constexpr {
               assert(promises_);
               *done     = true;
               auto elem = promises_->handles_.find("call_" + id);

               if (elem != promises_->handles_.end()) {
                  // Detach the promise, as there is a slight chance that dispatch might be
                  // executed before the promise completes
                  std::move(elem->second).Detach();

                  promises_->handles_.erase(elem);
               } else {
                  assert(false);
               }
            });

            co_return;
         });
      } else {
         return std::move(ppromise).Then(
           [this, id, done = std::move(done)](RETURN const& result) -> ::Promise<RETURN> {
              // Cleanup

              Dispatch([this, id, done = std::move(done)]() constexpr {
                 assert(promises_);
                 *done     = true;
                 auto elem = promises_->handles_.find("call_" + id);

                 if (elem != promises_->handles_.end()) {
                    // Detach the promise, as there is a slight chance that dispatch might be
                    // executed before the promise completes
                    std::move(elem->second).Detach();

                    promises_->handles_.erase(elem);
                 } else {
                    assert(false);
                 }
              });

              co_return result;
           }
         );
      }
   }();

   auto const binding =
     std::make_shared<reverse_binding_t>([reject, resolve](bool error, std::string_view result) {
        if (error) {
           MakeReject<Exception>(*reject, error_t::WEBVIEW_ERROR_REJECT, result);
        } else {
           if constexpr (std::is_void_v<std::remove_cvref_t<RETURN>>) {
              (*resolve)();
           } else {
              (*resolve)(js::Parse<RETURN>(result));
           }
        }
     });

   std::tuple arguments{std::forward<ARGS>(args)...};

   auto  promise_ptr = std::make_unique<std::remove_cvref_t<decltype(promise)>>(std::move(promise));
   auto& promise_ref = *promise_ptr;

   auto const promise_holder = std::make_shared<decltype(promise_ptr)>(std::move(promise_ptr));

   Dispatch([this,
             id,
             arguments = std::move(arguments),
             name      = std::string{name},
             done,
             binding,
             reject,
             promise_holder = std::move(promise_holder)]() constexpr {
      if (!*done) {
         reverse_bindings_.emplace(id, std::move(binding));

         Promises::Cleaner cleaner{name, std::move(*promise_holder), reject};
         auto const& [_, emplaced] = promises_->handles_.emplace("call_" + id, std::move(cleaner));
         assert(emplaced);

         Eval(
           R"(if (window.__webview__) {{
        window.__webview__.reverseCall({}, "{}", "{}", {})
    }})",
           js::Stringify(name),
           id,
           nonce_,
           js::Stringify(arguments)
         );
      }
   });

   if (!promises_) {
      // Webview terminated : MakeReject may not be called as Dispatch won't be ever depleted
      throw Exception(error_t::WEBVIEW_ERROR_CANCELED, "Webview is terminating");
   }

   return promise_ref;
}

template <class... ARGS>
constexpr void
Webview::Eval(std::format_string<ARGS...> js, ARGS&&... args) {
   Eval(std::format(std::move(js), std::forward<ARGS>(args)...));
}

}  // namespace webview
