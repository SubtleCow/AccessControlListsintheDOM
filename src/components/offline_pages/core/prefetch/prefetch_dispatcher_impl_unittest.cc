// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/offline_pages/core/prefetch/prefetch_dispatcher_impl.h"

#include "base/logging.h"
#include "base/memory/ptr_util.h"
#include "base/test/scoped_feature_list.h"
#include "components/offline_pages/core/client_namespace_constants.h"
#include "components/offline_pages/core/offline_event_logger.h"
#include "components/offline_pages/core/offline_page_feature.h"
#include "components/offline_pages/core/prefetch/generate_page_bundle_request.h"
#include "components/offline_pages/core/prefetch/get_operation_request.h"
#include "components/offline_pages/core/prefetch/mock_thumbnail_fetcher.h"
#include "components/offline_pages/core/prefetch/prefetch_background_task.h"
#include "components/offline_pages/core/prefetch/prefetch_configuration.h"
#include "components/offline_pages/core/prefetch/prefetch_dispatcher_impl.h"
#include "components/offline_pages/core/prefetch/prefetch_network_request_factory.h"
#include "components/offline_pages/core/prefetch/prefetch_request_test_base.h"
#include "components/offline_pages/core/prefetch/prefetch_service.h"
#include "components/offline_pages/core/prefetch/prefetch_service_test_taco.h"
#include "components/offline_pages/core/prefetch/store/prefetch_store.h"
#include "components/offline_pages/core/prefetch/store/prefetch_store_test_util.h"
#include "components/offline_pages/core/prefetch/suggested_articles_observer.h"
#include "components/offline_pages/core/prefetch/test_prefetch_network_request_factory.h"
#include "components/offline_pages/core/stub_offline_page_model.h"
#include "components/version_info/channel.h"
#include "net/http/http_status_code.h"
#include "net/url_request/url_request_test_util.h"
#include "testing/gmock/include/gmock/gmock-matchers.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gmock_mutant.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

using testing::Contains;
using ::testing::InSequence;

namespace offline_pages {

namespace {
using testing::_;

const char kTestID[] = "id";
const GURL kTestURL("https://www.chromium.org");
const GURL kTestURL2("https://www.chromium.org/2");
const int64_t kTestOfflineID = 1111;
const char kClientID[] = "client-id-1";

class MockOfflinePageModel : public StubOfflinePageModel {
 public:
  MockOfflinePageModel() = default;
  ~MockOfflinePageModel() override = default;
  MOCK_METHOD1(StoreThumbnail, void(const OfflinePageThumbnail& thumb));
  MOCK_METHOD2(HasThumbnailForOfflineId_,
               void(int64_t offline_id,
                    base::OnceCallback<void(bool)>* callback));
  void HasThumbnailForOfflineId(
      int64_t offline_id,
      base::OnceCallback<void(bool)> callback) override {
    HasThumbnailForOfflineId_(offline_id, &callback);
  }
};

class TestPrefetchBackgroundTask : public PrefetchBackgroundTask {
 public:
  TestPrefetchBackgroundTask(
      PrefetchService* service,
      base::RepeatingCallback<void(PrefetchBackgroundTaskRescheduleType)>
          callback)
      : PrefetchBackgroundTask(service), callback_(std::move(callback)) {}
  ~TestPrefetchBackgroundTask() override = default;

  void SetReschedule(PrefetchBackgroundTaskRescheduleType type) override {
    PrefetchBackgroundTask::SetReschedule(type);
    if (!callback_.is_null())
      callback_.Run(reschedule_type());
  }

 private:
  base::RepeatingCallback<void(PrefetchBackgroundTaskRescheduleType)> callback_;
};

class TestPrefetchConfiguration : public PrefetchConfiguration {
 public:
  bool IsPrefetchingEnabledBySettings() override { return enabled_; };

  void set_enabled(bool enabled) { enabled_ = enabled; }

 private:
  bool enabled_ = true;
};

}  // namespace

class PrefetchDispatcherTest : public PrefetchRequestTestBase {
 public:
  PrefetchDispatcherTest();

  // Test implementation.
  void SetUp() override;
  void TearDown() override;

  void BeginBackgroundTask();

  PrefetchBackgroundTask* GetBackgroundTask() {
    return dispatcher_->background_task_.get();
  }

  TaskQueue& GetTaskQueueFrom(PrefetchDispatcherImpl* prefetch_dispatcher) {
    return prefetch_dispatcher->task_queue_;
  }

  void SetReschedule(PrefetchBackgroundTaskRescheduleType type) {
    reschedule_called_ = true;
    reschedule_type_ = type;
  }

  void DisablePrefetchingInSettings() {
    static_cast<TestPrefetchConfiguration*>(
        taco_->prefetch_service()->GetPrefetchConfiguration())
        ->set_enabled(false);
  }

  bool dispatcher_suspended() const { return dispatcher_->suspended_; }
  TaskQueue* dispatcher_task_queue() { return &dispatcher_->task_queue_; }
  PrefetchDispatcher* prefetch_dispatcher() { return dispatcher_; }
  TestPrefetchNetworkRequestFactory* network_request_factory() {
    return network_request_factory_;
  }

  bool reschedule_called() const { return reschedule_called_; }
  PrefetchBackgroundTaskRescheduleType reschedule_type_result() const {
    return reschedule_type_;
  }

  void ExpectFetchThumbnail(const std::string& thumbnail_data,
                            const char* client_id = kClientID) {
    EXPECT_CALL(*thumbnail_fetcher_,
                FetchSuggestionImageData_(
                    ClientId(kSuggestedArticlesNamespace, client_id), _))
        .WillOnce(
            testing::Invoke(testing::CallbackToFunctor(base::BindRepeating(
                [](const std::string& thumbnail_data,
                   scoped_refptr<base::TestMockTimeTaskRunner> task_runner,
                   const ClientId& id,
                   ThumbnailFetcher::ImageDataFetchedCallback* callback) {
                  task_runner->PostTask(
                      FROM_HERE,
                      base::BindOnce(std::move(*callback), thumbnail_data));
                },
                thumbnail_data, task_runner()))));
  }

  void ExpectHasThumbnailForOfflineId(int64_t offline_id, bool to_return) {
    EXPECT_CALL(*offline_model_, HasThumbnailForOfflineId_(offline_id, _))
        .WillOnce(
            testing::Invoke(testing::CallbackToFunctor(base::BindRepeating(
                [](bool to_return,
                   scoped_refptr<base::TestMockTimeTaskRunner> task_runner,
                   int64_t offline_id_,
                   base::OnceCallback<void(bool)>* callback) {
                  task_runner->PostTask(
                      FROM_HERE,
                      base::BindOnce(std::move(*callback), to_return));
                },
                to_return, task_runner()))));
  }

 protected:
  // Owned by |taco_|.
  MockOfflinePageModel* offline_model_;

  std::vector<PrefetchURL> test_urls_;

  // Owned by |taco_|.
  MockThumbnailFetcher* thumbnail_fetcher_;

 private:
  std::unique_ptr<PrefetchServiceTestTaco> taco_;

  base::test::ScopedFeatureList feature_list_;

  // Owned by |taco_|.
  PrefetchDispatcherImpl* dispatcher_;
  // Owned by |taco_|.
  TestPrefetchNetworkRequestFactory* network_request_factory_;

  bool reschedule_called_ = false;
  PrefetchBackgroundTaskRescheduleType reschedule_type_ =
      PrefetchBackgroundTaskRescheduleType::NO_RESCHEDULE;
};

PrefetchDispatcherTest::PrefetchDispatcherTest() {
  feature_list_.InitAndEnableFeature(kPrefetchingOfflinePagesFeature);
}

void PrefetchDispatcherTest::SetUp() {
  dispatcher_ = new PrefetchDispatcherImpl();
  network_request_factory_ = new TestPrefetchNetworkRequestFactory();
  taco_.reset(new PrefetchServiceTestTaco);
  taco_->SetPrefetchDispatcher(base::WrapUnique(dispatcher_));
  taco_->SetPrefetchNetworkRequestFactory(
      base::WrapUnique(network_request_factory_));
  taco_->SetPrefetchConfiguration(
      std::make_unique<TestPrefetchConfiguration>());
  auto thumbnail_fetcher = std::make_unique<MockThumbnailFetcher>();
  thumbnail_fetcher_ = thumbnail_fetcher.get();
  taco_->SetThumbnailFetcher(std::move(thumbnail_fetcher));
  auto model = std::make_unique<MockOfflinePageModel>();
  offline_model_ = model.get();
  taco_->SetOfflinePageModel(std::move(model));
  taco_->CreatePrefetchService();

  ASSERT_TRUE(test_urls_.empty());
  test_urls_.push_back({"1", GURL("http://testurl.com/foo"), base::string16()});
  test_urls_.push_back(
      {"2", GURL("https://testurl.com/bar"), base::string16()});
}

void PrefetchDispatcherTest::TearDown() {
  // Ensures that the task is stopped first.
  dispatcher_->StopBackgroundTask();
  RunUntilIdle();

  // Ensures that the store is properly disposed off.
  taco_.reset();
  RunUntilIdle();
}

void PrefetchDispatcherTest::BeginBackgroundTask() {
  dispatcher_->BeginBackgroundTask(std::make_unique<TestPrefetchBackgroundTask>(
      taco_->prefetch_service(),
      base::BindRepeating(&PrefetchDispatcherTest::SetReschedule,
                          base::Unretained(this))));
}

MATCHER(ValidThumbnail, "") {
  return arg.offline_id == kTestOfflineID && !arg.thumbnail.empty();
};

TEST_F(PrefetchDispatcherTest, DispatcherDoesNotCrash) {
  // TODO(https://crbug.com/735254): Ensure that Dispatcher unit test keep up
  // with the state of adding tasks, and that the end state is we have tests
  // that verify the proper tasks were added in the proper order at each wakeup
  // signal of the dispatcher.
  prefetch_dispatcher()->AddCandidatePrefetchURLs(kSuggestedArticlesNamespace,
                                                  test_urls_);
  prefetch_dispatcher()->RemoveAllUnprocessedPrefetchURLs(
      kSuggestedArticlesNamespace);
  prefetch_dispatcher()->RemovePrefetchURLsByClientId(
      {kSuggestedArticlesNamespace, "123"});
}

TEST_F(PrefetchDispatcherTest, AddCandidatePrefetchURLsTask) {
  prefetch_dispatcher()->AddCandidatePrefetchURLs(kSuggestedArticlesNamespace,
                                                  test_urls_);
  EXPECT_TRUE(dispatcher_task_queue()->HasPendingTasks());
  RunUntilIdle();
  EXPECT_FALSE(dispatcher_task_queue()->HasPendingTasks());
  EXPECT_FALSE(dispatcher_task_queue()->HasRunningTask());
}

TEST_F(PrefetchDispatcherTest, RemovePrefetchURLsByClientId) {
  prefetch_dispatcher()->AddCandidatePrefetchURLs(kSuggestedArticlesNamespace,
                                                  test_urls_);
  RunUntilIdle();
  prefetch_dispatcher()->RemovePrefetchURLsByClientId(
      ClientId(kSuggestedArticlesNamespace, test_urls_.front().id));
  EXPECT_TRUE(dispatcher_task_queue()->HasPendingTasks());
  RunUntilIdle();
  EXPECT_FALSE(dispatcher_task_queue()->HasPendingTasks());
  EXPECT_FALSE(dispatcher_task_queue()->HasRunningTask());
}

TEST_F(PrefetchDispatcherTest, DispatcherDoesNothingIfFeatureNotEnabled) {
  base::test::ScopedFeatureList disabled_feature_list;
  disabled_feature_list.InitAndDisableFeature(kPrefetchingOfflinePagesFeature);

  // Don't add a task for new prefetch URLs.
  prefetch_dispatcher()->AddCandidatePrefetchURLs(kSuggestedArticlesNamespace,
                                                  test_urls_);
  EXPECT_FALSE(dispatcher_task_queue()->HasRunningTask());

  // Do nothing with a new background task.
  BeginBackgroundTask();
  EXPECT_EQ(nullptr, GetBackgroundTask());

  // TODO(carlosk): add more checks here.
}

TEST_F(PrefetchDispatcherTest, DispatcherDoesNothingIfSettingsDoNotAllowIt) {
  DisablePrefetchingInSettings();

  // Don't add a task for new prefetch URLs.
  prefetch_dispatcher()->AddCandidatePrefetchURLs(kSuggestedArticlesNamespace,
                                                  test_urls_);
  EXPECT_FALSE(dispatcher_task_queue()->HasRunningTask());

  // Do nothing with a new background task.
  BeginBackgroundTask();
  EXPECT_EQ(nullptr, GetBackgroundTask());

  // TODO(carlosk): add more checks here.
}

TEST_F(PrefetchDispatcherTest, DispatcherReleasesBackgroundTask) {
  PrefetchURL prefetch_url(kTestID, kTestURL, base::string16());
  prefetch_dispatcher()->AddCandidatePrefetchURLs(
      kSuggestedArticlesNamespace, std::vector<PrefetchURL>(1, prefetch_url));
  RunUntilIdle();

  // We start the background task, causing reconcilers and action tasks to be
  // run. We should hold onto the background task until there is no more work to
  // do, after the network request ends.
  ASSERT_EQ(nullptr, GetBackgroundTask());
  BeginBackgroundTask();
  EXPECT_TRUE(dispatcher_task_queue()->HasRunningTask());
  RunUntilIdle();

  // Still holding onto the background task.
  EXPECT_NE(nullptr, GetBackgroundTask());
  EXPECT_FALSE(dispatcher_task_queue()->HasRunningTask());
  EXPECT_THAT(*network_request_factory()->GetAllUrlsRequested(),
              Contains(prefetch_url.url.spec()));

  // When the network request finishes, the dispatcher should still hold the
  // ScopedBackgroundTask because it needs to process the results of the
  // request.
  RespondWithHttpError(net::HTTP_INTERNAL_SERVER_ERROR);
  EXPECT_NE(nullptr, GetBackgroundTask());
  RunUntilIdle();

  // Because there is no work remaining, the background task should be released.
  EXPECT_EQ(nullptr, GetBackgroundTask());
}

TEST_F(PrefetchDispatcherTest, RetryWithBackoffAfterFailedNetworkRequest) {
  PrefetchURL prefetch_url(kTestID, kTestURL, base::string16());
  prefetch_dispatcher()->AddCandidatePrefetchURLs(
      kSuggestedArticlesNamespace, std::vector<PrefetchURL>(1, prefetch_url));
  RunUntilIdle();

  BeginBackgroundTask();
  RunUntilIdle();

  // Trigger another request to make sure we have more work to do.
  PrefetchURL prefetch_url2(kTestID, kTestURL2, base::string16());
  prefetch_dispatcher()->AddCandidatePrefetchURLs(
      kSuggestedArticlesNamespace, std::vector<PrefetchURL>(1, prefetch_url2));
  RunUntilIdle();

  // This should trigger retry with backoff.
  RespondWithHttpError(net::HTTP_INTERNAL_SERVER_ERROR);
  RunUntilIdle();

  EXPECT_TRUE(reschedule_called());
  EXPECT_EQ(PrefetchBackgroundTaskRescheduleType::RESCHEDULE_WITH_BACKOFF,
            reschedule_type_result());

  EXPECT_FALSE(dispatcher_suspended());

  // There are still outstanding requests.
  EXPECT_TRUE(network_request_factory()->HasOutstandingRequests());

  // Still holding onto the background task.
  EXPECT_NE(nullptr, GetBackgroundTask());
}

TEST_F(PrefetchDispatcherTest, RetryWithoutBackoffAfterFailedNetworkRequest) {
  PrefetchURL prefetch_url(kTestID, kTestURL, base::string16());
  prefetch_dispatcher()->AddCandidatePrefetchURLs(
      kSuggestedArticlesNamespace, std::vector<PrefetchURL>(1, prefetch_url));
  RunUntilIdle();

  BeginBackgroundTask();
  RunUntilIdle();

  // Trigger another request to make sure we have more work to do.
  PrefetchURL prefetch_url2(kTestID, kTestURL2, base::string16());
  prefetch_dispatcher()->AddCandidatePrefetchURLs(
      kSuggestedArticlesNamespace, std::vector<PrefetchURL>(1, prefetch_url2));

  // This should trigger retry without backoff.
  RespondWithNetError(net::ERR_CONNECTION_CLOSED);
  RunUntilIdle();

  EXPECT_TRUE(reschedule_called());
  EXPECT_EQ(PrefetchBackgroundTaskRescheduleType::RESCHEDULE_WITHOUT_BACKOFF,
            reschedule_type_result());

  EXPECT_FALSE(dispatcher_suspended());

  // There are still outstanding requests.
  EXPECT_TRUE(network_request_factory()->HasOutstandingRequests());

  // Still holding onto the background task.
  EXPECT_NE(nullptr, GetBackgroundTask());
}

TEST_F(PrefetchDispatcherTest, SuspendAfterFailedNetworkRequest) {
  PrefetchURL prefetch_url(kTestID, kTestURL, base::string16());
  prefetch_dispatcher()->AddCandidatePrefetchURLs(
      kSuggestedArticlesNamespace, std::vector<PrefetchURL>(1, prefetch_url));
  RunUntilIdle();

  BeginBackgroundTask();
  RunUntilIdle();

  // Trigger another request to make sure we have more work to do.
  PrefetchURL prefetch_url2(kTestID, kTestURL2, base::string16());
  prefetch_dispatcher()->AddCandidatePrefetchURLs(
      kSuggestedArticlesNamespace, std::vector<PrefetchURL>(1, prefetch_url2));

  EXPECT_FALSE(dispatcher_suspended());

  // This should trigger suspend.
  RespondWithNetError(net::ERR_BLOCKED_BY_ADMINISTRATOR);
  RunUntilIdle();

  EXPECT_TRUE(reschedule_called());
  EXPECT_EQ(PrefetchBackgroundTaskRescheduleType::SUSPEND,
            reschedule_type_result());

  // The dispatcher should be suspended.
  EXPECT_TRUE(dispatcher_suspended());

  // The 2nd request will not be created because the prefetch dispatcher
  // pipeline is in suspended state and will not queue any new tasks.
  EXPECT_FALSE(network_request_factory()->HasOutstandingRequests());

  // The background task should finally be released due to suspension.
  EXPECT_EQ(nullptr, GetBackgroundTask());
}

TEST_F(PrefetchDispatcherTest, SuspendRemovedAfterNewBackgroundTask) {
  PrefetchURL prefetch_url(kTestID, kTestURL, base::string16());
  prefetch_dispatcher()->AddCandidatePrefetchURLs(
      kSuggestedArticlesNamespace, std::vector<PrefetchURL>(1, prefetch_url));
  RunUntilIdle();

  BeginBackgroundTask();
  RunUntilIdle();

  // This should trigger suspend.
  RespondWithNetError(net::ERR_BLOCKED_BY_ADMINISTRATOR);
  RunUntilIdle();

  EXPECT_TRUE(reschedule_called());
  EXPECT_EQ(PrefetchBackgroundTaskRescheduleType::SUSPEND,
            reschedule_type_result());

  // The dispatcher should be suspended.
  EXPECT_TRUE(dispatcher_suspended());

  // No task is in the queue.
  EXPECT_FALSE(dispatcher_task_queue()->HasPendingTasks());

  // The background task should finally be released due to suspension.
  EXPECT_EQ(nullptr, GetBackgroundTask());

  // Trigger another request to make sure we have more work to do.
  PrefetchURL prefetch_url2(kTestID, kTestURL2, base::string16());
  prefetch_dispatcher()->AddCandidatePrefetchURLs(
      kSuggestedArticlesNamespace, std::vector<PrefetchURL>(1, prefetch_url2));

  BeginBackgroundTask();

  // The suspended state should be reset.
  EXPECT_FALSE(dispatcher_suspended());

  // Some reconcile and action tasks should be created.
  EXPECT_TRUE(dispatcher_task_queue()->HasPendingTasks());
}

TEST_F(PrefetchDispatcherTest, NoNetworkRequestsAfterNewURLs) {
  PrefetchURL prefetch_url(kTestID, kTestURL, base::string16());
  prefetch_dispatcher()->AddCandidatePrefetchURLs(
      kSuggestedArticlesNamespace, std::vector<PrefetchURL>(1, prefetch_url));
  RunUntilIdle();

  // We should not have started GPB
  EXPECT_EQ(nullptr, GetRunningFetcher());
}

TEST_F(PrefetchDispatcherTest, ThumbnailFetchFailure_ItemDownloaded) {
  ExpectFetchThumbnail("");
  ExpectHasThumbnailForOfflineId(kTestOfflineID, false);
  EXPECT_CALL(*offline_model_, StoreThumbnail(_)).Times(0);
  prefetch_dispatcher()->ItemDownloaded(
      kTestOfflineID, ClientId(kSuggestedArticlesNamespace, kClientID));
}

TEST_F(PrefetchDispatcherTest, ThumbnailFetchSuccess_ItemDownloaded) {
  std::string kThumbnailData = "abc";
  ExpectHasThumbnailForOfflineId(kTestOfflineID, false);
  EXPECT_CALL(*offline_model_, StoreThumbnail(ValidThumbnail()));
  ExpectFetchThumbnail(kThumbnailData);
  prefetch_dispatcher()->ItemDownloaded(
      kTestOfflineID, ClientId(kSuggestedArticlesNamespace, kClientID));
}

TEST_F(PrefetchDispatcherTest, ThumbnailAlreadyExists_ItemDownloaded) {
  ExpectHasThumbnailForOfflineId(kTestOfflineID, true);
  EXPECT_CALL(*thumbnail_fetcher_, FetchSuggestionImageData_(_, _)).Times(0);
  EXPECT_CALL(*offline_model_, StoreThumbnail(_)).Times(0);
  prefetch_dispatcher()->ItemDownloaded(
      kTestOfflineID, ClientId(kSuggestedArticlesNamespace, kClientID));
}

TEST_F(PrefetchDispatcherTest,
       ThumbnailVariousCases_GeneratePageBundleRequested) {
  // Covers all possible thumbnail cases with a single
  // GeneratePageBundleRequested call: fetch succeeds (#1), fetch fails (#2),
  // item already exists (#3).
  const int64_t kTestOfflineID1 = 100;
  const int64_t kTestOfflineID2 = 101;
  const int64_t kTestOfflineID3 = 102;
  const char kClientID1[] = "a";
  const char kClientID2[] = "b";
  const char kClientID3[] = "c";

  InSequence in_sequence;
  // Case #1.
  ExpectHasThumbnailForOfflineId(kTestOfflineID1, false);
  ExpectFetchThumbnail("abc", kClientID1);
  EXPECT_CALL(*offline_model_, StoreThumbnail(_)).Times(1);
  // Case #2.
  ExpectHasThumbnailForOfflineId(kTestOfflineID2, false);
  ExpectFetchThumbnail("", kClientID2);
  // Case #3.
  ExpectHasThumbnailForOfflineId(kTestOfflineID3, true);

  auto prefetch_item_ids = std::make_unique<PrefetchDispatcher::IdsVector>();
  prefetch_item_ids->emplace_back(
      kTestOfflineID1, ClientId(kSuggestedArticlesNamespace, kClientID1));
  prefetch_item_ids->emplace_back(
      kTestOfflineID2, ClientId(kSuggestedArticlesNamespace, kClientID2));
  prefetch_item_ids->emplace_back(
      kTestOfflineID3, ClientId(kSuggestedArticlesNamespace, kClientID3));
  prefetch_dispatcher()->GeneratePageBundleRequested(
      std::move(prefetch_item_ids));
}

}  // namespace offline_pages