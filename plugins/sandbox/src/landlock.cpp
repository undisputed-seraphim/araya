#include "araya/sandbox/sandbox.hpp"

#include "landlock.hpp"

#include <boost/system/error_code.hpp>

#include <cerrno>
#include <cstring>

#ifdef ARAYA_HAVE_LANDLOCK
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef __NR_landlock_create_ruleset
#define __NR_landlock_create_ruleset 444
#define __NR_landlock_add_rule 445
#define __NR_landlock_restrict_self 446
#endif
#endif

namespace araya::sandbox {
namespace detail {
namespace {

boost::system::error_code errno_code(int code) { return {code == 0 ? ENOSYS : code, boost::system::system_category()}; }

#ifdef ARAYA_HAVE_LANDLOCK

// The Landlock UAPI, self-defined so the build does not depend on the
// toolchain's <linux/landlock.h> vintage. Layouts are verbatim from the
// kernel header (the path-beneath struct is packed there, so it is here).
struct ruleset_attr {
	std::uint64_t handled_access_fs;
};

struct path_beneath_attr {
	std::uint64_t allowed_access;
	std::int32_t parent_fd;
} __attribute__((packed));

constexpr std::uint32_t ll_create_ruleset_version = 1U << 0;
constexpr std::uint32_t ll_rule_path_beneath = 1;

constexpr std::uint64_t ll_execute = 1ULL << 0;
constexpr std::uint64_t ll_write_file = 1ULL << 1;
constexpr std::uint64_t ll_read_file = 1ULL << 2;
constexpr std::uint64_t ll_read_dir = 1ULL << 3;
constexpr std::uint64_t ll_remove_dir = 1ULL << 4;
constexpr std::uint64_t ll_remove_file = 1ULL << 5;
constexpr std::uint64_t ll_make_char = 1ULL << 6;
constexpr std::uint64_t ll_make_dir = 1ULL << 7;
constexpr std::uint64_t ll_make_reg = 1ULL << 8;
constexpr std::uint64_t ll_make_sock = 1ULL << 9;
constexpr std::uint64_t ll_make_fifo = 1ULL << 10;
constexpr std::uint64_t ll_make_block = 1ULL << 11;
constexpr std::uint64_t ll_make_sym = 1ULL << 12;
constexpr std::uint64_t ll_refer = 1ULL << 13;	   // ABI 2
constexpr std::uint64_t ll_truncate = 1ULL << 14;  // ABI 3
constexpr std::uint64_t ll_ioctl_dev = 1ULL << 15; // ABI 5

// Every ABI-1 access (bits 0..12).
constexpr std::uint64_t ll_abi1_mask = (1ULL << 13) - 1;

std::uint64_t fs_mask_for_abi(long abi) {
	std::uint64_t mask = ll_abi1_mask;
	if (abi >= 2)
		mask |= ll_refer;
	if (abi >= 3)
		mask |= ll_truncate;
	if (abi >= 5)
		mask |= ll_ioctl_dev;
	return mask;
}

// The file-appropriate subset of the full mask. Landlock rejects the
// directory-only operations (MAKE_*, REMOVE_*) on a non-directory grant
// (e.g. /dev/null) with EINVAL, so each grant is narrowed to its type.
std::uint64_t file_mask_for_abi(long abi) {
	std::uint64_t mask = ll_read_file | ll_write_file;
	if (abi >= 3)
		mask |= ll_truncate;
	if (abi >= 5)
		mask |= ll_ioctl_dev;
	return mask;
}

// The read+execute subset valid for a directory or a file.
std::uint64_t read_mask_for(bool directory) {
	return directory ? (ll_execute | ll_read_file | ll_read_dir) : (ll_execute | ll_read_file);
}

bool is_directory(std::string const& path) {
	struct stat info{};
	return ::stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

// Add one path-beneath rule granting `access` beneath `path`.
int add_rule(int ruleset_fd, std::string const& path, std::uint64_t access) {
	int path_fd = ::open(path.c_str(), O_PATH | O_CLOEXEC);
	if (path_fd < 0)
		return errno;
	path_beneath_attr rule{access, path_fd};
	long const added = ::syscall(__NR_landlock_add_rule, ruleset_fd, ll_rule_path_beneath, &rule, 0);
	int const error = added < 0 ? errno : 0;
	::close(path_fd);
	return error;
}

#endif // ARAYA_HAVE_LANDLOCK

} // namespace

boost::system::error_code
apply_landlock(std::vector<std::string> const& read_only, std::vector<std::string> const& read_write) noexcept {
#ifdef ARAYA_HAVE_LANDLOCK
	errno = 0;
	long const abi = ::syscall(__NR_landlock_create_ruleset, nullptr, 0, ll_create_ruleset_version);
	if (abi < 1)
		return errno_code(errno);

	std::uint64_t const mask = fs_mask_for_abi(abi);
	ruleset_attr const attr{mask};
	int const ruleset_fd = static_cast<int>(::syscall(__NR_landlock_create_ruleset, &attr, sizeof(attr), 0));
	if (ruleset_fd < 0)
		return errno_code(errno);

	for (auto const& path : read_only) {
		if (int const error = add_rule(ruleset_fd, path, read_mask_for(is_directory(path)))) {
			::close(ruleset_fd);
			return errno_code(error);
		}
	}
	for (auto const& path : read_write) {
		std::uint64_t const access = is_directory(path) ? mask : file_mask_for_abi(abi);
		if (int const error = add_rule(ruleset_fd, path, access)) {
			::close(ruleset_fd);
			return errno_code(error);
		}
	}

	if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
		int const error = errno;
		::close(ruleset_fd);
		return errno_code(error);
	}
	if (::syscall(__NR_landlock_restrict_self, ruleset_fd, 0) != 0) {
		int const error = errno;
		::close(ruleset_fd);
		return errno_code(error);
	}
	::close(ruleset_fd);
	return {};
#else
	(void)read_only;
	(void)read_write;
	return errno_code(ENOTSUP);
#endif
}

probe_result probe_landlock() noexcept {
#ifdef ARAYA_HAVE_LANDLOCK
	errno = 0;
	long const abi = ::syscall(__NR_landlock_create_ruleset, nullptr, 0, ll_create_ruleset_version);
	if (abi < 1) {
		std::string reason = "the kernel does not support Landlock";
		if (errno != 0)
			reason += std::string(": ") + std::strerror(errno);
		return probe_result{false, 0, std::move(reason)};
	}
	return probe_result{true, static_cast<std::uint32_t>(abi), {}};
#else
	return probe_result{false, 0, "Landlock support was not compiled in"};
#endif
}

} // namespace detail
} // namespace araya::sandbox
