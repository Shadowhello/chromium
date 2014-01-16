// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "google_apis/gcm/engine/gcm_store_impl.h"

#include <string>
#include <vector>

#include "base/bind.h"
#include "base/files/file_path.h"
#include "base/files/scoped_temp_dir.h"
#include "base/memory/scoped_ptr.h"
#include "base/message_loop/message_loop.h"
#include "base/run_loop.h"
#include "base/strings/string_number_conversions.h"
#include "components/webdata/encryptor/encryptor.h"
#include "google_apis/gcm/base/mcs_message.h"
#include "google_apis/gcm/base/mcs_util.h"
#include "google_apis/gcm/protocol/mcs.pb.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace gcm {

namespace {

// Number of persistent ids to use in tests.
const int kNumPersistentIds = 10;

// Number of per-app messages in tests.
const int kNumMessagesPerApp = 20;

// App name for testing.
const char kAppName[] = "my_app";

// Category name for testing.
const char kCategoryName[] = "my_category";

const uint64 kDeviceId = 22;
const uint64 kDeviceToken = 55;

class GCMStoreImplTest : public testing::Test {
 public:
  GCMStoreImplTest();
  virtual ~GCMStoreImplTest();

  scoped_ptr<GCMStore> BuildGCMStore();

  std::string GetNextPersistentId();

  void PumpLoop();

  void LoadCallback(GCMStore::LoadResult* result_dst,
                    const GCMStore::LoadResult& result);
  void UpdateCallback(bool success);

 protected:
  base::MessageLoop message_loop_;
  base::ScopedTempDir temp_directory_;
  bool expected_success_;
  scoped_ptr<base::RunLoop> run_loop_;
};

GCMStoreImplTest::GCMStoreImplTest()
    : expected_success_(true) {
  EXPECT_TRUE(temp_directory_.CreateUniqueTempDir());
  run_loop_.reset(new base::RunLoop());

// On OSX, prevent the Keychain permissions popup during unit tests.
#if defined(OS_MACOSX)
  Encryptor::UseMockKeychain(true);
#endif
}

GCMStoreImplTest::~GCMStoreImplTest() {}

scoped_ptr<GCMStore> GCMStoreImplTest::BuildGCMStore() {
  return scoped_ptr<GCMStore>(new GCMStoreImpl(
      temp_directory_.path(), message_loop_.message_loop_proxy()));
}

std::string GCMStoreImplTest::GetNextPersistentId() {
  return base::Uint64ToString(base::Time::Now().ToInternalValue());
}

void GCMStoreImplTest::PumpLoop() { message_loop_.RunUntilIdle(); }

void GCMStoreImplTest::LoadCallback(GCMStore::LoadResult* result_dst,
                                    const GCMStore::LoadResult& result) {
  ASSERT_TRUE(result.success);
  *result_dst = result;
  run_loop_->Quit();
  run_loop_.reset(new base::RunLoop());
}

void GCMStoreImplTest::UpdateCallback(bool success) {
  ASSERT_EQ(expected_success_, success);
}

// Verify creating a new database and loading it.
TEST_F(GCMStoreImplTest, LoadNew) {
  scoped_ptr<GCMStore> gcm_store(BuildGCMStore());
  GCMStore::LoadResult load_result;
  gcm_store->Load(base::Bind(
      &GCMStoreImplTest::LoadCallback, base::Unretained(this), &load_result));
  PumpLoop();

  EXPECT_EQ(0U, load_result.device_android_id);
  EXPECT_EQ(0U, load_result.device_security_token);
  EXPECT_TRUE(load_result.incoming_messages.empty());
  EXPECT_TRUE(load_result.outgoing_messages.empty());
  EXPECT_EQ(1LL, load_result.serial_number_mappings.next_serial_number);
  EXPECT_TRUE(load_result.serial_number_mappings.user_serial_numbers.empty());
}

TEST_F(GCMStoreImplTest, DeviceCredentials) {
  scoped_ptr<GCMStore> gcm_store(BuildGCMStore());
  GCMStore::LoadResult load_result;
  gcm_store->Load(base::Bind(
      &GCMStoreImplTest::LoadCallback, base::Unretained(this), &load_result));
  PumpLoop();

  gcm_store->SetDeviceCredentials(
      kDeviceId,
      kDeviceToken,
      base::Bind(&GCMStoreImplTest::UpdateCallback, base::Unretained(this)));
  PumpLoop();

  gcm_store = BuildGCMStore().Pass();
  gcm_store->Load(base::Bind(
      &GCMStoreImplTest::LoadCallback, base::Unretained(this), &load_result));
  PumpLoop();

  ASSERT_EQ(kDeviceId, load_result.device_android_id);
  ASSERT_EQ(kDeviceToken, load_result.device_security_token);
}

// Verify saving some incoming messages, reopening the directory, and then
// removing those incoming messages.
TEST_F(GCMStoreImplTest, IncomingMessages) {
  scoped_ptr<GCMStore> gcm_store(BuildGCMStore());
  GCMStore::LoadResult load_result;
  gcm_store->Load(base::Bind(
      &GCMStoreImplTest::LoadCallback, base::Unretained(this), &load_result));
  PumpLoop();

  std::vector<std::string> persistent_ids;
  for (int i = 0; i < kNumPersistentIds; ++i) {
    persistent_ids.push_back(GetNextPersistentId());
    gcm_store->AddIncomingMessage(
        persistent_ids.back(),
        base::Bind(&GCMStoreImplTest::UpdateCallback, base::Unretained(this)));
    PumpLoop();
  }

  gcm_store = BuildGCMStore().Pass();
  gcm_store->Load(base::Bind(
      &GCMStoreImplTest::LoadCallback, base::Unretained(this), &load_result));
  PumpLoop();

  ASSERT_EQ(persistent_ids, load_result.incoming_messages);
  ASSERT_TRUE(load_result.outgoing_messages.empty());

  gcm_store->RemoveIncomingMessages(
      persistent_ids,
      base::Bind(&GCMStoreImplTest::UpdateCallback, base::Unretained(this)));
  PumpLoop();

  gcm_store = BuildGCMStore().Pass();
  load_result.incoming_messages.clear();
  gcm_store->Load(base::Bind(
      &GCMStoreImplTest::LoadCallback, base::Unretained(this), &load_result));
  PumpLoop();

  ASSERT_TRUE(load_result.incoming_messages.empty());
  ASSERT_TRUE(load_result.outgoing_messages.empty());
}

// Verify saving some outgoing messages, reopening the directory, and then
// removing those outgoing messages.
TEST_F(GCMStoreImplTest, OutgoingMessages) {
  scoped_ptr<GCMStore> gcm_store(BuildGCMStore());
  GCMStore::LoadResult load_result;
  gcm_store->Load(base::Bind(
      &GCMStoreImplTest::LoadCallback, base::Unretained(this), &load_result));
  PumpLoop();

  std::vector<std::string> persistent_ids;
  const int kNumPersistentIds = 10;
  for (int i = 0; i < kNumPersistentIds; ++i) {
    persistent_ids.push_back(GetNextPersistentId());
    mcs_proto::DataMessageStanza message;
    message.set_from(kAppName + persistent_ids.back());
    message.set_category(kCategoryName + persistent_ids.back());
    gcm_store->AddOutgoingMessage(
        persistent_ids.back(),
        MCSMessage(message),
        base::Bind(&GCMStoreImplTest::UpdateCallback, base::Unretained(this)));
    PumpLoop();
  }

  gcm_store = BuildGCMStore().Pass();
  gcm_store->Load(base::Bind(
      &GCMStoreImplTest::LoadCallback, base::Unretained(this), &load_result));
  PumpLoop();

  ASSERT_TRUE(load_result.incoming_messages.empty());
  ASSERT_EQ(load_result.outgoing_messages.size(), persistent_ids.size());
  for (int i = 0; i < kNumPersistentIds; ++i) {
    std::string id = persistent_ids[i];
    ASSERT_TRUE(load_result.outgoing_messages[id]);
    const mcs_proto::DataMessageStanza* message =
        reinterpret_cast<mcs_proto::DataMessageStanza*>(
            load_result.outgoing_messages[id]);
    ASSERT_EQ(message->from(), kAppName + id);
    ASSERT_EQ(message->category(), kCategoryName + id);
  }

  gcm_store->RemoveOutgoingMessages(
      persistent_ids,
      base::Bind(&GCMStoreImplTest::UpdateCallback, base::Unretained(this)));
  PumpLoop();

  gcm_store = BuildGCMStore().Pass();
  load_result.outgoing_messages.clear();
  gcm_store->Load(base::Bind(
      &GCMStoreImplTest::LoadCallback, base::Unretained(this), &load_result));
  PumpLoop();

  ASSERT_TRUE(load_result.incoming_messages.empty());
  ASSERT_TRUE(load_result.outgoing_messages.empty());
}

// Verify incoming and outgoing messages don't conflict.
TEST_F(GCMStoreImplTest, IncomingAndOutgoingMessages) {
  scoped_ptr<GCMStore> gcm_store(BuildGCMStore());
  GCMStore::LoadResult load_result;
  gcm_store->Load(base::Bind(
      &GCMStoreImplTest::LoadCallback, base::Unretained(this), &load_result));
  PumpLoop();

  std::vector<std::string> persistent_ids;
  const int kNumPersistentIds = 10;
  for (int i = 0; i < kNumPersistentIds; ++i) {
    persistent_ids.push_back(GetNextPersistentId());
    gcm_store->AddIncomingMessage(
        persistent_ids.back(),
        base::Bind(&GCMStoreImplTest::UpdateCallback, base::Unretained(this)));
    PumpLoop();

    mcs_proto::DataMessageStanza message;
    message.set_from(kAppName + persistent_ids.back());
    message.set_category(kCategoryName + persistent_ids.back());
    gcm_store->AddOutgoingMessage(
        persistent_ids.back(),
        MCSMessage(message),
        base::Bind(&GCMStoreImplTest::UpdateCallback, base::Unretained(this)));
    PumpLoop();
  }

  gcm_store = BuildGCMStore().Pass();
  gcm_store->Load(base::Bind(
      &GCMStoreImplTest::LoadCallback, base::Unretained(this), &load_result));
  PumpLoop();

  ASSERT_EQ(persistent_ids, load_result.incoming_messages);
  ASSERT_EQ(load_result.outgoing_messages.size(), persistent_ids.size());
  for (int i = 0; i < kNumPersistentIds; ++i) {
    std::string id = persistent_ids[i];
    ASSERT_TRUE(load_result.outgoing_messages[id]);
    const mcs_proto::DataMessageStanza* message =
        reinterpret_cast<mcs_proto::DataMessageStanza*>(
            load_result.outgoing_messages[id]);
    ASSERT_EQ(message->from(), kAppName + id);
    ASSERT_EQ(message->category(), kCategoryName + id);
  }

  gcm_store->RemoveIncomingMessages(
      persistent_ids,
      base::Bind(&GCMStoreImplTest::UpdateCallback, base::Unretained(this)));
  PumpLoop();
  gcm_store->RemoveOutgoingMessages(
      persistent_ids,
      base::Bind(&GCMStoreImplTest::UpdateCallback, base::Unretained(this)));
  PumpLoop();

  gcm_store = BuildGCMStore().Pass();
  load_result.incoming_messages.clear();
  load_result.outgoing_messages.clear();
  gcm_store->Load(base::Bind(
      &GCMStoreImplTest::LoadCallback, base::Unretained(this), &load_result));
  PumpLoop();

  ASSERT_TRUE(load_result.incoming_messages.empty());
  ASSERT_TRUE(load_result.outgoing_messages.empty());
}

// Verify that the next serial number of persisted properly.
TEST_F(GCMStoreImplTest, NextSerialNumber) {
  const int64 kNextSerialNumber = 77LL;
  scoped_ptr<GCMStore> gcm_store(BuildGCMStore());
  GCMStore::LoadResult load_result;
  gcm_store->Load(base::Bind(
      &GCMStoreImplTest::LoadCallback, base::Unretained(this), &load_result));
  PumpLoop();

  gcm_store->SetNextSerialNumber(
      kNextSerialNumber,
      base::Bind(&GCMStoreImplTest::UpdateCallback, base::Unretained(this)));
  PumpLoop();

  gcm_store = BuildGCMStore().Pass();
  gcm_store->Load(base::Bind(
      &GCMStoreImplTest::LoadCallback, base::Unretained(this), &load_result));
  PumpLoop();

  EXPECT_EQ(kNextSerialNumber,
            load_result.serial_number_mappings.next_serial_number);
}

// Verify that user serial number mappings are persisted properly.
TEST_F(GCMStoreImplTest, UserSerialNumberMappings) {
  scoped_ptr<GCMStore> gcm_store(BuildGCMStore());
  GCMStore::LoadResult load_result;
  gcm_store->Load(base::Bind(
      &GCMStoreImplTest::LoadCallback, base::Unretained(this), &load_result));
  PumpLoop();

  std::string username1 = "username1";
  int64 serial_number1 = 34LL;
  gcm_store->AddUserSerialNumber(
      username1,
      serial_number1,
      base::Bind(&GCMStoreImplTest::UpdateCallback, base::Unretained(this)));

  std::string username2 = "username2";
  int64 serial_number2 = 56LL;
  gcm_store->AddUserSerialNumber(
      username2,
      serial_number2,
      base::Bind(&GCMStoreImplTest::UpdateCallback, base::Unretained(this)));
  PumpLoop();

  gcm_store = BuildGCMStore().Pass();
  gcm_store->Load(base::Bind(
      &GCMStoreImplTest::LoadCallback, base::Unretained(this), &load_result));
  PumpLoop();

  ASSERT_EQ(2u, load_result.serial_number_mappings.user_serial_numbers.size());
  ASSERT_NE(
      load_result.serial_number_mappings.user_serial_numbers.end(),
      load_result.serial_number_mappings.user_serial_numbers.find(username1));
  EXPECT_EQ(serial_number1,
            load_result.serial_number_mappings.user_serial_numbers[username1]);
  ASSERT_NE(
      load_result.serial_number_mappings.user_serial_numbers.end(),
      load_result.serial_number_mappings.user_serial_numbers.find(username2));
  EXPECT_EQ(serial_number2,
            load_result.serial_number_mappings.user_serial_numbers[username2]);
}

// Test that per-app message limits are enforced, persisted across restarts,
// and updated as messages are removed.
TEST_F(GCMStoreImplTest, PerAppMessageLimits) {
  scoped_ptr<GCMStore> gcm_store(BuildGCMStore());
  GCMStore::LoadResult load_result;
  gcm_store->Load(base::Bind(&GCMStoreImplTest::LoadCallback,
                             base::Unretained(this),
                             &load_result));

  // Add the initial (below app limit) messages.
  for (int i = 0; i < kNumMessagesPerApp; ++i) {
    mcs_proto::DataMessageStanza message;
    message.set_from(kAppName);
    message.set_category(kCategoryName);
    EXPECT_TRUE(gcm_store->AddOutgoingMessage(
                    base::IntToString(i),
                    MCSMessage(message),
                    base::Bind(&GCMStoreImplTest::UpdateCallback,
                               base::Unretained(this))));
    PumpLoop();
  }

  // Attempting to add some more should fail.
  for (int i = 0; i < kNumMessagesPerApp; ++i) {
    mcs_proto::DataMessageStanza message;
    message.set_from(kAppName);
    message.set_category(kCategoryName);
    EXPECT_FALSE(gcm_store->AddOutgoingMessage(
                     base::IntToString(i + kNumMessagesPerApp),
                     MCSMessage(message),
                     base::Bind(&GCMStoreImplTest::UpdateCallback,
                                base::Unretained(this))));
    PumpLoop();
  }

  // Tear down and restore the database.
  gcm_store = BuildGCMStore().Pass();
  gcm_store->Load(base::Bind(&GCMStoreImplTest::LoadCallback,
                             base::Unretained(this),
                             &load_result));
  PumpLoop();

  // Adding more messages should still fail.
  for (int i = 0; i < kNumMessagesPerApp; ++i) {
    mcs_proto::DataMessageStanza message;
    message.set_from(kAppName);
    message.set_category(kCategoryName);
    EXPECT_FALSE(gcm_store->AddOutgoingMessage(
                     base::IntToString(i + kNumMessagesPerApp),
                     MCSMessage(message),
                     base::Bind(&GCMStoreImplTest::UpdateCallback,
                                base::Unretained(this))));
    PumpLoop();
  }

  // Remove the existing messages.
  for (int i = 0; i < kNumMessagesPerApp; ++i) {
    gcm_store->RemoveOutgoingMessage(
        base::IntToString(i),
        base::Bind(&GCMStoreImplTest::UpdateCallback,
                   base::Unretained(this)));
    PumpLoop();
  }

  // Successfully add new messages.
  for (int i = 0; i < kNumMessagesPerApp; ++i) {
    mcs_proto::DataMessageStanza message;
    message.set_from(kAppName);
    message.set_category(kCategoryName);
    EXPECT_TRUE(gcm_store->AddOutgoingMessage(
                    base::IntToString(i + kNumMessagesPerApp),
                    MCSMessage(message),
                    base::Bind(&GCMStoreImplTest::UpdateCallback,
                               base::Unretained(this))));
    PumpLoop();
  }
}

// When the database is destroyed, all database updates should fail. At the
// same time, they per-app message counts should not go up, as failures should
// result in decrementing the counts.
TEST_F(GCMStoreImplTest, AddMessageAfterDestroy) {
  scoped_ptr<GCMStore> gcm_store(BuildGCMStore());
  GCMStore::LoadResult load_result;
  gcm_store->Load(base::Bind(&GCMStoreImplTest::LoadCallback,
                             base::Unretained(this),
                             &load_result));
  PumpLoop();
  gcm_store->Destroy(base::Bind(&GCMStoreImplTest::UpdateCallback,
                               base::Unretained(this)));
  PumpLoop();

  expected_success_ = false;
  for (int i = 0; i < kNumMessagesPerApp * 2; ++i) {
    mcs_proto::DataMessageStanza message;
    message.set_from(kAppName);
    message.set_category(kCategoryName);
    // Because all adds are failing, none should hit the per-app message limits.
    EXPECT_TRUE(gcm_store->AddOutgoingMessage(
                    base::IntToString(i),
                    MCSMessage(message),
                    base::Bind(&GCMStoreImplTest::UpdateCallback,
                               base::Unretained(this))));
    PumpLoop();
  }
}

}  // namespace

}  // namespace gcm
