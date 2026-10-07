#include "naming.hpp"

#include <openssl/evp.h>

#include <array>
#include <cstddef>
#include <string>

namespace araya::mcp {
namespace {

// The DeepSeek function-name contract: at most 64 characters,
// `[A-Za-z0-9_-]`. Wire constants, not configuration.
constexpr std::size_t max_public_name_length = 64;
constexpr std::size_t hash_length = 12;

bool invalid_name_char(char c) {
	return !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-');
}

std::string sha256_hex(std::string_view data) {
	std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
	unsigned int length = 0;
	EVP_MD_CTX* ctx = EVP_MD_CTX_new();
	EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
	EVP_DigestUpdate(ctx, data.data(), data.size());
	EVP_DigestFinal_ex(ctx, digest.data(), &length);
	EVP_MD_CTX_free(ctx);
	static constexpr char hex[] = "0123456789abcdef";
	std::string out;
	out.reserve(length * 2);
	for (unsigned int i = 0; i < length; ++i) {
		out.push_back(hex[digest[i] >> 4]);
		out.push_back(hex[digest[i] & 0x0F]);
	}
	return out;
}

} // namespace

std::string public_tool_name(std::string_view server, std::string_view raw_name) {
	std::string joined = "mcp__" + std::string(server) + "__" + std::string(raw_name);
	std::string normalized = joined;
	for (char& c : normalized) {
		if (invalid_name_char(c))
			c = '_';
	}
	if (normalized == joined && normalized.size() <= max_public_name_length)
		return normalized;
	std::string identity = std::string(server) + '\0' + std::string(raw_name);
	std::string const hash = sha256_hex(identity).substr(0, hash_length);
	std::size_t const keep = max_public_name_length - hash_length - 1;
	return normalized.substr(0, keep) + "_" + hash;
}

} // namespace araya::mcp
