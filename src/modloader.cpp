#include "modloader.hpp"

#include <dlfcn.h>
#include <sys/stat.h>

#include <filesystem>
#include <optional>
#include <system_error>

#include "fileutils.hpp"
#include "log.hpp"

namespace fs = std::filesystem;

namespace {
// Get status type as string
const char* status_type(fs::file_type const type) {
  switch (type) {
    case fs::file_type::none:
      return "none";
    case fs::file_type::not_found:
      return "not found";
    case fs::file_type::regular:
      return "regular";
    case fs::file_type::directory:
      return "directory";
    case fs::file_type::symlink:
      return "symlink";
    case fs::file_type::block:
      return "block";
    case fs::file_type::character:
      return "character";
    case fs::file_type::fifo:
      return "fifo";
    case fs::file_type::socket:
      return "socket";
    case fs::file_type::unknown:
    default:
      return "unknown";
  }
}

// Get stats and status of file
#if defined(__aarch64__) && !defined(NO_STAT_DUMPS)
void statdump(fs::path const& path) {
  struct stat64 buffer {};
  if (stat64(path.c_str(), &buffer) != 0) {
    LOG_DEBUG("stat64 of file: %s failed: %d, %s", path.c_str(), errno, strerror(errno));
  } else {
    LOG_DEBUG(
        "file: %s, dev: %lu, ino: %lu, mode: %d, nlink: %d, uid: %d, gid: %d, rdid: %lu, sz: %ld, atime: %ld, "
        "mtime: %ld, ctime: %ld",
        path.c_str(), buffer.st_dev, buffer.st_ino, buffer.st_mode, buffer.st_nlink, buffer.st_uid, buffer.st_gid,
        buffer.st_rdev, buffer.st_size, buffer.st_atime, buffer.st_mtime, buffer.st_ctime);
    std::error_code err{};
    auto status = fs::status(path, err);
    if (err) {
      LOG_ERROR("Failed to get status of path: %s", path.c_str());
      return;
    }
    LOG_DEBUG("File: %s, type: %s, perms: 0x%x", path.c_str(), status_type(status.type()), (unsigned int)status.permissions());
  }
}
#else
void statdump([[maybe_unused]] fs::path const& path) {}
#endif

#define HANDLE_ERR(err)                                                             \
  if (err) {                                                                        \
    LOG_ERROR("Failed path operation: %d: %s", err.value(), err.message().c_str()); \
    return {};                                                                      \
  }

struct FindResult {
  void* handle;
  fs::path path;
  fs::path source;
};

[[nodiscard]] std::optional<FindResult> findAndOpenModloader(fileutils::path_container const& container) {
  auto const& path = container.modloaderSearchPath;
  LOG_DEBUG("Attempting modloader search at: %s", path.c_str());
  statdump(path);
  // Find the first .so file in the modloader directory and open it
  std::error_code err{};
  fs::directory_iterator iter(container.modloaderSearchPath, fs::directory_options::skip_permission_denied, err);
  HANDLE_ERR(err);
  for (auto const& entry : iter) {
    // First, handle an error if there is one
    HANDLE_ERR(err);
    // Next, check to see if this entry is a .so
    if (entry.is_regular_file(err) && entry.path().filename().string().ends_with(".so")) {
      // We found a viable option!
      auto src = fs::absolute(entry.path());
      statdump(src);
      auto modloaderPath = fs::absolute(container.filesDir / entry.path().filename());
      if (!fs::copy_file(src, modloaderPath, fs::copy_options::overwrite_existing, err)) {
        // Gracefully handle errors on copy + overwrite of modloader candidate
        LOG_WARN("findAndOpenModloader: Failed to copy %s to %s (error: %s)", src.c_str(), modloaderPath.c_str(),
                 err.message().c_str());
        err.clear();
        // Try other candidates
        continue;
      }
      // Ensure destination file has correct permissions
      auto flags = S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IXGRP | S_IWGRP | S_IROTH | S_IXOTH;
      if (chmod(modloaderPath.c_str(), flags) < 0) {
        LOG_WARN("Failed to chmod %s with flags: %x! errno: %d, %s", modloaderPath.c_str(), flags, errno,
                 strerror(errno));
        continue;
      }
      LOG_VERBOSE("Stat dump before dlopen...");
      statdump(modloaderPath);
      // Finally, try to dlopen
      auto modloaderHandle = dlopen(modloaderPath.c_str(), RTLD_LAZY | RTLD_GLOBAL);
      if (modloaderHandle == nullptr) {
        LOG_WARN("preload: Could not load %s: %s", modloaderPath.c_str(), dlerror());
        continue;
      }
      // If everything worked, return
      return FindResult{
          .handle = modloaderHandle,
          .path = modloaderPath,
          .source = src,
      };
    }
    if (err) {
      // Gracefully handle errors where we cannot check if the file is valid by skipping
      LOG_WARN("Failed to check if file was valid: %s (error: %s)", entry.path().c_str(), err.message().c_str());
      err.clear();
    }
  }
  // At this point, we had NO candidates for a modloader in our modloader directory.
  // Exit gracefully and continue game load
  LOG_WARN("Found no candidate modloaders in: %s", path.c_str());
  return {};
}

}  // namespace

MAIN_LOCAL void* modloader::modloaderHandle = nullptr;

MAIN_LOCAL void modloader::preload(JNIEnv* env) noexcept {
  LOG_VERBOSE("preload: Getting application id");
  auto applicationId = fileutils::getApplicationId();
  if (!applicationId) [[unlikely]] {
    LOG_WARN("preload: Failed to get ApplicationId");
    return;
  }
  LOG_VERBOSE("preload: Application ID: %s", applicationId->c_str());
  LOG_VERBOSE("preload: Attempting to acquire activity and permissions");
  jobject activity = fileutils::getActivityFromUnityPlayer(env);
  if (!activity) [[unlikely]] {
    LOG_WARN("preload: Failed to find UnityPlayer activity!");
  } else {
    fileutils::ensurePerms(env, activity, *applicationId);
  }
  LOG_VERBOSE("preload: Attempting to find dirs");

  auto pathContainer = fileutils::getDirs(env, *applicationId);
  if (!pathContainer) {
    LOG_WARN("preload: Failed to get paths! Stopping load.");
    return;
  }
  LOG_DEBUG("Found: files: %s, external: %s", pathContainer->filesDir.c_str(), pathContainer->externalDir.c_str());

  auto result = findAndOpenModloader(*pathContainer);
  if (!result) {
    LOG_WARN("Failed to find valid modloader, continuing load anyways...");
    return;
  }
  modloaderHandle = result->handle;
  auto const& modloaderPath = result->path;

  LOG_VERBOSE("preload: Loaded modloader: %s", modloaderPath.c_str());

  auto preload = reinterpret_cast<preload_t*>(dlsym(modloaderHandle, preloadName.data()));
  if (preload == nullptr) {
    LOG_WARN("preload: %s does not have %s: %s", preloadName.data(), modloaderPath.c_str(), dlerror());
    return;
  }

  LOG_VERBOSE("preload: Calling %s", preloadName.data());
  preload(env, applicationId->c_str(), modloaderPath.c_str(), result->source.c_str(), pathContainer->filesDir.c_str(),
          pathContainer->externalDir.c_str());

  LOG_VERBOSE("preload: Preloading done");
}

MAIN_LOCAL void modloader::load(JNIEnv* env, char const* soDir) noexcept {
  if (modloaderHandle == nullptr) {
    LOG_WARN("load: Modloader not loaded!");
    return;
  }
  auto load = reinterpret_cast<load_t*>(dlsym(modloaderHandle, loadName.data()));
  if (load == nullptr) {
    LOG_WARN("load: Failed to find %s: %s", loadName.data(), dlerror());
    return;
  }
  LOG_VERBOSE("load: Calling %s with soDir: %s", loadName.data(), soDir);
  load(env, soDir);
  LOG_VERBOSE("load: Loading done");
}

MAIN_LOCAL void modloader::accept_unity_handle(JNIEnv* env, void* unityHandle) noexcept {
  if (modloaderHandle == nullptr) {
    LOG_WARN("accept_unity_handle: Modloader not loaded!");
    return;
  }
  auto accept_unity_handle =
      reinterpret_cast<accept_unity_handle_t*>(dlsym(modloaderHandle, accept_unity_handleName.data()));
  if (accept_unity_handle == nullptr) {
    LOG_WARN("accept_unity_handle: Failed to find %s: %s", accept_unity_handleName.data(), dlerror());
    return;
  }
  LOG_VERBOSE("accept_unity_handle: Calling %s", accept_unity_handleName.data());
  accept_unity_handle(env, unityHandle);
  LOG_VERBOSE("accept_unity_handle: Init done");
}

MAIN_LOCAL void modloader::unload(JavaVM* vm) noexcept {
  if (modloaderHandle == nullptr) {
    LOG_WARN("unload: Modloader not loaded!");
    return;
  }
  auto unload = reinterpret_cast<unload_t*>(dlsym(modloaderHandle, unloadName.data()));
  if (unload == nullptr) {
    LOG_WARN("unload: Failed to find %s: %s", unloadName.data(), dlerror());
    return;
  }
  LOG_VERBOSE("unload: Calling %s", unloadName.data());
  unload(vm);
  LOG_VERBOSE("unload: Unload done");
}
