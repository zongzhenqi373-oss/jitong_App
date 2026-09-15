#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "client_core/dto/Dtos.h"
#include "client_core/storage/NativeDatabase.h"

namespace im { namespace search {
struct SearchQuery {
    std::int64_t ownerId=0;
    std::int64_t conversationId=0; // 0 means all conversations
    std::string keyword;
    int limit=100;
};

class SearchService {
public:
    explicit SearchService(im::storage::NativeDatabase* db):db_(db){}
    bool search(const SearchQuery& query,std::vector<im::dto::SearchHit>* hits,
                std::string* error=nullptr) const;
private:
    im::storage::NativeDatabase* db_;
};
} }
