#include "client_core/search/SearchService.h"
#include <sqlite3.h>
#include <algorithm>
#include <cctype>

namespace im { namespace search { namespace {
std::string trimLower(std::string s){
    auto space=[](unsigned char c){return std::isspace(c)!=0;};
    s.erase(s.begin(),std::find_if_not(s.begin(),s.end(),space));
    s.erase(std::find_if_not(s.rbegin(),s.rend(),space).base(),s.end());
    for(char& c:s) if(static_cast<unsigned char>(c)<128)c=static_cast<char>(std::tolower(c));
    return s;
}
// Convert byte offsets in valid UTF-8 into Android String (UTF-16 code unit) offsets.
int utf16Units(const std::string& s,std::size_t bytes){
    int units=0;
    for(std::size_t i=0;i<bytes;){
        const unsigned char c=s[i]; std::size_t n=c<0x80?1:(c<0xE0?2:(c<0xF0?3:4));
        if(i+n>bytes) break;
        units += n==4?2:1; i+=n;
    }
    return units;
}
void addContentRanges(const std::string& text,const std::string& keyword,im::dto::SearchHit& hit){
    std::string lower=text;
    for(char& c:lower)if(static_cast<unsigned char>(c)<128)c=static_cast<char>(std::tolower(c));
    for(std::size_t at=lower.find(keyword);at!=std::string::npos;at=lower.find(keyword,at+1)){
        hit.highlightRanges.push_back({utf16Units(text,at),utf16Units(text,at+keyword.size())});
    }
}
bool nextCodePoint(const std::string& s,std::size_t& at,std::uint32_t& cp,int& utf16){
    if(at>=s.size())return false;
    const unsigned char c=static_cast<unsigned char>(s[at]); std::size_t n=1; cp=c;
    if((c&0xE0)==0xC0){n=2;cp=c&0x1F;} else if((c&0xF0)==0xE0){n=3;cp=c&0x0F;}
    else if((c&0xF8)==0xF0){n=4;cp=c&0x07;}
    if(at+n>s.size())return false;
    for(std::size_t i=1;i<n;++i){const auto x=static_cast<unsigned char>(s[at+i]);if((x&0xC0)!=0x80)return false;cp=(cp<<6)|(x&0x3F);}
    at+=n; utf16=cp>0xFFFF?2:1; return true;
}
bool indexable(std::uint32_t cp){
    if(cp<128)return std::isalnum(static_cast<unsigned char>(cp))!=0;
    return (cp>=0x3400&&cp<=0x4DBF)||(cp>=0x4E00&&cp<=0x9FFF)||(cp>=0xF900&&cp<=0xFAFF);
}
void addInitialRanges(const std::string& text,const std::string& initials,const std::string& keyword,
                      im::dto::SearchHit& hit){
    std::vector<std::pair<int,int>> source; std::size_t at=0; int units=0;
    while(at<text.size()){
        std::uint32_t cp=0; int width=0; if(!nextCodePoint(text,at,cp,width))return;
        if(indexable(cp))source.push_back({units,units+width}); units+=width;
    }
    std::string lower=initials;
    for(char& c:lower)if(static_cast<unsigned char>(c)<128)c=static_cast<char>(std::tolower(c));
    if(lower.size()!=source.size())return;
    for(std::size_t pos=lower.find(keyword);pos!=std::string::npos;pos=lower.find(keyword,pos+1)){
        const auto end=pos+keyword.size()-1;
        if(end<source.size())hit.highlightRanges.push_back({source[pos].first,source[end].second});
    }
}
void normalizeRanges(im::dto::SearchHit& hit){
    std::sort(hit.highlightRanges.begin(),hit.highlightRanges.end());
    hit.highlightRanges.erase(std::unique(hit.highlightRanges.begin(),hit.highlightRanges.end()),hit.highlightRanges.end());
}
} // namespace

bool SearchService::search(const SearchQuery& input,std::vector<im::dto::SearchHit>* hits,
                           std::string* error) const {
    if(hits)hits->clear();
    const auto keyword=trimLower(input.keyword);
    if(!db_||!hits||input.ownerId<=0||keyword.empty()||input.limit<=0||input.limit>100){
        if(error)*error="invalid search query"; return false;
    }
    bool queryOk=false;
    const auto result=db_->withRead([&](sqlite3* db){
        sqlite3_stmt* s=nullptr;
        // Identity table supplies account isolation absent from the contentless FTS row.
        // A single ASCII letter is matched only against initials, avoiding full-pinyin
        // substring false positives (e.g. n must not match zheng/zhen).
        const bool oneInitial=keyword.size()==1 && keyword[0]>='a'&&keyword[0]<='z';
        std::string sql=
          "SELECT m.msg_id,m.conversation_id,m.peer_id,m.server_time,m.content,f.initials "
          "FROM message_fts_identity f JOIN messages m ON m.owner_id=f.owner_id AND m.msg_id=f.msg_id "
          "WHERE m.owner_id=?1 AND (?2=0 OR m.conversation_id=?2) AND m.type=0 AND (";
        sql += oneInitial ? "instr(lower(f.initials),?3)>0" :
          "instr(lower(m.content),?3)>0 OR instr(lower(f.pinyin),?3)>0 OR instr(lower(f.initials),?3)>0";
        sql += ") ORDER BY m.server_time DESC,m.conversation_seq DESC,m.local_order DESC,m.msg_id DESC LIMIT ?4";
        if(sqlite3_prepare_v2(db,sql.c_str(),-1,&s,nullptr)!=SQLITE_OK)return;
        sqlite3_bind_int64(s,1,input.ownerId); sqlite3_bind_int64(s,2,input.conversationId);
        sqlite3_bind_text(s,3,keyword.data(),static_cast<int>(keyword.size()),SQLITE_TRANSIENT);
        sqlite3_bind_int(s,4,input.limit);
        int rc;
        while((rc=sqlite3_step(s))==SQLITE_ROW){
            auto text=[&](int i){const auto* p=sqlite3_column_text(s,i);return p?std::string(reinterpret_cast<const char*>(p),sqlite3_column_bytes(s,i)):std::string();};
            im::dto::SearchHit h; h.msgId=text(0); h.conversationId=sqlite3_column_int64(s,1);
            h.peerId=sqlite3_column_int64(s,2); h.ts=sqlite3_column_int64(s,3); h.snippet=text(4);
            h.highlightUnit=im::dto::SearchHit::HighlightUnit::Utf16;
            addContentRanges(h.snippet,keyword,h);
            addInitialRanges(h.snippet,text(5),keyword,h);
            normalizeRanges(h);
            hits->push_back(std::move(h));
        }
        queryOk=rc==SQLITE_DONE; sqlite3_finalize(s);
    });
    if(result!=im::storage::ReadResult::Ok||!queryOk){if(error)*error="search database unavailable";hits->clear();return false;}
    return true;
}
} }
