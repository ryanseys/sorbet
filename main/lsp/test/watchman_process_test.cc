#include "doctest/doctest.h"
// has to go first as it violates our requirements
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
#include <cstdlib>
#include <filesystem>
#include <string>
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
    string watchmanPath = "main/lsp/test/fake_watchman.sh";
    unique_ptr<WatchmanProcess> process;

    FakeWatchmanTest() {
        const char *tmp = getenv("TEST_TMPDIR");
        dir = fmt::format("{}/watchman-XXXXXX", tmp != nullptr ? tmp : "/tmp");
        REQUIRE(mkdtemp(dir.data()) != nullptr);
        state = dir + "/state";
        FileOps::createDir(state);
        setenv("FAKE_WATCHMAN_STATE", state.c_str(), 1);
        initialized.Notify();
    }

    ~FakeWatchmanTest() {
        // Stop the Watchman thread and the fake before their files go away.
        process = nullptr;
        error_code ec;
        filesystem::remove_all(dir, ec);
    }

    void start(const string &workspace, const string &watchmanNamespace = "") {
        auto logger = make_shared<spdlog::logger>("console", make_shared<spdlog::sinks::null_sink_mt>());
        opts.rawInputDirNames.emplace_back(workspace);
        auto config = make_shared<LSPConfiguration>(opts, output, logger, false);
        process = make_unique<WatchmanProcess>(logger, watchmanPath, workspace, vector<string>{"rb", "rbi"}, queue,
                                               mutex, initialized, config, watchmanNamespace);
    }

    vector<string> waitForChangedFiles() {
        absl::MutexLock lock(&mutex);
        mutex.AwaitWithTimeout(
            absl::Condition(
                +[](MessageQueueState *queue) -> bool { return queue->terminate || !changedFiles(*queue).empty(); },
                &queue),
            absl::Seconds(10));
        return changedFiles(queue);
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

    void checkStoppedWithError(string_view problem) {
        {
            absl::MutexLock lock(&mutex);
            REQUIRE(mutex.AwaitWithTimeout(absl::Condition(&queue.terminate), absl::Seconds(10)));
        }
        auto errors = errorsShownToUser();
        REQUIRE_EQ(errors.size(), 1);
        CHECK_NE(errors[0].find(problem), string::npos);
        CHECK_NE(errors[0].find("--disable-watchman"), string::npos);
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

TEST_CASE_FIXTURE(FakeWatchmanTest, "WatchmanProcess stops Sorbet when Watchman cannot watch the input directory") {
    auto workspace = dir + "/project";
    FileOps::createDir(workspace);
    FileOps::write(state + "/refuse_watch", "None of the files listed in global config root_files are present and "
                                            "enforce_root_files is set to true.");

    start(workspace);

    checkStoppedWithError("None of the files listed in global config root_files are present");
}

TEST_CASE_FIXTURE(FakeWatchmanTest, "WatchmanProcess stops Sorbet when Watchman refuses the subscription") {
    auto workspace = dir + "/project";
    FileOps::createDir(workspace);
    FileOps::write(state + "/forget_watches", "");

    start(workspace);

    checkStoppedWithError("is not watched");
}

TEST_CASE_FIXTURE(FakeWatchmanTest, "WatchmanProcess stops Sorbet when Watchman cancels the subscription") {
    auto workspace = dir + "/project";
    FileOps::createDir(workspace);
    FileOps::write(state + "/cancel", "");

    start(workspace);

    checkStoppedWithError("Watchman canceled the subscription");
}

TEST_CASE_FIXTURE(FakeWatchmanTest, "WatchmanProcess stops Sorbet when it cannot run Watchman") {
    auto workspace = dir + "/project";
    FileOps::createDir(workspace);
    watchmanPath = dir + "/no-such-watchman";

    start(workspace);

    checkStoppedWithError("execve failed");
}

} // namespace sorbet::realmain::lsp::watchman::test
