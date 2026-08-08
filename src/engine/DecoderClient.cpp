#include "engine/DecoderClient.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <filesystem>

#include "util/Env.h"

extern char** environ;

namespace microbrowser::engine {

namespace {

bool SetNonBlocking(int descriptor) {
  if (descriptor < 0) {
    return false;
  }
  const int flags = ::fcntl(descriptor, F_GETFL, 0);
  if (flags < 0) {
    return false;
  }
  return ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0;
}

}  // namespace

DecoderClient::DecoderClient() = default;

DecoderClient::~DecoderClient() { StopChild("destroyed"); }

std::string DecoderClient::FindDecoderBinary() {
  if (const char* env = util::EnvValue("MICROBROWSER_DECODER")) {
    return env;
  }
  std::array<char, PATH_MAX> buffer{};
  const ssize_t length = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
  if (length <= 0) {
    return "microbrowser_decoder";
  }
  buffer[static_cast<std::size_t>(length)] = '\0';
  const std::filesystem::path self(buffer.data());
  const std::filesystem::path sibling = self.parent_path() / "microbrowser_decoder";
  return sibling.string();
}

bool DecoderClient::Start() {
  if (child_pid_ > 0) {
    return true;
  }
  if (binary_path_.empty()) {
    binary_path_ = FindDecoderBinary();
  }
  int to_child[2] = {-1, -1};
  int from_child[2] = {-1, -1};
  if (::pipe2(to_child, O_CLOEXEC) != 0 || ::pipe2(from_child, O_CLOEXEC) != 0) {
    return false;
  }

  posix_spawn_file_actions_t actions;
  if (posix_spawn_file_actions_init(&actions) != 0) {
    ::close(to_child[0]);
    ::close(to_child[1]);
    ::close(from_child[0]);
    ::close(from_child[1]);
    return false;
  }
  (void)posix_spawn_file_actions_addclose(&actions, to_child[1]);
  (void)posix_spawn_file_actions_addclose(&actions, from_child[0]);
  (void)posix_spawn_file_actions_adddup2(&actions, to_child[0], STDIN_FILENO);
  (void)posix_spawn_file_actions_adddup2(&actions, from_child[1], STDOUT_FILENO);
  (void)posix_spawn_file_actions_addclose(&actions, to_child[0]);
  (void)posix_spawn_file_actions_addclose(&actions, from_child[1]);

  const std::string path = binary_path_;
  std::vector<char> argv_storage(path.begin(), path.end());
  argv_storage.push_back('\0');
  char* argv[] = {argv_storage.data(), nullptr};

  pid_t pid = -1;
  const int spawn_result =
      posix_spawn(&pid, path.c_str(), &actions, nullptr, argv, environ);
  posix_spawn_file_actions_destroy(&actions);
  ::close(to_child[0]);
  ::close(from_child[1]);

  if (spawn_result != 0) {
    ::close(to_child[1]);
    ::close(from_child[0]);
    return false;
  }

  to_child_ = to_child[1];
  from_child_ = from_child[0];
  child_pid_ = pid;
  configured_ = false;
  read_buffer_.clear();
  (void)SetNonBlocking(from_child_);
  return true;
}

bool DecoderClient::WriteMessage(std::span<const std::uint8_t> bytes) {
  if (to_child_ < 0) {
    return false;
  }
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t written = ::write(to_child_, bytes.data() + offset, bytes.size() - offset);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      StopChild("write");
      return false;
    }
    offset += static_cast<std::size_t>(written);
  }
  return true;
}

bool DecoderClient::ReadMore() {
  if (from_child_ < 0) {
    return false;
  }
  std::array<std::uint8_t, 4096> chunk{};
  while (true) {
    const ssize_t read_count = ::read(from_child_, chunk.data(), chunk.size());
    if (read_count < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return true;
      }
      StopChild("read");
      return false;
    }
    if (read_count == 0) {
      StopChild("eof");
      return false;
    }
    read_buffer_.insert(read_buffer_.end(), chunk.begin(), chunk.begin() + read_count);
    if (static_cast<std::size_t>(read_count) < chunk.size()) {
      break;
    }
  }
  return true;
}

void DecoderClient::StopChild(const char* reason) {
  (void)reason;
  if (to_child_ >= 0) {
    ::close(to_child_);
    to_child_ = -1;
  }
  if (from_child_ >= 0) {
    ::close(from_child_);
    from_child_ = -1;
  }
  if (child_pid_ > 0) {
    int status = 0;
    (void)::waitpid(child_pid_, &status, WNOHANG);
    child_pid_ = -1;
  }
  configured_ = false;
}

bool DecoderClient::Configure(ipc::WireCodec codec, std::span<const std::uint8_t> extra_data) {
  if (!Start()) {
    return false;
  }
  ipc::ConfigureMessage message;
  message.codec = codec;
  message.extra_data.assign(extra_data.begin(), extra_data.end());
  if (!WriteMessage(ipc::EncodeConfigure(message))) {
    return false;
  }
  configured_ = true;
  return true;
}

bool DecoderClient::PushSample(std::int64_t timestamp_us, bool is_sync,
                               std::span<const std::uint8_t> bytes) {
  if (!configured_) {
    return false;
  }
  ipc::SampleMessage message;
  message.timestamp_us = timestamp_us;
  message.is_sync = is_sync;
  message.bytes.assign(bytes.begin(), bytes.end());
  return WriteMessage(ipc::EncodeSample(message));
}

bool DecoderClient::Flush() {
  if (!configured_) {
    return false;
  }
  return WriteMessage(ipc::EncodeFlush());
}

std::vector<ipc::FrameMessage> DecoderClient::PollFrames(std::string* error_out) {
  std::vector<ipc::FrameMessage> frames;
  if (from_child_ < 0) {
    return frames;
  }
  (void)ReadMore();
  while (true) {
    const ipc::DecoderDecodeResult decoded = ipc::DecodeDecoderMessage(read_buffer_);
    if (decoded.status == ipc::DecoderDecode::Incomplete) {
      break;
    }
    if (decoded.status == ipc::DecoderDecode::Failed) {
      if (error_out != nullptr) {
        *error_out = "malformed";
      }
      StopChild("malformed");
      break;
    }
    read_buffer_.erase(read_buffer_.begin(),
                       read_buffer_.begin() + static_cast<std::ptrdiff_t>(decoded.consumed));
    if (decoded.message.kind == ipc::DecoderMessageKind::Frame) {
      frames.push_back(std::move(decoded.message.frame));
      continue;
    }
    if (decoded.message.kind == ipc::DecoderMessageKind::Error) {
      if (error_out != nullptr) {
        *error_out = decoded.message.error.reason;
      }
      StopChild("error");
      break;
    }
  }
  return frames;
}

bool DecoderClient::ChildExited(std::string* reason_out) {
  if (child_pid_ <= 0) {
    return true;
  }
  int status = 0;
  const pid_t waited = ::waitpid(child_pid_, &status, WNOHANG);
  if (waited == 0) {
    return false;
  }
  if (reason_out != nullptr) {
    if (WIFEXITED(status)) {
      *reason_out = "exit";
    } else if (WIFSIGNALED(status)) {
      *reason_out = "signal";
    } else {
      *reason_out = "wait";
    }
  }
  StopChild("exited");
  return true;
}

std::optional<util::WaitDescriptor> DecoderClient::Interest() const {
  if (from_child_ < 0) {
    return std::nullopt;
  }
  util::WaitDescriptor descriptor;
  descriptor.descriptor = from_child_;
  descriptor.readable = true;
  return descriptor;
}

}  // namespace microbrowser::engine
