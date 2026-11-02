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

// Runs a WatchmanProcess against fake_watchman.sh, which (like Watchman) refuses to `subscribe` to
// a root that nothing has asked it to watch.
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
    unique_ptr<WatchmanProcess> process;

    FakeWatchmanTest() {
        const char *tmp = getenv("TEST_TMPDIR");
        string tmpl = fmt::format("{}/watchman-XXXXXX", tmp != nullptr ? tmp : "/tmp");
        REQUIRE(mkdtemp(tmpl.data()) != nullptr);
        // fake_watchman.sh reports physical paths, and /tmp is a symlink on macOS.
        dir = FileOps::realpath(tmpl).value();
        state = dir + "/state";
        FileOps::createDir(state);
        setenv("FAKE_WATCHMAN_STATE", state.c_str(), 1);
        initialized.Notify();
    }

    ~FakeWatchmanTest() {
        process = nullptr;
        error_code ec;
        filesystem::remove_all(dir, ec);
    }

    void start(const string &workspace) {
        auto logger = make_shared<spdlog::logger>("console", make_shared<spdlog::sinks::null_sink_mt>());
        opts.rawInputDirNames.emplace_back(workspace);
        auto config = make_shared<LSPConfiguration>(opts, output, logger, false);
        auto fakeWatchman = FileOps::realpath("main/lsp/test/fake_watchman.sh").value();
        process = make_unique<WatchmanProcess>(logger, fakeWatchman, workspace, vector<string>{"rb", "rbi"}, queue,
                                               mutex, initialized, config, "");
    }

    // Waits until Watchman reports changed files or the Watchman thread stops Sorbet, and returns
    // the names of the changed files.
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

TEST_CASE_FIXTURE(FakeWatchmanTest, "WatchmanProcess watches the workspace before it subscribes to changes") {
    auto workspace = dir + "/project";
    FileOps::createDir(workspace);
    FileOps::write(state + "/changes", "foo.rb\n");

    start(workspace);

    CHECK_EQ(waitForChangedFiles(), vector<string>{"foo.rb"});
    CHECK_FALSE(stopped());
    CHECK(errorsShownToUser().empty());
}

TEST_CASE_FIXTURE(FakeWatchmanTest, "WatchmanProcess reports file names relative to a workspace inside the project") {
    auto project = dir + "/project";
    auto workspace = project + "/app";
    FileOps::createDir(project);
    FileOps::createDir(workspace);
    // Watchman watches the whole project, not only the workspace that Sorbet asked for.
    FileOps::write(state + "/project_root", project);
    FileOps::write(state + "/changes", "app/foo.rb\nscripts/bar.rb\n");

    start(workspace);

    CHECK_EQ(waitForChangedFiles(), vector<string>{"foo.rb"});
    CHECK_FALSE(stopped());
}

TEST_CASE_FIXTURE(FakeWatchmanTest, "WatchmanProcess stops Sorbet with an error when Watchman cannot watch") {
    auto workspace = dir + "/project";
    FileOps::createDir(workspace);
    FileOps::write(state + "/refuse", "none of the files in root_files are present");

    start(workspace);

    CHECK(waitForChangedFiles().empty());
    REQUIRE(stopped());
    {
        absl::MutexLock lock(&mutex);
        CHECK_EQ(queue.errorCode, 1);
    }
    auto errors = errorsShownToUser();
    REQUIRE_EQ(errors.size(), 1);
    CHECK_NE(errors[0].find("none of the files in root_files are present"), string::npos);
    CHECK_NE(errors[0].find("--disable-watchman"), string::npos);
}

} // namespace sorbet::realmain::lsp::watchman::test
