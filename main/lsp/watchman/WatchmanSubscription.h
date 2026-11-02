#ifndef RUBY_TYPER_LSP_WATCHMAN_WATCHMANSUBSCRIPTION_H
#define RUBY_TYPER_LSP_WATCHMAN_WATCHMANSUBSCRIPTION_H

#include <string>
#include <string_view>
#include <vector>

namespace sorbet::realmain::lsp::watchman {

// Watchman only accepts a `subscribe` for a root that it already watches, so Sorbet sends this command first. Watchman
// answers with the root it watches for the project that contains `workspace` (`watch`), and with the path of
// `workspace` inside that root (`relative_path`), if they differ.
std::string buildWatchProjectCommand(std::string_view workspace);

// `relativeRoot` is the `relative_path` from `watch-project`. When it is not empty, Watchman reports the names of
// changed files relative to it instead of relative to `root`.
std::string buildSubscribeCommand(std::string_view root, std::string_view relativeRoot,
                                  std::string_view subscriptionName, const std::vector<std::string> &extensions,
                                  std::string_view watchmanNamespace, const std::vector<std::string> &deferStates);

} // namespace sorbet::realmain::lsp::watchman

#endif // RUBY_TYPER_LSP_WATCHMAN_WATCHMANSUBSCRIPTION_H
