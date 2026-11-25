#include "ghostfs/engine.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace {

using json = nlohmann::json;

TEST(EngineSmokeTest, PingReturnsEngineMetadata) {
  const std::string out = ghostfs::process_request_json(R"({"command":"ping"})");
  const json parsed = json::parse(out);
  EXPECT_TRUE(parsed.at("ok").get<bool>());
  EXPECT_EQ(parsed.at("command").get<std::string>(), "ping");
  EXPECT_EQ(parsed.at("data").at("engine").get<std::string>(), "ghostfs_engine");
  EXPECT_EQ(parsed.at("data").at("fs_format").get<std::string>(), "GFS1");
}

TEST(EngineSmokeTest, VersionReturnsVersionInfo) {
  const std::string out = ghostfs::process_request_json(R"({"command":"version"})");
  const json parsed = json::parse(out);
  EXPECT_TRUE(parsed.at("ok").get<bool>());
  EXPECT_EQ(parsed.at("data").at("version").get<std::string>(), "0.1.0");
}

TEST(EngineSmokeTest, UnknownCommandReturnsStructuredError) {
  const std::string out = ghostfs::process_request_json(R"({"command":"nonexistent"})");
  const json parsed = json::parse(out);
  EXPECT_FALSE(parsed.at("ok").get<bool>());
  EXPECT_EQ(parsed.at("error").at("code").get<std::string>(), "ERR_UNKNOWN_COMMAND");
}

TEST(EngineSmokeTest, InvalidJsonReturnsStructuredError) {
  const std::string out = ghostfs::process_request_json("{broken");
  const json parsed = json::parse(out);
  EXPECT_FALSE(parsed.at("ok").get<bool>());
  EXPECT_EQ(parsed.at("error").at("code").get<std::string>(), "ERR_INVALID_JSON");
}

TEST(PathGuardTest, RejectsTraversalOutsideWorkspace) {
  const std::filesystem::path root = std::filesystem::temp_directory_path() / "ghostfs_test_root";
  EXPECT_THROW(
      ghostfs::resolve_workspace_path(root, "../outside.gfs"),
      std::runtime_error);
  EXPECT_THROW(
      ghostfs::resolve_workspace_path(root, "/etc/passwd"),
      std::invalid_argument);
  EXPECT_NO_THROW({
    const auto safe = ghostfs::resolve_workspace_path(root, "lab/sample.gfs");
    EXPECT_EQ(safe.filename(), "sample.gfs");
  });
}

}  // namespace
