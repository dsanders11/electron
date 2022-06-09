// Copyright (c) 2014 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include <vector>

#include "base/numerics/safe_math.h"
#include "base/task/thread_pool.h"
#include "gin/handle.h"
#include "shell/common/asar/archive.h"
#include "shell/common/asar/asar_util.h"
#include "shell/common/gin_converters/file_path_converter.h"
#include "shell/common/gin_helper/dictionary.h"
#include "shell/common/gin_helper/function_template_extensions.h"
#include "shell/common/gin_helper/promise.h"
#include "shell/common/node_includes.h"
#include "shell/common/node_util.h"

#include "shell/common/api/postject.h"

#if defined(__linux__) && !defined(__POSTJECT_NO_SHT_PTR)
volatile void* _binary_postject_sht_start = (void*)POSTJECT_SHT_PTR_SENTINEL;
#endif

namespace {

std::unique_ptr<asar::Archive> GetAsarArchive(const base::FilePath& path) {
  if (path.BaseName().MaybeAsASCII() == "app.asar") {
    struct postject_options postject_options;

    postject_options_init(&postject_options);
    postject_options.macho_framework_name = "Electron Framework";
    postject_options.macho_segment_name = "__ELECTRON";

    size_t size;
    const void* ptr =
        postject_find_resource("app_asar", &size, &postject_options);

    if (ptr && size > 0) {
      return std::make_unique<asar::Archive>(
          path, reinterpret_cast<const uint8_t*>(ptr), size);
    }
  }

  return std::make_unique<asar::Archive>(path);
}

class Archive : public node::ObjectWrap {
 public:
  static v8::Local<v8::FunctionTemplate> CreateFunctionTemplate(
      v8::Isolate* isolate) {
    auto tpl = v8::FunctionTemplate::New(isolate, Archive::New);
    tpl->SetClassName(
        v8::String::NewFromUtf8(isolate, "Archive").ToLocalChecked());
    tpl->InstanceTemplate()->SetInternalFieldCount(1);

    NODE_SET_PROTOTYPE_METHOD(tpl, "getFileInfo", &Archive::GetFileInfo);
    NODE_SET_PROTOTYPE_METHOD(tpl, "stat", &Archive::Stat);
    NODE_SET_PROTOTYPE_METHOD(tpl, "readdir", &Archive::Readdir);
    NODE_SET_PROTOTYPE_METHOD(tpl, "realpath", &Archive::Realpath);
    NODE_SET_PROTOTYPE_METHOD(tpl, "copyFileOut", &Archive::CopyFileOut);
    NODE_SET_PROTOTYPE_METHOD(tpl, "read", &Archive::Read);
    NODE_SET_PROTOTYPE_METHOD(tpl, "readSync", &Archive::ReadSync);

    return tpl;
  }

  // disable copy
  Archive(const Archive&) = delete;
  Archive& operator=(const Archive&) = delete;

 protected:
  explicit Archive(std::unique_ptr<asar::Archive> archive)
      : archive_(std::move(archive)) {}

  static void New(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto* isolate = args.GetIsolate();

    base::FilePath path;
    if (!gin::ConvertFromV8(isolate, args[0], &path)) {
      isolate->ThrowException(v8::Exception::Error(node::FIXED_ONE_BYTE_STRING(
          isolate, "failed to convert path to V8")));
      return;
    }

    std::unique_ptr<asar::Archive> archive = GetAsarArchive(path);
    if (!archive->Init()) {
      isolate->ThrowException(v8::Exception::Error(node::FIXED_ONE_BYTE_STRING(
          isolate, "failed to initialize archive")));
      return;
    }

    auto* archive_wrap = new Archive(std::move(archive));
    archive_wrap->Wrap(args.This());
    args.GetReturnValue().Set(args.This());
  }

  // Reads the offset and size of file.
  static void GetFileInfo(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto* isolate = args.GetIsolate();
    auto* wrap = node::ObjectWrap::Unwrap<Archive>(args.Holder());

    base::FilePath path;
    if (!gin::ConvertFromV8(isolate, args[0], &path)) {
      args.GetReturnValue().Set(v8::False(isolate));
      return;
    }

    asar::Archive::FileInfo info;
    if (!wrap->archive_ || !wrap->archive_->GetFileInfo(path, &info)) {
      args.GetReturnValue().Set(v8::False(isolate));
      return;
    }

    gin_helper::Dictionary dict(isolate, v8::Object::New(isolate));
    dict.Set("size", info.size);
    dict.Set("unpacked", info.unpacked);
    dict.Set("offset", info.offset);
    if (info.integrity.has_value()) {
      gin_helper::Dictionary integrity(isolate, v8::Object::New(isolate));
      asar::HashAlgorithm algorithm = info.integrity.value().algorithm;
      switch (algorithm) {
        case asar::HashAlgorithm::SHA256:
          integrity.Set("algorithm", "SHA256");
          break;
        case asar::HashAlgorithm::NONE:
          CHECK(false);
          break;
      }
      integrity.Set("hash", info.integrity.value().hash);
      dict.Set("integrity", integrity);
    }
    args.GetReturnValue().Set(dict.GetHandle());
  }

  // Returns a fake result of fs.stat(path).
  static void Stat(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto* isolate = args.GetIsolate();
    auto* wrap = node::ObjectWrap::Unwrap<Archive>(args.Holder());
    base::FilePath path;
    if (!gin::ConvertFromV8(isolate, args[0], &path)) {
      args.GetReturnValue().Set(v8::False(isolate));
      return;
    }

    asar::Archive::Stats stats;
    if (!wrap->archive_ || !wrap->archive_->Stat(path, &stats)) {
      args.GetReturnValue().Set(v8::False(isolate));
      return;
    }

    gin_helper::Dictionary dict(isolate, v8::Object::New(isolate));
    dict.Set("size", stats.size);
    dict.Set("offset", stats.offset);
    dict.Set("isFile", stats.is_file);
    dict.Set("isDirectory", stats.is_directory);
    dict.Set("isLink", stats.is_link);
    args.GetReturnValue().Set(dict.GetHandle());
  }

  // Returns all files under a directory.
  static void Readdir(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto* isolate = args.GetIsolate();
    auto* wrap = node::ObjectWrap::Unwrap<Archive>(args.Holder());
    base::FilePath path;
    if (!gin::ConvertFromV8(isolate, args[0], &path)) {
      args.GetReturnValue().Set(v8::False(isolate));
      return;
    }

    std::vector<base::FilePath> files;
    if (!wrap->archive_ || !wrap->archive_->Readdir(path, &files)) {
      args.GetReturnValue().Set(v8::False(isolate));
      return;
    }
    args.GetReturnValue().Set(gin::ConvertToV8(isolate, files));
  }

  // Returns the path of file with symbol link resolved.
  static void Realpath(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto* isolate = args.GetIsolate();
    auto* wrap = node::ObjectWrap::Unwrap<Archive>(args.Holder());
    base::FilePath path;
    if (!gin::ConvertFromV8(isolate, args[0], &path)) {
      args.GetReturnValue().Set(v8::False(isolate));
      return;
    }

    base::FilePath realpath;
    if (!wrap->archive_ || !wrap->archive_->Realpath(path, &realpath)) {
      args.GetReturnValue().Set(v8::False(isolate));
      return;
    }
    args.GetReturnValue().Set(gin::ConvertToV8(isolate, realpath));
  }

  // Copy the file out into a temporary file and returns the new path.
  static void CopyFileOut(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto* isolate = args.GetIsolate();
    auto* wrap = node::ObjectWrap::Unwrap<Archive>(args.Holder());
    base::FilePath path;
    if (!gin::ConvertFromV8(isolate, args[0], &path)) {
      args.GetReturnValue().Set(v8::False(isolate));
      return;
    }

    base::FilePath new_path;
    if (!wrap->archive_ || !wrap->archive_->CopyFileOut(path, &new_path)) {
      args.GetReturnValue().Set(v8::False(isolate));
      return;
    }
    args.GetReturnValue().Set(gin::ConvertToV8(isolate, new_path));
  }

  static void ReadSync(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto* isolate = args.GetIsolate();
    auto* wrap = node::ObjectWrap::Unwrap<Archive>(args.Holder());

    uint64_t offset;
    if (!gin::ConvertFromV8(isolate, args[0], &offset)) {
      args.GetReturnValue().Set(v8::False(isolate));
      return;
    }

    uint64_t length;
    if (!gin::ConvertFromV8(isolate, args[1], &length)) {
      args.GetReturnValue().Set(v8::False(isolate));
      return;
    }

    if (!wrap->archive_) {
      args.GetReturnValue().Set(v8::False(isolate));
      return;
    }

    base::CheckedNumeric<uint64_t> safe_offset(offset);
    base::CheckedNumeric<uint64_t> safe_end = safe_offset + length;
    if (!safe_end.IsValid() ||
        safe_end.ValueOrDie() > wrap->archive_->length()) {
      isolate->ThrowException(v8::Exception::Error(
          node::FIXED_ONE_BYTE_STRING(isolate, "Out of bounds read")));
      args.GetReturnValue().Set(v8::Local<v8::ArrayBuffer>());
      return;
    }
    auto array_buffer = v8::ArrayBuffer::New(isolate, length);
    auto backing_store = array_buffer->GetBackingStore();
    memcpy(backing_store->Data(), wrap->archive_->data() + offset, length);
    args.GetReturnValue().Set(array_buffer);
  }

  static void Read(const v8::FunctionCallbackInfo<v8::Value>& args) {
    auto* isolate = args.GetIsolate();
    auto* wrap = node::ObjectWrap::Unwrap<Archive>(args.Holder());

    uint64_t offset;
    if (!gin::ConvertFromV8(isolate, args[0], &offset)) {
      args.GetReturnValue().Set(v8::False(isolate));
      return;
    }

    uint64_t length;
    if (!gin::ConvertFromV8(isolate, args[1], &length)) {
      args.GetReturnValue().Set(v8::False(isolate));
      return;
    }

    gin_helper::Promise<v8::Local<v8::ArrayBuffer>> promise(isolate);
    v8::Local<v8::Promise> handle = promise.GetHandle();

    base::CheckedNumeric<uint64_t> safe_offset(offset);
    base::CheckedNumeric<uint64_t> safe_end = safe_offset + length;
    if (!safe_end.IsValid() ||
        safe_end.ValueOrDie() > wrap->archive_->length()) {
      promise.RejectWithErrorMessage("Out of bounds read");
      args.GetReturnValue().Set(handle);
      return;
    }

    auto backing_store = v8::ArrayBuffer::NewBackingStore(isolate, length);
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(&Archive::ReadOnIO, isolate, wrap->archive_,
                       std::move(backing_store), offset, length),
        base::BindOnce(&Archive::ResolveReadOnUI, std::move(promise)));

    args.GetReturnValue().Set(handle);
  }

  static std::unique_ptr<v8::BackingStore> ReadOnIO(
      v8::Isolate* isolate,
      std::shared_ptr<asar::Archive> archive,
      std::unique_ptr<v8::BackingStore> backing_store,
      uint64_t offset,
      uint64_t length) {
    memcpy(backing_store->Data(), archive->data() + offset, length);
    return backing_store;
  }

  static void ResolveReadOnUI(
      gin_helper::Promise<v8::Local<v8::ArrayBuffer>> promise,
      std::unique_ptr<v8::BackingStore> backing_store) {
    v8::HandleScope scope(promise.isolate());
    v8::Context::Scope context_scope(promise.GetContext());
    auto array_buffer =
        v8::ArrayBuffer::New(promise.isolate(), std::move(backing_store));
    promise.Resolve(array_buffer);
  }

  std::shared_ptr<asar::Archive> archive_;
};

static void InitAsarSupport(const v8::FunctionCallbackInfo<v8::Value>& args) {
  auto* isolate = args.GetIsolate();
  auto require = args[0];

  // Evaluate asar_bundle.js.
  std::vector<v8::Local<v8::String>> asar_bundle_params = {
      node::FIXED_ONE_BYTE_STRING(isolate, "require")};
  std::vector<v8::Local<v8::Value>> asar_bundle_args = {require};
  electron::util::CompileAndCall(
      isolate->GetCurrentContext(), "electron/js2c/asar_bundle",
      &asar_bundle_params, &asar_bundle_args, nullptr);
}

static void SplitPath(const v8::FunctionCallbackInfo<v8::Value>& args) {
  auto* isolate = args.GetIsolate();

  base::FilePath path;
  if (!gin::ConvertFromV8(isolate, args[0], &path)) {
    args.GetReturnValue().Set(v8::False(isolate));
    return;
  }

  gin_helper::Dictionary dict = gin::Dictionary::CreateEmpty(isolate);
  base::FilePath asar_path, file_path;
  if (asar::GetAsarArchivePath(path, &asar_path, &file_path, true)) {
    dict.Set("isAsar", true);
    dict.Set("asarPath", asar_path);
    dict.Set("filePath", file_path);
  } else {
    dict.Set("isAsar", false);
  }
  args.GetReturnValue().Set(dict.GetHandle());
}

void Initialize(v8::Local<v8::Object> exports,
                v8::Local<v8::Value> unused,
                v8::Local<v8::Context> context,
                void* priv) {
  auto* isolate = exports->GetIsolate();

  auto cons = Archive::CreateFunctionTemplate(isolate)
                  ->GetFunction(context)
                  .ToLocalChecked();
  cons->SetName(node::FIXED_ONE_BYTE_STRING(isolate, "Archive"));

  exports->Set(context, node::FIXED_ONE_BYTE_STRING(isolate, "Archive"), cons)
      .Check();
  NODE_SET_METHOD(exports, "splitPath", &SplitPath);
  NODE_SET_METHOD(exports, "initAsarSupport", &InitAsarSupport);
}

}  // namespace

NODE_LINKED_MODULE_CONTEXT_AWARE(electron_common_asar, Initialize)
