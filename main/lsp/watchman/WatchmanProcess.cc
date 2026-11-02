#include "WatchmanProcess.h"
#include "WatchmanShutdown.h"
#include "WatchmanSubscription.h"
#include "absl/strings/strip.h"
#include "common/FileOps.h"
#include "common/common.h"
#include "common/strings/formatting.h"
#include "main/lsp/LSPConfiguration.h"
#include "main/lsp/LSPMessage.h"
#include "main/lsp/LSPOutput.h"
#include "main/lsp/json_types.h"
#include "rapidjson/document.h"
#include "subprocess.hpp"

using namespace std;

namespace sorbet::realmain::lsp::watchman {

WatchmanProcess::WatchmanProcess(shared_ptr<spdlog::logger> logger, string_view watchmanPath, string_view workSpace,
                                 vector<string> extensions, MessageQueueState &messageQueue,
                                 absl::Mutex &messageQueueMutex, absl::Notification &initializedNotification,
                                 shared_ptr<const LSPConfiguration> config, string_view watchmanNamespace)
    : logger(std::move(logger)), watchmanPath(string(watchmanPath)), workSpace(string(workSpace)),
      extensions(std::move(extensions)), messageQueue(messageQueue), messageQueueMutex(messageQueueMutex),
      initializedNotification(initializedNotification), config(std::move(config)), watchmanNamespace(watchmanNamespace),
      thread(runInAThread("watchmanReader", std::bind(&WatchmanProcess::start, this))) {}

WatchmanProcess::~WatchmanProcess() {
    exitWithCode(0, "");
    // Destructor of Joinable ensures Watchman thread exits before this destructor finishes.
};

namespace {
template <typename F> void catchDeserializationError(spdlog::logger &logger, const string &line, F &&f) {
    try {
        f();
    } catch (sorbet::realmain::lsp::DeserializationError e) {
        // Gracefully handle deserialization errors, since they could be our fault.
        logger.error("Unable to deserialize Watchman request: {}\nOriginal request:\n{}", e.what(), line);
    }
}

// Declare one right after the `subprocess::Popen` it guards, so it only runs if construction
// succeeded. It runs on every exit path out of that scope — normal return, break, or exception — so
// the watchman child is always reaped before the thread returns.
struct ShutdownGuard {
    subprocess::Popen &p;
    ~ShutdownGuard() noexcept {
        shutdownWatchmanChild(p);
    }
};

// Sends one command to the Watchman CLI. The CLI reads a single JSON value from stdin; the newline
// marks where it ends for anything that reads the command line by line.
void sendCommand(subprocess::Popen &p, spdlog::logger &logger, string command) {
    logger.debug(command);
    command += '\n';
    p.send(command.c_str(), command.size());
}

} // namespace

optional<WatchmanProcess::WatchedRoot> WatchmanProcess::watchProject(const string &workspace) {
    auto p = subprocess::Popen({watchmanPath.c_str(), "-j", "--no-pretty"}, subprocess::output{subprocess::PIPE},
                               subprocess::input{subprocess::PIPE});
    ShutdownGuard shutdownGuard{p};

    sendCommand(p, *logger, buildWatchProjectCommand(workspace));
    // This CLI is not persistent: it answers the one command and exits.
    p.close_input();

    auto fd = fileno(p.output());
    string buffer;
    while (!isStopped()) {
        errno = 0;
        auto maybeLine = FileOps::readLineFromFd(fd, buffer);
        if (maybeLine.result == FileOps::ReadResult::Timeout) {
            // Watchman answers after its first crawl of the project, which can take a while.
            continue;
        }

        string line;
        if (maybeLine.result == FileOps::ReadResult::Success) {
            line = move(*maybeLine.output);
        } else if (errno == EINTR) {
            continue;
        } else if (!buffer.empty()) {
            // The CLI exited after an answer that did not end with a newline.
            line = move(buffer);
        } else {
            exitWithWatchmanError(
                fmt::format("Watchman (`{} -j --no-pretty`) exited without answering `watch-project`.", watchmanPath));
            return nullopt;
        }

        logger->debug(line);
        rapidjson::MemoryPoolAllocator<> alloc;
        rapidjson::Document d(&alloc);
        if (d.Parse(line.c_str(), line.size()).HasParseError() || !d.IsObject()) {
            exitWithWatchmanError(fmt::format("Watchman gave an unexpected answer to `watch-project`: `{}`", line));
            return nullopt;
        }
        if (d.HasMember("error") && d["error"].IsString()) {
            exitWithWatchmanError(fmt::format("Watchman could not watch `{}`: {}", workspace, d["error"].GetString()));
            return nullopt;
        }
        if (!d.HasMember("watch") || !d["watch"].IsString()) {
            exitWithWatchmanError(fmt::format("Watchman gave an unexpected answer to `watch-project`: `{}`", line));
            return nullopt;
        }

        WatchedRoot watchedRoot{d["watch"].GetString(), ""};
        if (d.HasMember("relative_path") && d["relative_path"].IsString()) {
            watchedRoot.relativePath = d["relative_path"].GetString();
        }
        return watchedRoot;
    }
    return nullopt;
}

void WatchmanProcess::start() {
    auto mainPid = getpid();
    try {
        string subscriptionName = fmt::format("ruby-typer-{}", getpid());

        string modifiedWorkspace = workSpace;
        if (!watchmanNamespace.empty()) {
            const optional<string> maybeResolved = FileOps::realpath(workSpace);
            if (!maybeResolved.has_value()) {
                logger->debug("Unable to resolve workspace path {} for namespacing", workSpace);
                watchmanNamespace.clear();
            } else {
                string_view root(*maybeResolved);
                logger->debug("realpath({}) = {}", workSpace, root);
                if (absl::ConsumeSuffix(&root, watchmanNamespace)) {
                    string gitDirectory(root);
                    gitDirectory += "/.git";
                    if (FileOps::dirExists(gitDirectory)) {
                        logger->debug("Using {} as watchman root", root);
                        modifiedWorkspace = root;
                    } else {
                        logger->debug(
                            "Parent directory {} of namespace {} is not a git repository, disabling namespacing", root,
                            watchmanNamespace);
                        watchmanNamespace.clear();
                    }
                } else {
                    logger->debug("Watched directory {} is not in namespace {}, disabling namespacing", root,
                                  watchmanNamespace);
                    watchmanNamespace.clear();
                }
            }
        }

        auto watchedRoot = watchProject(modifiedWorkspace);
        if (!watchedRoot.has_value()) {
            ENFORCE(isStopped());
            return;
        }

        logger->debug("Starting monitoring path {} with watchman for files with extensions {}. Subscription id: {}",
                      modifiedWorkspace, fmt::join(extensions, ","), subscriptionName);

        auto p = subprocess::Popen({watchmanPath.c_str(), "-j", "-p", "--no-pretty"},
                                   subprocess::output{subprocess::PIPE}, subprocess::input{subprocess::PIPE});
        ShutdownGuard shutdownGuard{p};

        sendCommand(p, *logger,
                    buildSubscribeCommand(watchedRoot->root, watchedRoot->relativePath, subscriptionName, extensions,
                                          watchmanNamespace, config->opts.watchmanPauseStateNames));

        auto file = p.output();
        auto fd = fileno(file);

        string buffer;

        while (!isStopped()) {
            errno = 0;
            auto maybeLine = FileOps::readLineFromFd(fd, buffer);
            if (maybeLine.result == FileOps::ReadResult::Timeout) {
                // Timeout occurred. See if we should abort before reading further.
                continue;
            }

            if (maybeLine.result == FileOps::ReadResult::ErrorOrEof) {
                if (errno == EINTR) {
                    continue;
                }

                // Exit loop; unable to read from Watchman process.
                exitWithCode(1, nullopt);
                break;
            }

            ENFORCE(maybeLine.result == FileOps::ReadResult::Success);

            const string &line = *maybeLine.output;
            // Line found!
            rapidjson::MemoryPoolAllocator<> alloc;
            rapidjson::Document d(&alloc);
            logger->debug(line);
            if (d.Parse(line.c_str(), line.size()).HasParseError()) {
                logger->error("Error parsing Watchman response: `{}` is not a valid json object", line);
            } else if (d.HasMember("is_fresh_instance")) {
                catchDeserializationError(*logger, line, [&d, this]() {
                    auto queryResponse = sorbet::realmain::lsp::WatchmanQueryResponse::fromJSONValue(d);
                    if (!watchmanNamespace.empty()) {
                        auto prefix(watchmanNamespace);
                        prefix += "/";

                        for (auto &file : queryResponse->files) {
                            string_view view(file);
                            if (!absl::ConsumePrefix(&view, prefix)) {
                                continue;
                            }
                            file = view;
                        }
                    }
                    processQueryResponse(move(queryResponse));
                });
            } else if (d.HasMember("state-enter")) {
                // These are messages from "state-enter" commands.  See
                // https://facebook.github.io/watchman/docs/cmd/state-enter.html
                // for more information.
                catchDeserializationError(*logger, line, [&d, this]() {
                    auto stateEnter = sorbet::realmain::lsp::WatchmanStateEnter::fromJSONValue(d);
                    processStateEnter(move(stateEnter));
                });
            } else if (d.HasMember("state-leave")) {
                // These are messages from "state-leave" commands.  See
                // https://facebook.github.io/watchman/docs/cmd/state-leave.html
                // for more information.
                catchDeserializationError(*logger, line, [&d, this]() {
                    auto stateLeave = sorbet::realmain::lsp::WatchmanStateLeave::fromJSONValue(d);
                    processStateLeave(move(stateLeave));
                });
            } else if (d.HasMember("error") && d["error"].IsString()) {
                exitWithWatchmanError(fmt::format("Watchman could not subscribe to changes in `{}`: {}",
                                                  watchedRoot->root, d["error"].GetString()));
                break;
            } else if (!d.HasMember("subscribe")) {
                // Something we don't understand yet.
                logger->debug("Unknown Watchman response:\n{}", line);
            }
        }
    } catch (exception e) {
        // Ignore exceptions thrown on forked process.
        if (getpid() == mainPid) {
            exitWithWatchmanError(fmt::format("Error running Watchman (with `{} -j`).", watchmanPath));
        } else {
            // The forked process failed to start, likely because Watchman wasn't found. Exit the process.
            exit(1);
        }
    }

    ENFORCE(isStopped());
}

bool WatchmanProcess::isStopped() {
    absl::MutexLock lck(&mutex);
    return stopped;
}

void WatchmanProcess::exitWithWatchmanError(string_view problem) {
    auto msg = fmt::format("{}\nWatchman is required for Sorbet to detect changes to files made outside of your code "
                           "editor.\nDon't need Watchman? Run Sorbet with `--disable-watchman`.",
                           problem);
    logger->error(msg);
    exitWithCode(1, msg);
}

void WatchmanProcess::exitWithCode(int code, const optional<string> &msg) {
    absl::MutexLock lck(&mutex);
    if (!stopped) {
        stopped = true;
        processExit(code, msg);
    }
}

void WatchmanProcess::enqueueNotification(unique_ptr<NotificationMessage> notification) {
    auto msg = make_unique<LSPMessage>(move(notification));
    // Don't start enqueueing requests until LSP is initialized.
    initializedNotification.WaitForNotification();
    {
        absl::MutexLock lck(&messageQueueMutex);
        msg->tagNewRequest(*logger);
        messageQueue.counters = mergeCounters(move(messageQueue.counters));
        messageQueue.pendingRequests.push_back(move(msg));
    }
}

void WatchmanProcess::processQueryResponse(unique_ptr<WatchmanQueryResponse> response) {
    auto notifMsg = make_unique<NotificationMessage>("2.0", LSPMethod::SorbetWatchmanFileChange, move(response));
    enqueueNotification(move(notifMsg));
}

void WatchmanProcess::processStateEnter(unique_ptr<sorbet::realmain::lsp::WatchmanStateEnter> stateEnter) {
    auto notification = make_unique<NotificationMessage>("2.0", LSPMethod::SorbetWatchmanStateEnter, move(stateEnter));
    enqueueNotification(move(notification));
}

void WatchmanProcess::processStateLeave(unique_ptr<sorbet::realmain::lsp::WatchmanStateLeave> stateLeave) {
    auto notification = make_unique<NotificationMessage>("2.0", LSPMethod::SorbetWatchmanStateLeave, move(stateLeave));
    enqueueNotification(move(notification));
}

void WatchmanProcess::processExit(int watchmanExitCode, const optional<string> &msg) {
    {
        absl::MutexLock lck(&messageQueueMutex);
        if (!messageQueue.terminate) {
            messageQueue.terminate = true;
            messageQueue.errorCode = watchmanExitCode;
            if (watchmanExitCode != 0 && msg.has_value()) {
                auto params = make_unique<ShowMessageParams>(MessageType::Error, msg.value());
                config->output->write(make_unique<LSPMessage>(
                    make_unique<NotificationMessage>("2.0", LSPMethod::WindowShowMessage, move(params))));
            }
        }
        logger->debug("Watchman terminating");
    }
}

} // namespace sorbet::realmain::lsp::watchman
