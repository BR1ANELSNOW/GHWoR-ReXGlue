/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <array>
#include <cstring>
#include <queue>
#include <string>

#include <fmt/format.h>

#include <rex/filesystem.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/string.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_device.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xfile.h>
#include <rex/system/xobject.h>

namespace rex {
namespace system {
namespace xam {

static const char* kThumbnailFileName = "__thumbnail.png";

static const char* kGameUserContentDirName = "profile";

static const char* kGameContentHeaderDirName = "Headers";

static int content_device_id_ = 0;

ContentPackage::ContentPackage(KernelState* kernel_state, const std::string_view root_name,
                               const XCONTENT_AGGREGATE_DATA& data,
                               const std::filesystem::path& package_path)
    : kernel_state_(kernel_state), root_name_(root_name), package_path_(package_path), license_(0) {
  device_path_ = fmt::format("\\Device\\Content\\{0}\\", ++content_device_id_);
  content_data_ = data;

  auto fs = kernel_state_->file_system();

  if (std::filesystem::is_regular_file(package_path_)) {
    auto device =
        std::make_unique<rex::filesystem::StfsContainerDevice>(
            device_path_, package_path_);

    if (!device->Initialize()) {
      REXLOG_ERROR(
          "GHWOR XBOX CONTENT: failed to mount STFS '{}'",
          rex::path_to_utf8(package_path_));
      return;
    }

    // Xbox-style license: read it directly from the LIVE/STFS header.
    for (size_t i = 0; i < 0x10; ++i) {
      if (device->header().header.licenses[i].license_flags) {
        license_ |= device->header().header.licenses[i].license_bits;
      }
    }

    fs->RegisterDevice(std::move(device));

    REXLOG_INFO(
        "GHWOR XBOX CONTENT: mounted LIVE '{}' license=0x{:08X}",
        rex::path_to_utf8(package_path_), license_);
  } else {
    auto device =
        std::make_unique<rex::filesystem::HostPathDevice>(
            device_path_, package_path_, false,
            /*allow_share_delete=*/true);

    if (!device->Initialize()) {
      REXLOG_ERROR(
          "GHWOR CONTENT: failed to mount directory '{}'",
          rex::path_to_utf8(package_path_));
      return;
    }

    fs->RegisterDevice(std::move(device));
  }

  fs->RegisterSymbolicLink(root_name_ + ":", device_path_);
}

ContentPackage::~ContentPackage() {
  auto fs = kernel_state_->file_system();
  fs->UnregisterSymbolicLink(root_name_ + ":");
  fs->UnregisterDevice(device_path_);
}

void ContentPackage::LoadPackageLicenseMask(const std::filesystem::path header_path) {
  if (!std::filesystem::exists(header_path)) {
    return;
  }

  auto file = rex::filesystem::OpenFile(header_path, "rb");
  if (!file) {
    return;
  }

  auto file_size = std::filesystem::file_size(header_path);
  if (file_size < sizeof(XCONTENT_AGGREGATE_DATA) + sizeof(license_)) {
    fclose(file);
    return;
  }

  fseek(file, sizeof(XCONTENT_AGGREGATE_DATA), SEEK_SET);
  fread(&license_, 1, sizeof(license_), file);
  fclose(file);
}

ContentManager::ContentManager(KernelState* kernel_state,
                               const std::filesystem::path& root_path)
    : kernel_state_(kernel_state), root_path_(root_path) {
  // GHWoR Xbox 360-style content layout.
  // Marketplace LIVE/STFS packages are read directly from:
  //
  // Content/0000000000000000/<TitleID>/00000002/<LIVE>
}

ContentManager::~ContentManager() = default;

std::filesystem::path ContentManager::ResolvePackageRoot(uint64_t xuid, XContentType content_type,
                                                         uint32_t title_id) {
  if (title_id == kCurrentlyRunningTitleId) {
    title_id = kernel_state_->title_id();
  }
  auto xuid_str = fmt::format("{:016X}", xuid);
  auto title_id_str = fmt::format("{:08X}", title_id);
  auto content_type_str = fmt::format("{:08X}", uint32_t(content_type));

  // Package root path:
  // content_root/xuid/title_id/content_type/
  return root_path_ / xuid_str / title_id_str / content_type_str;
}

std::filesystem::path ContentManager::ResolvePackagePath(
    uint64_t xuid, const XCONTENT_AGGREGATE_DATA& data) {
  uint32_t title_id = data.title_id;

  if (title_id == kCurrentlyRunningTitleId) {
    title_id = kernel_state_->title_id();
  }

  // Xbox 360 Marketplace/DLC layout:
  // Content/0000000000000000/<TitleID>/00000002/<LIVE>
  if (data.content_type == XContentType::kMarketplaceContent) {
    return root_path_ /
           "Content" /
           "0000000000000000" /
           fmt::format("{:08X}", title_id) /
           fmt::format("{:08X}", static_cast<uint32_t>(data.content_type.get())) /
           rex::to_path(data.file_name());
  }

  uint64_t used_xuid =
      (data.xuid != uint64_t(-1) && data.xuid != 0)
          ? uint64_t(data.xuid)
          : xuid;

  auto package_root =
      ResolvePackageRoot(used_xuid, data.content_type, title_id);

  return package_root / rex::to_path(data.file_name());
}

std::filesystem::path ContentManager::ResolvePackageHeaderPath(const std::string_view file_name,
                                                               uint64_t xuid, uint32_t title_id,
                                                               XContentType content_type) const {
  if (title_id == kCurrentlyRunningTitleId) {
    title_id = kernel_state_->title_id();
  }

  if (content_type == XContentType::kMarketplaceContent) {
    xuid = 0;
  }

  auto xuid_str = fmt::format("{:016X}", xuid);
  auto title_id_str = fmt::format("{:08X}", title_id);
  auto content_type_str = fmt::format("{:08X}", uint32_t(content_type));
  std::string final_name = std::string(file_name) + ".header";

  // Header root path:
  // content_root/xuid/title_id/Headers/content_type/filename.header
  return root_path_ / xuid_str / title_id_str / kGameContentHeaderDirName / content_type_str /
         final_name;
}

std::vector<XCONTENT_AGGREGATE_DATA> ContentManager::ListContent(
    uint32_t device_id, uint64_t xuid,
    XContentType content_type, uint32_t title_id) {
  std::vector<XCONTENT_AGGREGATE_DATA> result;

  if (title_id == kCurrentlyRunningTitleId) {
    title_id = kernel_state_->title_id();
  }

  // ==========================================================
  // Xbox 360 Marketplace content.
  // Only common content (0000000000000000) is valid here.
  // ==========================================================
  if (content_type == XContentType::kMarketplaceContent) {
    if (xuid != 0) {
      return result;
    }

    auto package_root =
        root_path_ /
        "Content" /
        "0000000000000000" /
        fmt::format("{:08X}", title_id) /
        fmt::format("{:08X}", uint32_t(content_type));

    auto file_infos = rex::filesystem::ListFiles(package_root);

    for (const auto& file_info : file_infos) {
      // Old extracted directories are deliberately ignored.
      if (file_info.type ==
          rex::filesystem::FileInfo::Type::kDirectory) {
        continue;
      }

      const auto package_path =
          package_root / file_info.name;

      auto device =
          std::make_unique<rex::filesystem::StfsContainerDevice>(
              "", package_path);

      if (!device->Initialize()) {
        REXLOG_WARN(
            "GHWOR XBOX CONTENT: ignoring non-STFS file '{}'",
            rex::path_to_utf8(package_path));
        continue;
      }

      const uint32_t package_title_id =
          static_cast<uint32_t>(
              device->header().metadata.execution_info.title_id);

      // The directory Title ID must agree with the STFS metadata.
      if (package_title_id != title_id) {
        REXLOG_WARN(
            "GHWOR XBOX CONTENT: title mismatch '{}' "
            "package={:08X} folder={:08X}",
            rex::path_to_utf8(package_path),
            package_title_id,
            title_id);
        continue;
      }

      XCONTENT_AGGREGATE_DATA content_data{};

      content_data.device_id = device_id;
      content_data.content_type = content_type;
      content_data.title_id = package_title_id;
      content_data.xuid = 0;

      content_data.set_file_name(
          rex::path_to_utf8(file_info.name));

      auto display_name =
          device->header().metadata.display_name(
              rex::system::XLanguage::kEnglish);

      if (!display_name.empty()) {
        content_data.set_display_name(display_name);
      } else {
        content_data.set_display_name(
            rex::path_to_utf16(file_info.name));
      }

      REXLOG_INFO(
          "GHWOR XBOX CONTENT: enumerated LIVE '{}' title={:08X}",
          rex::path_to_utf8(file_info.name),
          package_title_id);

      result.emplace_back(std::move(content_data));
    }

    return result;
  }

  // ==========================================================
  // Existing non-Marketplace content behavior.
  // Saves / profiles / other content remain untouched.
  // ==========================================================

  auto package_root =
      ResolvePackageRoot(xuid, content_type, title_id);

  auto file_infos =
      rex::filesystem::ListFiles(package_root);

  for (const auto& file_info : file_infos) {
    if (file_info.type !=
        rex::filesystem::FileInfo::Type::kDirectory) {
      continue;
    }

    XCONTENT_AGGREGATE_DATA content_data{};

    if (XSUCCEEDED(
            ReadContentHeaderFile(
                rex::path_to_utf8(file_info.name),
                xuid,
                title_id,
                content_type,
                content_data))) {
      result.emplace_back(std::move(content_data));
    } else {
      content_data.device_id = device_id;
      content_data.content_type = content_type;
      content_data.set_display_name(
          rex::path_to_utf16(file_info.name));
      content_data.set_file_name(
          rex::path_to_utf8(file_info.name));
      content_data.title_id = title_id;
      content_data.xuid = xuid;

      result.emplace_back(std::move(content_data));
    }
  }

  return result;
}

std::unique_ptr<ContentPackage> ContentManager::ResolvePackage(
    const std::string_view root_name, uint64_t xuid, const XCONTENT_AGGREGATE_DATA& data) {
  auto package_path = ResolvePackagePath(xuid, data);
  if (!std::filesystem::exists(package_path)) {
    return nullptr;
  }
  auto package = std::make_unique<ContentPackage>(kernel_state_, root_name, data, package_path);
  return package;
}

bool ContentManager::ContentExists(uint64_t xuid, const XCONTENT_AGGREGATE_DATA& data) {
  auto path = ResolvePackagePath(xuid, data);
  return std::filesystem::exists(path);
}

X_RESULT ContentManager::WriteContentHeaderFile(uint64_t xuid, XCONTENT_AGGREGATE_DATA data,
                                                uint32_t license_mask) {
  if (data.title_id == uint32_t(-1)) {
    data.title_id = kernel_state_->title_id();
  }
  if (data.xuid == uint64_t(-1)) {
    data.xuid = xuid;
  }
  uint64_t used_xuid = (data.xuid != uint64_t(-1) && data.xuid != 0) ? uint64_t(data.xuid) : xuid;

  auto header_path =
      ResolvePackageHeaderPath(data.file_name(), used_xuid, data.title_id, data.content_type);
  auto parent_path = header_path.parent_path();

  if (!std::filesystem::exists(parent_path)) {
    if (!std::filesystem::create_directories(parent_path)) {
      return X_ERROR_ACCESS_DENIED;
    }
  }

  rex::filesystem::CreateEmptyFile(header_path);

  auto file = rex::filesystem::OpenFile(header_path, "wb");
  if (!file) {
    return X_ERROR_FILE_NOT_FOUND;
  }
  fwrite(&data, 1, sizeof(XCONTENT_AGGREGATE_DATA), file);
  if (license_mask != 0) {
    fwrite(&license_mask, 1, sizeof(license_mask), file);
  }
  fclose(file);
  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::ReadContentHeaderFile(const std::string_view file_name, uint64_t xuid,
                                               uint32_t title_id, XContentType content_type,
                                               XCONTENT_AGGREGATE_DATA& data) const {
  auto header_file_path = ResolvePackageHeaderPath(file_name, xuid, title_id, content_type);
  constexpr uint32_t header_size = sizeof(XCONTENT_AGGREGATE_DATA);

  if (!std::filesystem::exists(header_file_path)) {
    return X_ERROR_FILE_NOT_FOUND;
  }

  auto file = rex::filesystem::OpenFile(header_file_path, "rb");
  if (!file) {
    return X_ERROR_FILE_NOT_FOUND;
  }

  auto file_size = std::filesystem::file_size(header_file_path);
  if (file_size < header_size) {
    fclose(file);
    return X_ERROR_FILE_NOT_FOUND;
  }

  std::array<uint8_t, header_size> buffer;
  size_t result = fread(buffer.data(), 1, header_size, file);
  fclose(file);

  if (result != header_size) {
    return X_ERROR_FILE_NOT_FOUND;
  }

  std::memcpy(&data, buffer.data(), buffer.size());
  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::CreateContent(const std::string_view root_name, uint64_t xuid,
                                       const XCONTENT_AGGREGATE_DATA& data) {
  {
    auto global_lock = global_critical_region_.Acquire();
    if (open_packages_.count(string::string_key_case(root_name))) {
      return X_ERROR_ALREADY_EXISTS;
    }
  }

  auto package_path = ResolvePackagePath(xuid, data);
  if (std::filesystem::exists(package_path)) {
    return X_ERROR_ALREADY_EXISTS;
  }
  if (!std::filesystem::create_directories(package_path)) {
    return X_ERROR_ACCESS_DENIED;
  }
  auto package = ResolvePackage(root_name, xuid, data);
  assert_not_null(package);

  {
    auto global_lock = global_critical_region_.Acquire();
    if (open_packages_.count(string::string_key_case(root_name))) {
      return X_ERROR_ALREADY_EXISTS;
    }
    open_packages_.insert({string::string_key_case::create(root_name), package.release()});
  }
  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::OpenContent(
    const std::string_view root_name, uint64_t xuid,
    const XCONTENT_AGGREGATE_DATA& data,
    uint32_t& content_license) {
  // GHWoR: reuse an identical Marketplace package retained under this root.
  // If the root now refers to different content, replace the old mount.
  ContentPackage* replaced_package = nullptr;
  {
    auto global_lock = global_critical_region_.Acquire();
    auto existing_it =
        open_packages_.find(string::string_key_case(root_name));

    if (existing_it != open_packages_.end()) {
      const auto& existing_data =
          existing_it->second->GetPackageContentData();

      if (existing_data.content_type == data.content_type &&
          existing_data.file_name() == data.file_name() &&
          existing_data.title_id == data.title_id) {
        content_license =
            existing_it->second->GetPackageLicense();
        return X_ERROR_SUCCESS;
      }

      replaced_package = DetachPackage(existing_it);
    }
  }

  delete replaced_package;

  auto package_path =
      ResolvePackagePath(xuid, data);

  if (!std::filesystem::exists(package_path)) {
    return X_ERROR_FILE_NOT_FOUND;
  }

  auto package =
      ResolvePackage(root_name, xuid, data);

  if (!package) {
    return X_ERROR_FILE_NOT_FOUND;
  }

  // Folder-based legacy content still uses our sidecar header.
  // Raw LIVE/STFS gets the license from ContentPackage itself.
  if (std::filesystem::is_directory(package_path)) {
    package->LoadPackageLicenseMask(
        ResolvePackageHeaderPath(
            data.file_name(),
            xuid,
            data.title_id,
            data.content_type));
  }

  content_license =
      package->GetPackageLicense();

  {
    auto global_lock =
        global_critical_region_.Acquire();

    if (open_packages_.count(
            string::string_key_case(root_name))) {
      return X_ERROR_ALREADY_EXISTS;
    }

    open_packages_.insert(
        {string::string_key_case::create(root_name),
         package.release()});
  }

  REXLOG_INFO(
      "GHWOR XBOX CONTENT: CONT0 mounted '{}' license=0x{:08X}",
      rex::path_to_utf8(package_path),
      content_license);

  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::CloseContent(const std::string_view root_name) {
  ContentPackage* package = nullptr;
  {
    auto global_lock = global_critical_region_.Acquire();
    // Some games use different casing between Create and Close (e.g. "save" vs "SAVE")
    auto it = open_packages_.find(string::string_key_case(root_name));
    if (it == open_packages_.end()) {
      return X_ERROR_FILE_NOT_FOUND;
    }
    const auto& package_data =
        it->second->GetPackageContentData();

    // GHWoR repeatedly closes and reopens immutable LIVE/STFS DLC.
    // Retain only Marketplace packages so their parsed tree can be reused.
    if (package_data.content_type ==
        XContentType::kMarketplaceContent) {
      return X_ERROR_SUCCESS;
    }

    package = DetachPackage(it);
  }
  delete package;
  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::GetContentThumbnail(uint64_t xuid, const XCONTENT_AGGREGATE_DATA& data,
                                             std::vector<uint8_t>* buffer) {
  auto global_lock = global_critical_region_.Acquire();
  auto package_path = ResolvePackagePath(xuid, data);
  auto thumb_path = package_path / kThumbnailFileName;
  if (std::filesystem::exists(thumb_path)) {
    auto file = rex::filesystem::OpenFile(thumb_path, "rb");
    fseek(file, 0, SEEK_END);
    size_t file_len = ftell(file);
    fseek(file, 0, SEEK_SET);
    buffer->resize(file_len);
    fread(const_cast<uint8_t*>(buffer->data()), 1, buffer->size(), file);
    fclose(file);
    return X_ERROR_SUCCESS;
  } else {
    return X_ERROR_FILE_NOT_FOUND;
  }
}

X_RESULT ContentManager::SetContentThumbnail(uint64_t xuid, const XCONTENT_AGGREGATE_DATA& data,
                                             std::vector<uint8_t> buffer) {
  auto global_lock = global_critical_region_.Acquire();
  auto package_path = ResolvePackagePath(xuid, data);
  std::filesystem::create_directories(package_path);
  if (std::filesystem::exists(package_path)) {
    auto thumb_path = package_path / kThumbnailFileName;
    auto file = rex::filesystem::OpenFile(thumb_path, "wb");
    fwrite(buffer.data(), 1, buffer.size(), file);
    fclose(file);
    return X_ERROR_SUCCESS;
  } else {
    return X_ERROR_FILE_NOT_FOUND;
  }
}

X_RESULT ContentManager::DeleteContent(uint64_t xuid, const XCONTENT_AGGREGATE_DATA& data) {
  auto global_lock = global_critical_region_.Acquire();

  if (IsContentOpen(data)) {
    // TODO(Gliniak): Get real error code for this case.
    return X_ERROR_ACCESS_DENIED;
  }

  auto package_path = ResolvePackagePath(xuid, data);
  std::error_code ec;
  auto dir_removed = std::filesystem::remove_all(package_path, ec);
  if (ec) {
    return X_ERROR_ACCESS_DENIED;
  }

  uint64_t used_xuid = (data.xuid != uint64_t(-1) && data.xuid != 0) ? uint64_t(data.xuid) : xuid;
  auto header_path =
      ResolvePackageHeaderPath(data.file_name(), used_xuid, data.title_id, data.content_type);
  std::error_code ec2;
  bool header_removed = std::filesystem::remove(header_path, ec2);

  if (dir_removed > 0 || header_removed) {
    return X_ERROR_SUCCESS;
  }
  return X_ERROR_FILE_NOT_FOUND;
}

X_RESULT ContentManager::UnmountContent(uint64_t xuid, const XCONTENT_AGGREGATE_DATA& data) {
  ContentPackage* package = nullptr;
  {
    auto global_lock = global_critical_region_.Acquire();
    auto it = FindOpenPackageByData(data);
    if (it == open_packages_.end()) {
      return X_ERROR_FILE_NOT_FOUND;
    }
    package = DetachPackage(it);
  }
  delete package;
  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::UnmountAndDeleteContent(uint64_t xuid,
                                                 const XCONTENT_AGGREGATE_DATA& data) {
  // Unmount phase: tolerant of not-mounted state
  ContentPackage* package = nullptr;
  {
    auto global_lock = global_critical_region_.Acquire();
    auto it = FindOpenPackageByData(data);
    if (it != open_packages_.end()) {
      package = DetachPackage(it);
    }
  }
  delete package;

  // Delete phase: remove package directory and .header file
  auto package_path = ResolvePackagePath(xuid, data);

  uint64_t used_xuid = (data.xuid != uint64_t(-1) && data.xuid != 0) ? uint64_t(data.xuid) : xuid;
  auto header_path =
      ResolvePackageHeaderPath(data.file_name(), used_xuid, data.title_id, data.content_type);

  std::error_code ec;
  auto dir_removed = std::filesystem::remove_all(package_path, ec);
  if (ec) {
    return X_ERROR_ACCESS_DENIED;
  }

  std::error_code ec2;
  bool header_removed = std::filesystem::remove(header_path, ec2);

  if (dir_removed > 0 || header_removed) {
    return X_ERROR_SUCCESS;
  }
  return X_ERROR_FILE_NOT_FOUND;
}

std::filesystem::path ContentManager::ResolveGameUserContentPath() {
  auto title_id = fmt::format("{:08X}", kernel_state_->title_id());
  auto user_name = rex::to_path(kernel_state_->user_profile()->name());

  // Per-game per-profile data location:
  // content_root/title_id/profile/user_name
  return root_path_ / title_id / kGameUserContentDirName / user_name;
}

std::unordered_map<string::string_key_case, ContentPackage*,
                   string::string_key_case::Hash>::iterator
ContentManager::FindOpenPackageByData(const XCONTENT_AGGREGATE_DATA& data) {
  // Resolve kCurrentlyRunningTitleId so both sides compare actual title IDs.
  uint32_t query_title = data.title_id;
  if (query_title == kCurrentlyRunningTitleId) {
    query_title = kernel_state_->title_id();
  }

  for (auto it = open_packages_.begin(); it != open_packages_.end(); ++it) {
    const auto& pkg = it->second->GetPackageContentData();

    uint32_t pkg_title = pkg.title_id;
    if (pkg_title == kCurrentlyRunningTitleId) {
      pkg_title = kernel_state_->title_id();
    }

    // Match on content_type + file_name + resolved title_id.
    // device_id is a virtual storage selector, not a content identifier.
    if (data.content_type == pkg.content_type && data.file_name() == pkg.file_name() &&
        query_title == pkg_title) {
      return it;
    }
  }
  return open_packages_.end();
}

ContentPackage* ContentManager::DetachPackage(
    std::unordered_map<string::string_key_case, ContentPackage*,
                       string::string_key_case::Hash>::iterator it) {
  CloseOpenedFilesFromContent(it->first.view());
  ContentPackage* package = it->second;
  open_packages_.erase(it);
  return package;
}

bool ContentManager::IsContentOpen(const XCONTENT_AGGREGATE_DATA& data) const {
  return std::any_of(open_packages_.cbegin(), open_packages_.cend(), [&data](const auto& content) {
    return data == content.second->GetPackageContentData();
  });
}

std::filesystem::path ContentManager::GetOpenPackagePath(const std::string_view root_name) const {
  auto it = open_packages_.find(string::string_key_case(root_name));
  if (it == open_packages_.end()) {
    return {};
  }
  return it->second->package_path();
}

void ContentManager::CloseOpenedFilesFromContent(const std::string_view root_name) {
  // TODO(Gliniak): Cleanup this code to care only about handles
  // related to provided content
  const std::vector<object_ref<XFile>> all_files_handles =
      kernel_state_->object_table()->GetObjectsByType<XFile>(XObject::Type::File);

  std::string resolved_path;
  const bool ghwor_content_root_resolved =
      kernel_state_->file_system()->FindSymbolicLink(
          std::string(root_name) + ':', resolved_path);

  // GHWOR CONTENT HANDLE GUARD
  //
  // Never release unrelated XFile handles if the requested
  // content root cannot be resolved. An empty prefix matches
  // every file path and would invalidate handles outside
  // the content package, including files under GAME:\.
  if (!ghwor_content_root_resolved || resolved_path.empty()) {
    return;
  }

  for (const object_ref<XFile>& file : all_files_handles) {
    std::string file_path = file->entry()->absolute_path();
    bool is_file_inside_content = rex::string::utf8_starts_with(file_path, resolved_path);

    if (is_file_inside_content) {
      file->ReleaseHandle();
    }
  }
}

static X_RESULT ExtractEntry(rex::filesystem::Entry* entry,
                             const std::filesystem::path& base_path) {
  auto dest_path = base_path / rex::to_path(rex::string::utf8_fix_path_separators(entry->path()));

  if (entry->attributes() & rex::filesystem::kFileAttributeDirectory) {
    std::error_code ec;
    std::filesystem::create_directories(dest_path, ec);
    if (ec) {
      return X_ERROR_ACCESS_DENIED;
    }
    return X_ERROR_SUCCESS;
  }

  // Ensure parent directory exists
  std::error_code ec;
  std::filesystem::create_directories(dest_path.parent_path(), ec);

  rex::filesystem::File* in_file = nullptr;
  X_STATUS status = entry->Open(rex::filesystem::FileAccess::kFileReadData, &in_file);
  if (status != X_STATUS_SUCCESS) {
    return X_ERROR_ACCESS_DENIED;
  }

  auto out_file = rex::filesystem::OpenFile(dest_path, "wb");
  if (!out_file) {
    in_file->Destroy();
    return X_ERROR_ACCESS_DENIED;
  }

  constexpr size_t kBufferSize = 4 * 1024 * 1024;  // 4 MiB
  auto buffer = std::make_unique<uint8_t[]>(kBufferSize);
  size_t remaining = entry->size();
  size_t offset = 0;

  while (remaining > 0) {
    size_t bytes_read = 0;
    size_t to_read = std::min(remaining, kBufferSize);
    in_file->ReadSync(std::span<uint8_t>(buffer.get(), to_read), offset, &bytes_read);
    if (bytes_read == 0) {
      break;
    }
    fwrite(buffer.get(), 1, bytes_read, out_file);
    offset += bytes_read;
    remaining -= bytes_read;
  }

  fclose(out_file);
  in_file->Destroy();
  return X_ERROR_SUCCESS;
}

X_RESULT ContentManager::InstallContent(
    const std::filesystem::path& package_path) {
  if (!std::filesystem::exists(package_path) ||
      !std::filesystem::is_regular_file(package_path)) {
    return X_ERROR_FILE_NOT_FOUND;
  }

  auto device =
      std::make_unique<rex::filesystem::StfsContainerDevice>(
          "", package_path);

  if (!device->Initialize()) {
    return X_ERROR_ACCESS_DENIED;
  }

  const uint32_t title_id =
      static_cast<uint32_t>(
          device->header().metadata.execution_info.title_id);

  const auto destination_root =
      root_path_ /
      "Content" /
      "0000000000000000" /
      fmt::format("{:08X}", title_id) /
      "00000002";

  const auto destination =
      destination_root / package_path.filename();

  std::error_code ec;

  std::filesystem::create_directories(
      destination_root, ec);

  if (ec) {
    return X_ERROR_ACCESS_DENIED;
  }

  if (package_path == destination) {
    return X_ERROR_SUCCESS;
  }

  if (std::filesystem::exists(destination, ec)) {
    ec.clear();

    if (std::filesystem::is_directory(destination, ec)) {
      ec.clear();
      std::filesystem::remove_all(destination, ec);
    } else {
      ec.clear();
      std::filesystem::remove(destination, ec);
    }

    if (ec) {
      return X_ERROR_ACCESS_DENIED;
    }
  }

  ec.clear();

  std::filesystem::copy_file(
      package_path,
      destination,
      std::filesystem::copy_options::overwrite_existing,
      ec);

  if (ec) {
    return X_ERROR_ACCESS_DENIED;
  }

  REXLOG_INFO(
      "GHWOR XBOX CONTENT: installed raw LIVE '{}' title={:08X}",
      rex::path_to_utf8(destination),
      title_id);

  return X_ERROR_SUCCESS;
}

}  // namespace xam
}  // namespace system
}  // namespace rex

