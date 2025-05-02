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

#include <json/json.h>
#include <promise/promise.h>

#include <exception>
#include <format>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string_view>
#include <tuple>

namespace webview {

template <class PROMISE>
void
Webview::Bind(std::string_view name, PROMISE&& promise) {
   if (!bindings_
          .emplace(
            name,
            std::make_shared<binding_t>([this, promise = std::forward<PROMISE>(promise)](
                                          std::string_view id, std::string_view js_args
                                        ) {
               using return_t = promise::return_t<promise::return_t<decltype(promise)>>;
               using args_t   = promise::args_t<decltype(promise)>;

               std::shared_lock lock{promises_.done_mutex_};
               if (promises_.done_) {
                  return Dispatch([id = js::Serialize(id), this]() {
                     Eval(
                       "window.__webview__.onReply(" + id + ", 1, "
                       + js::Serialize<std::string_view>("Terminated webview !") + ")"
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

                  std::apply(
                    [&]<class... ARGS>(ARGS&&... args) constexpr {
                       bool ended = false;

                       ::Promise<void> wrapper{
                         [this, &promise, &id, &args...]() constexpr {
                            if constexpr (std::is_void_v<return_t>) {
                               return MakePromise(promise, std::forward<ARGS>(args)...)
                                 .Then([id = std::string{id}, this]() -> ::Promise<void> {
                                    co_return Dispatch([id = js::Serialize(id), this]() {
                                       Eval("window.__webview__.onReply(" + id + ", 0, undefined)");
                                    });
                                 });
                            } else {
                               return MakePromise(promise, std::forward<ARGS>(args)...)
                                 .Then(
                                   [id{std::string{id}},
                                    this](return_t const& result) -> ::Promise<void> {
                                      co_return Dispatch([id     = js::Serialize(id),
                                                          result = js::Serialize(result),
                                                          this]() {
                                         Eval(
                                           "window.__webview__.onReply(" + id + ", 0, "
                                           + js::Serialize(result) + ")"
                                         );
                                      });
                                   }
                                 );
                            }
                         }()
                           .Catch(
                             [id = std::string{id},
                              this](js::SerializableException const& exc) -> ::Promise<void> {
                                co_return Dispatch(
                                  [id = js::Serialize(id), exception = exc.Serialize(), this]() {
                                     Eval(
                                       "window.__webview__.onReply(" + id + ", 1, "
                                       + js::Serialize(exception) + ")"
                                     );
                                  }
                                );
                             }
                           )
                           .Catch(
                             [id = std::string{id},
                              this](std::exception const& exc) -> ::Promise<void> {
                                co_return Dispatch([id = js::Serialize(id),
                                                    exception =
                                                      js::Serialize<std::string_view>(exc.what()),
                                                    this]() {
                                   Eval(
                                     "window.__webview__.onReply(" + id + ", 1, "
                                     + js::Serialize(exception) + ")"
                                   );
                                });
                             }
                           )
                           .Catch(
                             [id = std::string{id}, this](std::exception_ptr) -> ::Promise<void> {
                                co_return Dispatch([id = js::Serialize(id), this]() {
                                   Eval(
                                     "window.__webview__.onReply(" + id + ", 1, "
                                     + js::Serialize<std::string_view>(R"("unknown exception")")
                                     + ")"
                                   );
                                });
                             }
                           )
                           .Then([this, &ended, id = std::string{id}]() -> ::Promise<void> {
                              // Cleanup

                              std::unique_lock lock{promises_.mutex_};
                              auto             elem = promises_.handles_.find(id);

                              if (elem != promises_.handles_.end()) {
                                 static_cast<::Promise<void>&&>(*elem->second.release()).Detach();

                                 promises_.handles_.erase(elem);
                              } else {
                                 // Not Saved
                                 ended = true;
                              }

                              co_return;
                           })
                       };

                       // Save promise if not ended
                       std::unique_lock lock{promises_.mutex_};

                       if (!ended) {
                          auto const& [_, emplaced] = promises_.handles_.emplace(
                            id, static_cast<::Promise<void>&&>(wrapper).ToPointer()
                          );
                          assert(emplaced);
                       }
                    },
                    args
                  );
               } catch (js::SerializableException const& exc) {
                  Dispatch([id = js::Serialize(id), exception = exc.Serialize(), this]() {
                     Eval(
                       "window.__webview__.onReply(" + id + ", 1, " + js::Serialize(exception) + ")"
                     );
                  });
               } catch (std::exception const& exc) {
                  Dispatch([id        = js::Serialize(id),
                            exception = js::Serialize(std::string_view{exc.what()}),
                            this]() {
                     Eval(
                       "window.__webview__.onReply(" + id + ", 1, " + js::Serialize(exception) + ")"
                     );
                  });
               } catch (...) {
                  Dispatch([id = js::Serialize(id), this]() {
                     Eval(
                       "window.__webview__.onReply(" + id + ", 1, "
                       + js::Serialize<std::string_view>(R"("unknown exception")") + ")"
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
   Eval(std::format(
     R"(if (window.__webview__) {{
       window.__webview__.onBind({})
    }})",
     js::Serialize(name)
   ));
}
}  // namespace webview
