#include "client_core/search/SearchService.h"
#include "client_core/storage/NativeRepository.h"
#include <iostream>
#include <cstdlib>
using namespace im::storage; using namespace im::search; using im::dto::MessageDto;
int main(){
 const char* dir="/tmp/test_search_service"; ::system("rm -rf /tmp/test_search_service; mkdir -p /tmp/test_search_service");
 NativeDatabase db; std::string err; int fail=0; auto ck=[&](bool v,const char*n){if(!v){std::cerr<<n<<" "<<err<<'\n';++fail;}};
 ck(db.open(dir,1,std::vector<unsigned char>(32,0x71),&err),"open"); NativeRepository repo(&db);
 long long nextSeq=1;
 auto add=[&](const char* id,const char* text,const char* py,const char* initials,long long conv=10){MessageDto m;m.ownerId=1;m.msgId=id;m.conversationId=conv;m.peerId=conv;m.seq=nextSeq++;m.ts=m.seq;m.type=0;m.content=text;m.pinyin=py;m.initials=initials;return repo.commitIncomingMessage(m,nullptr,&err);};
 ck(add("you","哈哈，你有私藏的好剧吗","haha ni you sicang de haoju ma","hhnyscdhjm"),"add you");
 ck(add("truth","真正在这里","zhen zheng zai zheli","zzzjzl"),"add truth");
 ck(add("emoji","😀你好","nihao","nh",20),"add emoji");
 SearchService svc(&db); std::vector<im::dto::SearchHit> hits;
 ck(svc.search({1,0,"n",100},&hits,&err)&&hits.size()==2,"n matches initials n, not zhen/zheng");
 bool hasTruth=false;for(auto&h:hits)if(h.msgId=="truth")hasTruth=true;ck(!hasTruth,"no false truth");
 bool mapped=false;for(auto&h:hits)if(h.msgId=="you"&&h.highlightRanges.size()==1&&
   h.highlightRanges[0].first==3&&h.highlightRanges[0].second==4)mapped=true;
 ck(mapped,"pinyin initial maps to source UTF16 range");
 ck(svc.search({1,10,"ni",100},&hits,&err)&&hits.size()==1&&hits[0].msgId=="you","full pinyin scoped");
 ck(svc.search({1,20,"你",100},&hits,&err)&&hits.size()==1&&hits[0].highlightRanges.size()==1&&hits[0].highlightRanges[0].first==2,"UTF16 emoji offset");
 ck(!svc.search({1,0,"",100},&hits,&err),"empty rejected");
 ck(svc.search({2,0,"n",100},&hits,&err)&&hits.empty(),"owner isolation");
 db.close(); ::system("rm -rf /tmp/test_search_service"); return fail?1:0;
}
