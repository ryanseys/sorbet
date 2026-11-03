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

// Declare it right after the `Popen` that it guards, so it only runs if construction succeeded. Runs on every exit
// path out of that scope — normal return, break, or exception — so the watchman child is always reaped before the
// thread returns.
struct ShutdownGuard {
    subprocess::Popen &p;
    ~ShutdownGuard() noexcept {
        shutdownWatchmanChild(p);
    }
};

void sendCommand(subprocess::Popen &p, spdlog::logger &logger, const string &command) {
    p.send(command.c_str(), command.size());
    logger.debug(command);
}

bool parseResponse(spdlog::logger &logger, const string &line, rapidjson::Document &d) {
    logger.debug(line);
    if (d.Parse(line.c_str(), line.size()).HasParseError() || !d.IsObject()) {
        logger.error("Error parsing Watchman response: `{}` is not a valid json object", line);
        return false;
    }
    return true;
}

} // namespace

optional<string> WatchmanProcess::readLine(int fd, string &buffer) {
    while (!isStopped()) {
        errno = 0;
        auto maybeLine = FileOps::readLineFromFd(fd, buffer);
        if (maybeLine.result == FileOps::ReadResult::Success) {
            return move(maybeLine.output);
        }
        if (maybeLine.result == FileOps::ReadResult::ErrorOrEof && errno != EINTR) {
            return nullopt;
        }
    }
    return nullopt;
}

void WatchmanProcess::watch(const string &root) {
    auto p = subprocess::Popen({watchmanPath.c_str(), "-j", "--no-pretty"}, subprocess::output{subprocess::PIPE},
                               subprocess::input{subprocess::PIPE});
    ShutdownGuard shutdownGuard{p};
    sendCommand(p, *logger, buildWatchCommand(root));
    // Without `-p`, the CLI sends one response and exits.
    p.close_input();

    string buffer;
    auto line = readLine(fileno(p.output()), buffer);
    if (!line.has_value()) {
        // Sorbet stopped, or the CLI sent no response, for example a --watchman-path wrapper that does not handle
        // `watch`. In the second case, subscribe anyway: a directory that Watchman already watches needs no watch.
        return;
    }

    rapidjson::MemoryPoolAllocator<> alloc;
    rapidjson::Document d(&alloc);
    if (parseResponse(*logger, *line, d) && d.HasMember("error") && d["error"].IsString()) {
        logger->error("Watchman could not watch `{}`: {}", root, d["error"].GetString());
    }
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

        watch(modifiedWorkspace);
        if (isStopped()) {
            return;
        }

        logger->debug("Starting monitoring path {} with watchman for files with extensions {}. Subscription id: {}",
                      modifiedWorkspace, fmt::join(extensions, ","), subscriptionName);

        auto p = subprocess::Popen({watchmanPath.c_str(), "-j", "-p", "--no-pretty"},
                                   subprocess::output{subprocess::PIPE}, subprocess::input{subprocess::PIPE});
        ShutdownGuard shutdownGuard{p};
        sendCommand(p, *logger,
                    buildSubscribeCommand(modifiedWorkspace, subscriptionName, extensions, watchmanNamespace,
                                          config->opts.watchmanPauseStateNames));

        auto fd = fileno(p.output());
        string buffer;
        while (auto maybeLine = readLine(fd, buffer)) {
            const string &line = *maybeLine;
            rapidjson::MemoryPoolAllocator<> alloc;
            rapidjson::Document d(&alloc);
            if (!parseResponse(*logger, line, d)) {
                continue;
            }

            if (d.HasMember("is_fresh_instance")) {
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
            } else if (!d.HasMember("subscribe")) {
                // Something we don't understand yet.
                logger->debug("Unknown Watchman response:\n{}", line);
            }
        }
        // The CLI closed its output. If Sorbet already stopped, this does nothing.
        exitWithCode(1, nullopt);
    } catch (exception e) {
        // Ignore exceptions thrown on forked process.
        if (getpid() == mainPid) {
            auto msg = fmt::format(
                "Error running Watchman (`{}`).\nWatchman is required for Sorbet to "
                "detect changes to files made outside of your code editor.\nDon't need Watchman? Run Sorbet "
                "with `--disable-watchman`.",
                watchmanPath);
            logger->error(msg);
            exitWithCode(1, msg);
        } else {
            // The forked process failed to start, likely because Watchman wasn't found. Use _exit: exit runs static
            // destructors and flushes stdio, which can wait forever for a lock that another thread held at the fork.
            _exit(1);
        }
    }

    ENFORCE(isStopped());
}

bool WatchmanProcess::isStopped() {
    absl::MutexLock lck(&mutex);
    return stopped;
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
