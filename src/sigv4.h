#pragma once
#include <string>
#include <utility>
#include <vector>

// AWS Signature Version 4 (pure; unit-tested against the examples in AWS' documentation).
// S3 and every S3-compatible store (MinIO, Cloudflare R2, Wasabi, Backblaze B2...) accept it.
namespace sigv4 {

struct Credentials { std::string accessKey, secretKey, sessionToken; };

using Headers = std::vector<std::pair<std::string, std::string>>;

struct Request {
    std::string method;               // "GET"
    std::string host;                 // "examplebucket.s3.amazonaws.com" (with ":port" when not the default)
    std::string path;                 // already URI-encoded, "/" at least
    std::vector<std::pair<std::string, std::string>> query;   // unencoded names and values
    Headers headers;                  // besides host / x-amz-date / x-amz-content-sha256 (added)
    std::string payloadHash;          // hex SHA-256 of the body, or "UNSIGNED-PAYLOAD"
};

// adds x-amz-date, x-amz-content-sha256 (and x-amz-security-token) and Authorization to
// r.headers; amzDate "yyyymmddThhmmssZ" ("" = now)
void sign(Request& r, const Credentials& c, const std::string& region, const std::string& service, std::string amzDate = {});

// the pieces (tests)
std::string canonical_query(const std::vector<std::pair<std::string, std::string>>& q);
std::string canonical_request(const Request& r, std::string& signedHeaders);
std::string signature(const std::string& canonicalRequest, const Credentials& c, const std::string& region, const std::string& service,
                      const std::string& amzDate);
std::string uri_encode(const std::string& s, bool keepSlash);
std::string now_amz_date();
std::string sha256_hex(const std::string& data);

} // namespace sigv4
