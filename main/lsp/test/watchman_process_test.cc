#include "doctest/doctest.h"
// has to go first as it violates our requirements
#include "absl/strings/str_join.h"
#include "absl/synchronization/mutex.h"
#include "absl/synchronization/notification.h"
#include "common/FileOps.h"
#include "common/common.h"
#include "main/lsp/LSPConfiguration.h"
#include "main/lsp/LSPMessage.h"
#include "main/lsp/LSPOutput.h"
#include "main/lsp/MessageQueueState.h"
#include "main/lsp/json_types.h"
#include "main/lsp/watchman/WatchmanProcess.h"
#include "spdlog/sinks/null_sink.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace std;

namespace sorbet::realmain::lsp::watchman::test {
namespace {

// Runs a WatchmanProcess against fake_watchman.sh, which acts as the watchman CLI.
class FakeWatchmanTest {
public:
    string dir;
    string state;
    // LSPConfiguration keeps a reference to its options.
    options::Options opts;
    shared_ptr<LSPOutputToVector> output = make_shared<LSPOutputToVector>();
    absl::Mutex mutex;
    MessageQueueState queue;
    absl::Notification initialized;
    // If false, LSP does not initialize until the test notifies `initialized`.
    bool initializeOnStart = true;
    unique_ptr<WatchmanProcess> process;

    FakeWatchmanTest() {
        const char *tmp = getenv("TEST_TMPDIR");
        dir = fmt::format("{}/watchman-XXXXXX", tmp != nullptr ? tmp : "/tmp");
        REQUIRE(mkdtemp(dir.data()) != nullptr);
        state = dir + "/state";
        FileOps::createDir(state);
        setenv("FAKE_WATCHMAN_STATE", state.c_str(), 1);
    }

    ~FakeWatchmanTest() {
        // Stop the Watchman thread and the fake before their files go away.
        process = nullptr;
        error_code ec;
        filesystem::remove_all(dir, ec);
    }

    void start(const string &workspace, const string &watchmanNamespace = "") {
        if (initializeOnStart) {
            initialized.Notify();
        }
        auto logger = make_shared<spdlog::logger>("console", make_shared<spdlog::sinks::null_sink_mt>());
        opts.rawInputDirNames.emplace_back(workspace);
        auto config = make_shared<LSPConfiguration>(opts, output, logger, false);
        process = make_unique<WatchmanProcess>(logger, "main/lsp/test/fake_watchman.sh", workspace,
                                               vector<string>{"rb", "rbi"}, queue, mutex, initialized, config,
                                               watchmanNamespace);
    }

    vector<string> waitForChangedFiles(size_t count = 1) {
        absl::MutexLock lock(&mutex);
        auto ready = [this, count]() { return queue.terminate || changedFiles(queue).size() >= count; };
        mutex.AwaitWithTimeout(absl::Condition(&ready), absl::Seconds(10));
        return changedFiles(queue);
    }

    bool queueEmpty() {
        absl::MutexLock lock(&mutex);
        return queue.pendingRequests.empty();
    }

    static bool waitForFile(const string &path) {
        auto deadline = chrono::steady_clock::now() + chrono::seconds(10);
        while (!FileOps::exists(path) && chrono::steady_clock::now() < deadline) {
            this_thread::sleep_for(chrono::milliseconds(10));
        }
        return FileOps::exists(path);
    }

    bool stopped() {
        absl::MutexLock lock(&mutex);
        return queue.terminate;
    }

    // Empties `output`, so a second call returns nothing.
    vector<string> errorsShownToUser() {
        vector<string> messages;
        for (auto &msg : output->getOutput()) {
            if (msg->isNotification() && msg->method() == LSPMethod::WindowShowMessage) {
                auto &params = get<unique_ptr<ShowMessageParams>>(msg->asNotification().params);
                if (params->type == MessageType::Error) {
                    messages.emplace_back(params->message);
                }
            }
        }
        return messages;
    }

private:
    static vector<string> changedFiles(const MessageQueueState &queue) {
        vector<string> files;
        for (auto &msg : queue.pendingRequests) {
            if (msg->isNotification() && msg->method() == LSPMethod::SorbetWatchmanFileChange) {
                auto &response = get<unique_ptr<WatchmanQueryResponse>>(msg->asNotification().params);
                files.insert(files.end(), response->files.begin(), response->files.end());
            }
        }
        return files;
    }
};

} // namespace

TEST_CASE_FIXTURE(FakeWatchmanTest, "WatchmanProcess watches the input directory before it subscribes to changes") {
    auto workspace = dir + "/project";
    FileOps::createDir(workspace);
    FileOps::write(state + "/changes", "foo.rb\n");

    start(workspace);

    CHECK_EQ(waitForChangedFiles(), vector<string>{"foo.rb"});
    CHECK_FALSE(stopped());
    CHECK(errorsShownToUser().empty());
}

TEST_CASE_FIXTURE(FakeWatchmanTest, "WatchmanProcess watches the git root of a namespace") {
    auto repo = dir + "/repo";
    auto workspace = repo + "/app";
    FileOps::createDir(repo);
    FileOps::createDir(repo + "/.git");
    FileOps::createDir(workspace);
    FileOps::write(state + "/changes", "app/foo.rb\n");

    start(workspace, "app");

    // Sorbet removes the namespace from the names of the changed files.
    CHECK_EQ(waitForChangedFiles(), vector<string>{"foo.rb"});
    CHECK_FALSE(stopped());
}

TEST_CASE_FIXTURE(FakeWatchmanTest, "WatchmanProcess reads Watchman output while LSP initializes") {
    auto workspace = dir + "/project";
    FileOps::createDir(workspace);
    // The updates for these changes are more than a pipe holds.
    vector<string> changes;
    for (int i = 0; i < 1000; i++) {
        changes.emplace_back(fmt::format("file{}.rb", i));
    }
    FileOps::write(state + "/changes", absl::StrJoin(changes, "\n"));
    initializeOnStart = false;

    start(workspace);

    CHECK(waitForFile(state + "/sent"));
    // Sorbet holds the changes until LSP initializes.
    CHECK(queueEmpty());

    initialized.Notify();

    CHECK_EQ(waitForChangedFiles(changes.size()), changes);
    CHECK_FALSE(stopped());
}

} // namespace sorbet::realmain::lsp::watchman::test
