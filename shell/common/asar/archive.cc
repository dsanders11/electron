// Copyright (c) 2014 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "shell/common/asar/archive.h"

#include <string>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/pickle.h"
#include "base/strings/string_number_conversions.h"
#include "base/threading/thread_restrictions.h"
#include "base/values.h"
#include "electron/fuses.h"
#include "shell/common/asar/asar_util.h"
#include "shell/common/asar/scoped_temporary_file.h"

#if BUILDFLAG(IS_WIN)
#include <io.h>
#endif

namespace asar {

namespace {

#if BUILDFLAG(IS_WIN)
const char kSeparators[] = "\\/";
#else
const char kSeparators[] = "/";
#endif

const base::Value::Dict* GetNodeFromPath(std::string path,
                                         const base::Value::Dict& root);

// Gets the "files" from "dir".
const base::Value::Dict* GetFilesNode(const base::Value::Dict& root,
                                      const base::Value::Dict& dir) {
  // Test for symbol linked directory.
  const std::string* link = dir.FindString("link");
  if (link != nullptr) {
    const base::Value::Dict* linked_node = GetNodeFromPath(*link, root);
    if (!linked_node)
      return nullptr;
    return linked_node->FindDict("files");
  }

  return dir.FindDict("files");
}

// Gets sub-file "name" from "dir".
const base::Value::Dict* GetChildNode(const base::Value::Dict& root,
                                      const std::string& name,
                                      const base::Value::Dict& dir) {
  if (name.empty())
    return &root;

  const base::Value::Dict* files = GetFilesNode(root, dir);
  return files ? files->FindDict(name) : nullptr;
}

// Gets the node of "path" from "root".
const base::Value::Dict* GetNodeFromPath(std::string path,
                                         const base::Value::Dict& root) {
  if (path.empty())
    return &root;

  const base::Value::Dict* dir = &root;
  for (size_t delimiter_position = path.find_first_of(kSeparators);
       delimiter_position != std::string::npos;
       delimiter_position = path.find_first_of(kSeparators)) {
    const base::Value::Dict* child =
        GetChildNode(root, path.substr(0, delimiter_position), *dir);
    if (!child)
      return nullptr;

    dir = child;
    path.erase(0, delimiter_position + 1);
  }

  return GetChildNode(root, path, *dir);
}

bool FillFileInfoWithNode(Archive::FileInfo* info,
                          uint32_t header_size,
                          bool load_integrity,
                          const base::Value::Dict* node) {
  if (absl::optional<int> size = node->FindInt("size")) {
    info->size = static_cast<uint32_t>(*size);
  } else {
    return false;
  }

  if (absl::optional<bool> unpacked = node->FindBool("unpacked")) {
    info->unpacked = *unpacked;
    if (info->unpacked) {
      return true;
    }
  }

  const std::string* offset = node->FindString("offset");
  if (offset &&
      base::StringToUint64(base::StringPiece(*offset), &info->offset)) {
    info->offset += header_size;
  } else {
    return false;
  }

  if (absl::optional<bool> executable = node->FindBool("executable")) {
    info->executable = *executable;
  }

#if BUILDFLAG(IS_MAC)
  if (load_integrity &&
      electron::fuses::IsEmbeddedAsarIntegrityValidationEnabled()) {
    if (const base::Value::Dict* integrity = node->FindDict("integrity")) {
      const std::string* algorithm = integrity->FindString("algorithm");
      const std::string* hash = integrity->FindString("hash");
      absl::optional<int> block_size = integrity->FindInt("blockSize");
      const base::Value::List* blocks = integrity->FindList("blocks");

      if (algorithm && hash && block_size && block_size > 0 && blocks) {
        IntegrityPayload integrity_payload;
        integrity_payload.hash = *hash;
        integrity_payload.block_size =
            static_cast<uint32_t>(block_size.value());
        for (auto& value : *blocks) {
          if (const std::string* block = value.GetIfString()) {
            integrity_payload.blocks.push_back(*block);
          } else {
            LOG(FATAL)
                << "Invalid block integrity value for file in ASAR archive";
          }
        }
        if (*algorithm == "SHA256") {
          integrity_payload.algorithm = HashAlgorithm::SHA256;
          info->integrity = std::move(integrity_payload);
        }
      }
    }

    if (!info->integrity.has_value()) {
      LOG(FATAL) << "Failed to read integrity for file in ASAR archive";
      return false;
    }
  }
#endif

  return true;
}

}  // namespace

IntegrityPayload::IntegrityPayload()
    : algorithm(HashAlgorithm::NONE), block_size(0) {}
IntegrityPayload::~IntegrityPayload() = default;
IntegrityPayload::IntegrityPayload(const IntegrityPayload& other) = default;

Archive::FileInfo::FileInfo()
    : unpacked(false), executable(false), size(0), offset(0) {}
Archive::FileInfo::~FileInfo() = default;

Archive::Archive(const base::FilePath& path, const uint8_t* data, size_t length)
    : initialized_(false), path_(path), data_(data), length_(length) {
}

Archive::Archive(const base::FilePath& path)
    : initialized_(false), path_(path) {
  base::ThreadRestrictions::ScopedAllowIO allow_io;
  if (base::PathExists(path_) && !file_.Initialize(path_)) {
    LOG(ERROR) << "Failed to open ASAR archive at '" << path_.value() << "'";
  }
}

Archive::~Archive() {}

bool Archive::Init() {
  // Should only be initialized once
  CHECK(!initialized_);
  initialized_ = true;

  if (!data_ && !file_.IsValid()) {
    return false;
  } else if (file_.IsValid()) {
    data_ = file_.data();
    length_ = file_.length();
  }

  if (length_ < 8) {
    LOG(ERROR) << "Malformed ASAR file at '" << path_.value()
               << "' (too short)";
    return false;
  }

  uint32_t size;
  base::PickleIterator size_pickle(
      base::Pickle(reinterpret_cast<const char*>(data_), 8));
  if (!size_pickle.ReadUInt32(&size)) {
    LOG(ERROR) << "Failed to read header size at '" << path_.value() << "'";
    return false;
  }

  if (length_ - 8 < size) {
    LOG(ERROR) << "Malformed ASAR file at '" << path_.value()
               << "' (incorrect header)";
    return false;
  }

  base::PickleIterator header_pickle(
      base::Pickle(reinterpret_cast<const char*>(data_ + 8), size));
  std::string header;
  if (!header_pickle.ReadString(&header)) {
    LOG(ERROR) << "Failed to read header string at '" << path_.value() << "'";
    return false;
  }

#if BUILDFLAG(IS_MAC)
  // Validate header signature if required and possible
  if (electron::fuses::IsEmbeddedAsarIntegrityValidationEnabled() &&
      RelativePath().has_value()) {
    absl::optional<IntegrityPayload> integrity = HeaderIntegrity();
    if (!integrity.has_value()) {
      LOG(FATAL) << "Failed to get integrity for validatable asar archive: "
                 << RelativePath().value();
      return false;
    }

    // Currently we only support the sha256 algorithm, we can add support for
    // more below ensure we read them in preference order from most secure to
    // least
    if (integrity.value().algorithm != HashAlgorithm::NONE) {
      ValidateIntegrityOrDie(header.c_str(), header.length(),
                             integrity.value());
    } else {
      LOG(FATAL) << "No eligible hash for validatable asar archive: "
                 << RelativePath().value();
    }

    header_validated_ = true;
  }
#endif

  absl::optional<base::Value> value = base::JSONReader::Read(header);
  if (!value || !value->is_dict()) {
    LOG(ERROR) << "Header was not valid JSON at '" << path_.value() << "'";
    return false;
  }

  header_size_ = 8 + size;
  header_ = std::move(value->GetDict());
  return true;
}

#if !BUILDFLAG(IS_MAC)
absl::optional<IntegrityPayload> Archive::HeaderIntegrity() const {
  return absl::nullopt;
}

absl::optional<base::FilePath> Archive::RelativePath() const {
  return absl::nullopt;
}
#endif

bool Archive::GetFileInfo(const base::FilePath& path, FileInfo* info) const {
  if (!header_)
    return false;

  const base::Value::Dict* node =
      GetNodeFromPath(path.AsUTF8Unsafe(), *header_);
  if (!node)
    return false;

  const std::string* link = node->FindString("link");
  if (link)
    return GetFileInfo(base::FilePath::FromUTF8Unsafe(*link), info);

  return FillFileInfoWithNode(info, header_size_, header_validated_, node);
}

bool Archive::Stat(const base::FilePath& path, Stats* stats) const {
  if (!header_)
    return false;

  const base::Value::Dict* node =
      GetNodeFromPath(path.AsUTF8Unsafe(), *header_);
  if (!node)
    return false;

  if (node->Find("link")) {
    stats->is_file = false;
    stats->is_link = true;
    return true;
  }

  if (node->Find("files")) {
    stats->is_file = false;
    stats->is_directory = true;
    return true;
  }

  return FillFileInfoWithNode(stats, header_size_, header_validated_, node);
}

bool Archive::Readdir(const base::FilePath& path,
                      std::vector<base::FilePath>* files) const {
  if (!header_)
    return false;

  const base::Value::Dict* node =
      GetNodeFromPath(path.AsUTF8Unsafe(), *header_);
  if (!node)
    return false;

  const base::Value::Dict* files_node = GetFilesNode(*header_, *node);
  if (!files_node)
    return false;

  for (const auto iter : *files_node)
    files->push_back(base::FilePath::FromUTF8Unsafe(iter.first));
  return true;
}

bool Archive::Realpath(const base::FilePath& path,
                       base::FilePath* realpath) const {
  if (!header_)
    return false;

  const base::Value::Dict* node =
      GetNodeFromPath(path.AsUTF8Unsafe(), *header_);
  if (!node)
    return false;

  const std::string* link = node->FindString("link");
  if (link) {
    *realpath = base::FilePath::FromUTF8Unsafe(*link);
    return true;
  }

  *realpath = path;
  return true;
}

bool Archive::CopyFileOut(const base::FilePath& path, base::FilePath* out) {
  if (!header_)
    return false;

  base::AutoLock auto_lock(external_files_lock_);

  auto it = external_files_.find(path.value());
  if (it != external_files_.end()) {
    *out = it->second->path();
    return true;
  }

  FileInfo info;
  if (!GetFileInfo(path, &info))
    return false;

  if (info.unpacked) {
    *out = path_.AddExtension(FILE_PATH_LITERAL("unpacked")).Append(path);
    return true;
  }

  base::CheckedNumeric<uint64_t> safe_offset(info.offset);
  auto safe_end = safe_offset + info.size;
  if (!safe_end.IsValid() || safe_end.ValueOrDie() > length_)
    return false;

  auto temp_file = std::make_unique<ScopedTemporaryFile>();
  base::FilePath::StringType ext = path.Extension();
  if (!temp_file->Init(ext))
    return false;

  base::File dest(temp_file->path(),
                  base::File::FLAG_OPEN | base::File::FLAG_WRITE);
  if (!dest.IsValid())
    return false;

  dest.WriteAtCurrentPos(
      reinterpret_cast<const char*>(data_ + info.offset), info.size);

#if BUILDFLAG(IS_POSIX)
  if (info.executable) {
    // chmod a+x temp_file;
    base::SetPosixFilePermissions(temp_file->path(), 0755);
  }
#endif

  *out = temp_file->path();
  external_files_[path.value()] = std::move(temp_file);
  return true;
}

}  // namespace asar
