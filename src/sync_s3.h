#pragma once
#include "sigv4.h"
#include "sync_backend.h"

// Amazon S3 and compatible stores (docs/sync-plan.md, "S3"): ListObjectsV2 listings,
// conditional PUTs (If-None-Match / If-Match) for the engine's "only if unchanged", server
// copies for moves, range reads. No change feed: the encrypting layer's head marker is
// the cheap poll.
namespace sync {

struct S3Options {
    std::wstring endpoint;                    // "" = https://s3.<region>.amazonaws.com
    std::string region = "us-east-1", bucket, prefix;
    sigv4::Credentials cred;
    bool pathStyle = false;                   // https://host/bucket/key (MinIO...) instead of https://bucket.host/key
};
std::unique_ptr<Backend> make_s3_backend(S3Options o);

// the XML S3 answers with (pure, tested)
struct S3Object { std::string key, etag; uint64_t size = 0; };
bool parse_list_objects(const std::string& xml, std::vector<S3Object>& out, bool& truncated, std::string& next);
std::string xml_text(const std::string& xml, const std::string& tag, size_t from = 0, size_t* end = nullptr);
std::string xml_unescape(const std::string& s);

} // namespace sync
