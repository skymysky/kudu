// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include <gflags/gflags.h>
#include <gtest/gtest.h>
#include <memory>
#include <string>

#include "kudu/common/schema.h"
#include "kudu/common/wire_protocol-test-util.h"
#include "kudu/fs/fs_manager.h"
#include "kudu/gutil/gscoped_ptr.h"
#include "kudu/gutil/strings/substitute.h"
#include "kudu/integration-tests/mini_cluster.h"
#include "kudu/master/master-test-util.h"
#include "kudu/master/master.h"
#include "kudu/master/master.pb.h"
#include "kudu/master/mini_master.h"
#include "kudu/master/sys_catalog.h"
#include "kudu/master/ts_descriptor.h"
#include "kudu/security/test/test_certs.h"
#include "kudu/security/tls_context.h"
#include "kudu/security/token_verifier.h"
#include "kudu/tablet/tablet.h"
#include "kudu/tablet/tablet_replica.h"
#include "kudu/tserver/mini_tablet_server.h"
#include "kudu/tserver/tablet_server.h"
#include "kudu/util/curl_util.h"
#include "kudu/util/faststring.h"
#include "kudu/util/metrics.h"
#include "kudu/util/pb_util.h"
#include "kudu/util/stopwatch.h"
#include "kudu/util/test_util.h"
#include "kudu/util/version_info.h"

DECLARE_int32(heartbeat_interval_ms);
METRIC_DECLARE_counter(rows_inserted);
METRIC_DECLARE_counter(rows_updated);

namespace kudu {

using std::vector;
using std::shared_ptr;
using std::string;

using master::CatalogManager;
using master::CreateTableRequestPB;
using master::CreateTableResponsePB;
using master::GetTableLocationsResponsePB;
using master::GetTableSchemaRequestPB;
using master::GetTableSchemaResponsePB;
using master::IsCreateTableDoneRequestPB;
using master::IsCreateTableDoneResponsePB;
using master::MiniMaster;
using master::TSDescriptor;
using master::TabletLocationsPB;
using tserver::MiniTabletServer;

void CreateTableForTesting(MiniMaster* mini_master,
                           const string& table_name,
                           const Schema& schema,
                           string *tablet_id) {
  {
    CreateTableRequestPB req;
    CreateTableResponsePB resp;

    req.set_name(table_name);
    req.set_num_replicas(1);
    ASSERT_OK(SchemaToPB(schema, req.mutable_schema()));
    CatalogManager* catalog = mini_master->master()->catalog_manager();
    CatalogManager::ScopedLeaderSharedLock l(catalog);
    ASSERT_OK(l.first_failed_status());
    ASSERT_OK(catalog->CreateTable(&req, &resp, NULL));
  }

  int wait_time = 1000;
  bool is_table_created = false;
  for (int i = 0; i < 80; ++i) {
    IsCreateTableDoneRequestPB req;
    IsCreateTableDoneResponsePB resp;

    req.mutable_table()->set_table_name(table_name);
    CatalogManager* catalog = mini_master->master()->catalog_manager();
    {
      CatalogManager::ScopedLeaderSharedLock l(catalog);
      ASSERT_OK(l.first_failed_status());
      ASSERT_OK(catalog->IsCreateTableDone(&req, &resp));
    }
    if (resp.done()) {
      is_table_created = true;
      break;
    }

    VLOG(1) << "Waiting for table '" << table_name << "'to be created";

    SleepFor(MonoDelta::FromMicroseconds(wait_time));
    wait_time = std::min(wait_time * 5 / 4, 1000000);
  }
  ASSERT_TRUE(is_table_created);

  {
    GetTableSchemaRequestPB req;
    GetTableSchemaResponsePB resp;
    req.mutable_table()->set_table_name(table_name);
    CatalogManager* catalog = mini_master->master()->catalog_manager();
    CatalogManager::ScopedLeaderSharedLock l(catalog);
    ASSERT_OK(l.first_failed_status());
    ASSERT_OK(catalog->GetTableSchema(&req, &resp));
    ASSERT_TRUE(resp.create_table_done());
  }

  GetTableLocationsResponsePB resp;
  ASSERT_OK(WaitForRunningTabletCount(mini_master, table_name, 1, &resp));
  *tablet_id = resp.tablet_locations(0).tablet_id();
  LOG(INFO) << "Got tablet " << *tablet_id << " for table " << table_name;
}


// Tests for the Tablet Server registering with the Master,
// and the master maintaining the tablet descriptor.
class RegistrationTest : public KuduTest {
 public:
  RegistrationTest()
    : schema_({ ColumnSchema("c1", UINT32) }, 1) {
  }

  virtual void SetUp() OVERRIDE {
    // Make heartbeats faster to speed test runtime.
    FLAGS_heartbeat_interval_ms = 10;

    KuduTest::SetUp();

    cluster_.reset(new MiniCluster(env_, MiniClusterOptions()));
    ASSERT_OK(cluster_->Start());
  }

  virtual void TearDown() OVERRIDE {
    cluster_->Shutdown();
  }

  void CheckTabletServersPage(string* contents = nullptr) {
    EasyCurl c;
    faststring buf;
    string addr = cluster_->mini_master()->bound_http_addr().ToString();
    ASSERT_OK(c.FetchURL(strings::Substitute("http://$0/tablet-servers", addr),
                                &buf));
    string buf_str = buf.ToString();

    // Should include the TS UUID
    string expected_uuid =
      cluster_->mini_tablet_server(0)->server()->instance_pb().permanent_uuid();
    ASSERT_STR_CONTAINS(buf_str, expected_uuid);

    // Should check that the TS software version is included on the page.
    // tserver version should be the same as returned by GetShortVersionString()
    ASSERT_STR_CONTAINS(buf_str, VersionInfo::GetShortVersionString());
    if (contents != nullptr) {
      *contents = std::move(buf_str);
    }
  }

  Status WaitForReplicaCount(const string& tablet_id,
                             int expected_count,
                             TabletLocationsPB* locations) {
    while (true) {
      master::CatalogManager* catalog =
          cluster_->mini_master()->master()->catalog_manager();
      Status s;
      do {
        master::CatalogManager::ScopedLeaderSharedLock l(catalog);
        const Status& ls = l.first_failed_status();
        if (ls.IsServiceUnavailable()) {
          // ServiceUnavailable means catalog manager is not yet ready
          // to serve requests -- try again later.
          break;  // exiting out of the 'do {...} while (false)' scope
        }
        RETURN_NOT_OK(ls);
        s = catalog->GetTabletLocations(tablet_id, locations);
      } while (false);
      if (s.ok() && locations->replicas_size() == expected_count) {
        return Status::OK();
      }
      SleepFor(MonoDelta::FromMilliseconds(1));
    }
  }

 protected:
  gscoped_ptr<MiniCluster> cluster_;
  Schema schema_;
};

TEST_F(RegistrationTest, TestTSRegisters) {
  // Wait for the TS to register.
  vector<shared_ptr<TSDescriptor> > descs;
  ASSERT_OK(cluster_->WaitForTabletServerCount(
      1, MiniCluster::MatchMode::MATCH_TSERVERS, &descs));
  ASSERT_EQ(1, descs.size());

  // Verify that the registration is sane.
  ServerRegistrationPB reg;
  descs[0]->GetRegistration(&reg);
  {
    SCOPED_TRACE(SecureShortDebugString(reg));
    ASSERT_EQ(SecureShortDebugString(reg).find("0.0.0.0"), string::npos)
      << "Should not include wildcards in registration";
  }

  ASSERT_NO_FATAL_FAILURE(CheckTabletServersPage());

  // Restart the master, so it loses the descriptor, and ensure that the
  // hearbeater thread handles re-registering.
  cluster_->mini_master()->Shutdown();
  ASSERT_OK(cluster_->mini_master()->Restart());

  ASSERT_OK(cluster_->WaitForTabletServerCount(1));

  // TODO: when the instance ID / sequence number stuff is implemented,
  // restart the TS and ensure that it re-registers with the newer sequence
  // number.
}

TEST_F(RegistrationTest, TestMasterSoftwareVersion) {
  // Verify that the master's software version exists.
  ServerRegistrationPB reg;
  cluster_->mini_master()->master()->GetMasterRegistration(&reg);
  {
    SCOPED_TRACE(SecureShortDebugString(reg));
    ASSERT_TRUE(reg.has_software_version());
    ASSERT_STR_CONTAINS(reg.software_version(),
                        VersionInfo::GetShortVersionString());
  }
}

// Test starting multiple tablet servers and ensuring they both register with the master.
TEST_F(RegistrationTest, TestMultipleTS) {
  ASSERT_OK(cluster_->AddTabletServer());
  ASSERT_OK(cluster_->WaitForTabletServerCount(2));
}

// TODO: this doesn't belong under "RegistrationTest" - rename this file
// to something more appropriate - doesn't seem worth having separate
// whole test suites for registration, tablet reports, etc.
TEST_F(RegistrationTest, TestTabletReports) {
  string tablet_id_1;
  string tablet_id_2;

  MiniTabletServer* ts = cluster_->mini_tablet_server(0);
  string ts_root = cluster_->GetTabletServerFsRoot(0);

  auto GetCatalogMetric = [&](CounterPrototype& prototype) {
    auto metrics = cluster_->mini_master()->master()->catalog_manager()->sys_catalog()
        ->tablet_replica()->tablet()->GetMetricEntity();
    return prototype.Instantiate(metrics)->value();
  };
  int startup_rows_inserted = GetCatalogMetric(METRIC_rows_inserted);

  // Add a table, make sure it reports itself.
  CreateTableForTesting(cluster_->mini_master(), "fake-table", schema_, &tablet_id_1);

  TabletLocationsPB locs;
  ASSERT_OK(WaitForReplicaCount(tablet_id_1, 1, &locs));
  ASSERT_EQ(1, locs.replicas_size());
  LOG(INFO) << "Tablet successfully reported on " << locs.replicas(0).ts_info().permanent_uuid();

  // Check that we inserted the right number of rows for the new single-tablet table
  // (one for the table, one for the tablet).
  int post_create_rows_inserted = GetCatalogMetric(METRIC_rows_inserted);
  EXPECT_EQ(2, post_create_rows_inserted - startup_rows_inserted)
      << "Should have inserted one row each for the table and tablet";

  // Add another table, make sure it is reported via incremental.
  CreateTableForTesting(cluster_->mini_master(), "fake-table2", schema_, &tablet_id_2);
  ASSERT_OK(WaitForReplicaCount(tablet_id_2, 1, &locs));

  // Shut down the whole system, bring it back up, and make sure the tablets
  // are reported.
  ts->Shutdown();
  cluster_->mini_master()->Shutdown();
  ASSERT_OK(cluster_->mini_master()->Restart());
  ASSERT_OK(ts->Start());

  ASSERT_OK(WaitForReplicaCount(tablet_id_1, 1, &locs));
  ASSERT_OK(WaitForReplicaCount(tablet_id_2, 1, &locs));

  SleepFor(MonoDelta::FromSeconds(1));

  // After restart, check that the tablet reports produced the expected number of
  // writes to the catalog table:
  // - No inserts, because there are no new tablets.
  // - Two updates, since both replicas should have increased their term on restart.
  EXPECT_EQ(0, GetCatalogMetric(METRIC_rows_inserted));
  EXPECT_EQ(2, GetCatalogMetric(METRIC_rows_updated));

  // If we restart just the master, it should not write any data to the catalog, since the
  // tablets themselves are not changing term, etc.
  cluster_->mini_master()->Shutdown();
  ASSERT_OK(cluster_->mini_master()->Restart());
  // Sleep for a second to make sure the TS has plenty of time to re-heartbeat.
  SleepFor(MonoDelta::FromSeconds(1));
  EXPECT_EQ(0, GetCatalogMetric(METRIC_rows_inserted));
  EXPECT_EQ(0, GetCatalogMetric(METRIC_rows_updated));
}

// Check that after the tablet server registers, it gets a signed cert
// from the master.
TEST_F(RegistrationTest, TestTSGetsSignedX509Certificate) {
  MiniTabletServer* ts = cluster_->mini_tablet_server(0);
  ASSERT_EVENTUALLY([&](){
      ASSERT_TRUE(ts->server()->tls_context().has_signed_cert());
    });
}

// Check that after the tablet server registers, it gets the list of valid
// public token signing keys.
TEST_F(RegistrationTest, TestTSGetsTskList) {
  MiniTabletServer* ts = cluster_->mini_tablet_server(0);
  ASSERT_EVENTUALLY([&](){
      ASSERT_FALSE(ts->server()->token_verifier().ExportKeys().empty());
    });
}

// Test that, if the tserver has HTTPS enabled, the master links to it
// via https:// URLs and not http://.
TEST_F(RegistrationTest, TestExposeHttpsURLs) {
  MiniTabletServer* ts = cluster_->mini_tablet_server(0);
  string password;
  WebserverOptions* opts = &ts->options()->webserver_opts;
  ASSERT_OK(security::CreateTestSSLCerts(GetTestDataDirectory(),
                                         &opts->certificate_file,
                                         &opts->private_key_file,
                                         &password));
  opts->private_key_password_cmd = strings::Substitute("echo $0", password);
  ts->Shutdown();
  ASSERT_OK(ts->Start());

  // The URL displayed on the page uses a hostname. Rather than
  // dealing with figuring out what the hostname should be, just
  // use a more permissive regex which doesn't check the host.
  string expected_url_regex = strings::Substitute(
      "https://[a-zA-Z0-9.-]+:$0/", opts->port);

  // Need "eventually" here because the tserver may take a few seconds
  // to re-register while starting up.
  ASSERT_EVENTUALLY([&](){
      string contents;
      NO_FATALS(CheckTabletServersPage(&contents));
      ASSERT_STR_MATCHES(contents, expected_url_regex);
    });
}

} // namespace kudu
